#define _CRT_SECURE_NO_WARNINGS
#include "Module.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace fm {

    static bool mem_readable(const void* ptr, SIZE_T sz = sizeof(void*))
    {
        if (!ptr) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT)  return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        return ((uintptr_t)ptr + sz) <=
            ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
    }

    static bool safe_read_ptr(void** out, const void* addr)
    {
        if (!mem_readable(addr, sizeof(void*))) return false;
        *out = *reinterpret_cast<void* const*>(addr);
        return true;
    }

    static void** vtbl_of(const void* obj)
    {
        if (!obj) return nullptr;
        void* vp = nullptr;
        if (!safe_read_ptr(&vp, obj)) return nullptr;
        if (!mem_readable(vp, 128))   return nullptr;
        return reinterpret_cast<void**>(vp);
    }

    static bool patch_vtable_slot(void** vt, size_t slot,
        void* newFn, void** oldFn)
    {
        if (!vt || !newFn) return false;
        void* cell = &vt[slot];
        DWORD oldProt = 0;
        if (!VirtualProtect(cell, sizeof(void*), PAGE_READWRITE, &oldProt))
            return false;
        if (oldFn && !*oldFn) *oldFn = vt[slot];
        vt[slot] = newFn;
        VirtualProtect(cell, sizeof(void*), oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), cell, sizeof(void*));
        return true;
    }

    static IDXGISwapChain* g_swapChain = nullptr;
    static ID3D11Device* g_device = nullptr;
    static ID3D11DeviceContext* g_context = nullptr;
    static ID3D11Texture2D* g_depthSrc = nullptr;

    static ID3D11Texture2D* g_sharedDepth = nullptr;
    static HANDLE           g_depthHandle = nullptr;
    static uint32_t         g_depthW = 0;
    static uint32_t         g_depthH = 0;
    static DXGI_FORMAT      g_depthFmt = DXGI_FORMAT_UNKNOWN;

    static ID3D11Texture2D* g_sharedColor = nullptr;
    static HANDLE           g_colorHandle = nullptr;
    static uint32_t         g_colorW = 0;
    static uint32_t         g_colorH = 0;
    static DXGI_FORMAT      g_colorFmt = DXGI_FORMAT_UNKNOWN;

    static uint32_t g_vpX = 0;
    static uint32_t g_vpY = 0;
    static uint32_t g_vpW = 0;
    static uint32_t g_vpH = 0;
    static bool     g_vpOK = false;

    typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(
        IDXGISwapChain*, UINT, UINT);

    typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(
        IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(
        ID3D11Device*,
        const D3D11_TEXTURE2D_DESC*,
        const D3D11_SUBRESOURCE_DATA*,
        ID3D11Texture2D**);

    static PFN_Present         g_origPresent = nullptr;
    static PFN_ResizeBuffers   g_origResizeBuffers = nullptr;
    static PFN_CreateTexture2D g_origCreateTexture2D = nullptr;

    static CRITICAL_SECTION g_cs;
    static volatile LONG    g_running = 0;
    static HANDLE           g_initThr = nullptr;
    static HANDLE           g_pipeThr = nullptr;
    static volatile LONG    g_pipeStop = 0;
    static HANDLE           g_pipeEvt = nullptr;
    static volatile LONG    g_pipeStarted = 0;

    static PipePayload   g_payload = {};
    static volatile LONG g_payloadNew = 0;
    static volatile LONG g_frameCount = 0;

    static DXGI_FORMAT depth_to_typeless(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
        default:                               return f;
        }
    }

    static DXGI_FORMAT strip_srgb(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:                              return f;
        }
    }

    static void release_shared_depth()
    {
        if (g_sharedDepth) { g_sharedDepth->Release(); g_sharedDepth = nullptr; }
        g_depthHandle = nullptr;
        g_depthW = 0;
        g_depthH = 0;
        g_depthFmt = DXGI_FORMAT_UNKNOWN;
    }

    static void release_shared_color()
    {
        if (g_sharedColor) { g_sharedColor->Release(); g_sharedColor = nullptr; }
        g_colorHandle = nullptr;
        g_colorW = 0;
        g_colorH = 0;
        g_colorFmt = DXGI_FORMAT_UNKNOWN;
    }

    static bool create_shared_texture(
        uint32_t          w,
        uint32_t          h,
        DXGI_FORMAT       fmt,
        UINT              bindFlags,
        ID3D11Texture2D** outTex,
        HANDLE* outHandle)
    {
        if (!g_device || !w || !h) return false;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc = { 1, 0 };
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &tex);
        if (FAILED(hr)) return false;

        IDXGIResource* res = nullptr;
        hr = tex->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
        if (FAILED(hr) || !res)
        {
            tex->Release();
            return false;
        }

        HANDLE sharedHandle = nullptr;
        hr = res->GetSharedHandle(&sharedHandle);
        res->Release();

        if (FAILED(hr) || !sharedHandle)
        {
            tex->Release();
            return false;
        }

        *outTex = tex;
        *outHandle = sharedHandle;
        return true;
    }

    HRESULT STDMETHODCALLTYPE hook_CreateTexture2D(
        ID3D11Device* self,
        const D3D11_TEXTURE2D_DESC* pDesc,
        const D3D11_SUBRESOURCE_DATA* pInitData,
        ID3D11Texture2D** ppTex)
    {
        HRESULT hr = g_origCreateTexture2D(self, pDesc, pInitData, ppTex);
        if (FAILED(hr) || !ppTex || !*ppTex || !pDesc) return hr;

        if (pDesc->Format != DEPTH_FORMAT_EXPECTED)      return hr;
        if (pDesc->BindFlags != DEPTH_BINDFLAG_EXPECTED) return hr;
        if (pDesc->SampleDesc.Count > 1)                 return hr;

        EnterCriticalSection(&g_cs);
        if (g_depthSrc) { g_depthSrc->Release(); g_depthSrc = nullptr; }
        g_depthSrc = *ppTex;
        g_depthSrc->AddRef();
        release_shared_depth();
        LeaveCriticalSection(&g_cs);

        return hr;
    }

    static DWORD WINAPI pipe_thread(LPVOID)
    {
        HANDLE writeEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!writeEvt) return 0;

        while (!InterlockedCompareExchange(&g_pipeStop, 0, 0))
        {
            HANDLE hPipe = CreateNamedPipeW(
                PIPE_NAME,
                PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                (DWORD)(sizeof(PipePayload) * 8), 0, 0, nullptr);

            if (hPipe == INVALID_HANDLE_VALUE)
            {
                Sleep(200);
                continue;
            }

            HANDLE     connEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED ovConn = {};
            ovConn.hEvent = connEvt;

            ConnectNamedPipe(hPipe, &ovConn);
            DWORD connErr = GetLastError();

            bool connected = false;
            if (connErr == ERROR_PIPE_CONNECTED)
            {
                connected = true;
            }
            else if (connErr == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(connEvt, 5000) == WAIT_OBJECT_0)
                {
                    DWORD dummy = 0;
                    connected = !!GetOverlappedResult(hPipe, &ovConn, &dummy, FALSE);
                }
                else
                {
                    CancelIo(hPipe);
                }
            }
            CloseHandle(connEvt);

            if (!connected)
            {
                CloseHandle(hPipe);
                Sleep(20);
                continue;
            }

            while (!InterlockedCompareExchange(&g_pipeStop, 0, 0))
            {
                DWORD waitRes = WaitForSingleObject(g_pipeEvt, 32);
                if (waitRes == WAIT_OBJECT_0) ResetEvent(g_pipeEvt);
                if (!InterlockedExchange(&g_payloadNew, 0)) continue;

                PipePayload p = g_payload;

                OVERLAPPED ovWrite = {};
                ovWrite.hEvent = writeEvt;
                ResetEvent(writeEvt);

                DWORD wrote = 0;
                BOOL  ok = WriteFile(hPipe, &p, (DWORD)sizeof(p), &wrote, &ovWrite);
                DWORD wErr = GetLastError();

                if (!ok && wErr == ERROR_IO_PENDING)
                {
                    if (WaitForSingleObject(writeEvt, 200) == WAIT_OBJECT_0)
                    {
                        ok = GetOverlappedResult(hPipe, &ovWrite, &wrote, FALSE);
                    }
                    else
                    {
                        CancelIo(hPipe);
                        break;
                    }
                }

                if (!ok || wrote != (DWORD)sizeof(p)) break;
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }

        CloseHandle(writeEvt);
        return 0;
    }

    HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain* self,
        UINT SyncInterval,
        UINT Flags)
    {
        if (!self) goto CALL_ORIG;

        // Автоматическое получение ID3D11Device и ID3D11DeviceContext без смещений памяти
        if (!g_device || g_swapChain != self)
        {
            EnterCriticalSection(&g_cs);
            if (g_context) { g_context->Release(); g_context = nullptr; }
            if (g_device) { g_device->Release();  g_device = nullptr; }

            g_swapChain = self;
            if (SUCCEEDED(self->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) && g_device)
            {
                g_device->GetImmediateContext(&g_context);
            }
            LeaveCriticalSection(&g_cs);
        }

        if (!g_device || !g_context) goto CALL_ORIG;

        if (InterlockedCompareExchange(&g_pipeStarted, 1, 0) == 0)
        {
            g_pipeEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_pipeThr = CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr);
        }

        {
            PipePayload p = {};
            p.frameCount = (uint32_t)InterlockedIncrement(&g_frameCount);
            uint32_t newFlags = 0;

            EnterCriticalSection(&g_cs);
            ID3D11Texture2D* depthSrc = g_depthSrc;
            if (depthSrc) depthSrc->AddRef();
            LeaveCriticalSection(&g_cs);

            uint32_t sceneW = 0;
            uint32_t sceneH = 0;

            if (depthSrc)
            {
                D3D11_TEXTURE2D_DESC sd;
                depthSrc->GetDesc(&sd);

                uint32_t    dw = sd.Width;
                uint32_t    dh = sd.Height;
                DXGI_FORMAT fmt = depth_to_typeless(sd.Format);

                sceneW = dw;
                sceneH = dh;

                if (!g_sharedDepth ||
                    g_depthW != dw ||
                    g_depthH != dh ||
                    g_depthFmt != fmt)
                {
                    release_shared_depth();
                    create_shared_texture(dw, dh, fmt,
                        D3D11_BIND_SHADER_RESOURCE,
                        &g_sharedDepth, &g_depthHandle);
                    g_depthW = dw;
                    g_depthH = dh;
                    g_depthFmt = fmt;
                }

                if (g_sharedDepth)
                {
                    g_context->CopyResource(g_sharedDepth, depthSrc);

                    p.depthHandle = (uint64_t)(uintptr_t)g_depthHandle;
                    p.depthWidth = g_depthW;
                    p.depthHeight = g_depthH;
                    p.depthFormat = (uint32_t)g_depthFmt;
                    newFlags |= FLAG_DEPTH_OK;
                }

                depthSrc->Release();
            }

            {
                ID3D11Texture2D* bb = nullptr;
                HRESULT hbb = self->GetBuffer(
                    0, __uuidof(ID3D11Texture2D), (void**)&bb);

                if (SUCCEEDED(hbb) && bb)
                {
                    D3D11_TEXTURE2D_DESC bd;
                    bb->GetDesc(&bd);

                    DXGI_FORMAT colorFmt = strip_srgb(bd.Format);
                    uint32_t targetW = bd.Width;
                    uint32_t targetH = bd.Height;

                    bool useSubregion = false;
                    if (sceneW > 0 && sceneH > 0 &&
                        (sceneW != bd.Width || sceneH != bd.Height) &&
                        sceneW <= bd.Width && sceneH <= bd.Height)
                    {
                        targetW = sceneW;
                        targetH = sceneH;
                        useSubregion = true;
                    }

                    if (!g_sharedColor ||
                        g_colorW != targetW ||
                        g_colorH != targetH ||
                        g_colorFmt != colorFmt)
                    {
                        release_shared_color();
                        create_shared_texture(targetW, targetH, colorFmt,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                            &g_sharedColor, &g_colorHandle);
                        g_colorW = targetW;
                        g_colorH = targetH;
                        g_colorFmt = colorFmt;
                    }

                    if (g_sharedColor)
                    {
                        if (useSubregion)
                        {
                            D3D11_BOX src;
                            src.left = 0;
                            src.top = 0;
                            src.right = sceneW;
                            src.bottom = sceneH;
                            src.front = 0;
                            src.back = 1;

                            g_context->CopySubresourceRegion(
                                g_sharedColor, 0,
                                0, 0, 0,
                                bb, 0, &src);
                        }
                        else
                        {
                            g_context->CopyResource(g_sharedColor, bb);
                        }

                        p.colorHandle = (uint64_t)(uintptr_t)g_colorHandle;
                        p.colorWidth = g_colorW;
                        p.colorHeight = g_colorH;
                        p.colorFormat = (uint32_t)g_colorFmt;
                        newFlags |= FLAG_COLOR_OK;

                        g_vpX = 0;
                        g_vpY = 0;
                        g_vpW = targetW;
                        g_vpH = targetH;
                        g_vpOK = true;
                    }

                    bb->Release();
                }
            }

            if (g_vpOK)
            {
                p.vpX = g_vpX;
                p.vpY = g_vpY;
                p.vpWidth = g_vpW;
                p.vpHeight = g_vpH;
                newFlags |= FLAG_VIEWPORT_OK;
            }
            else
            {
                p.vpX = 0;
                p.vpY = 0;
                p.vpWidth = g_colorW;
                p.vpHeight = g_colorH;
            }

            p.flags = newFlags;

            g_payload = p;
            InterlockedExchange(&g_payloadNew, 1);
            if (g_pipeEvt) SetEvent(g_pipeEvt);
        }

    CALL_ORIG:
        return g_origPresent
            ? g_origPresent(self, SyncInterval, Flags)
            : S_OK;
    }

    HRESULT STDMETHODCALLTYPE hook_ResizeBuffers(IDXGISwapChain* self,
        UINT   BufferCount,
        UINT   Width,
        UINT   Height,
        DXGI_FORMAT NewFormat,
        UINT   SwapChainFlags)
    {
        EnterCriticalSection(&g_cs);
        release_shared_color();
        release_shared_depth();
        g_vpOK = false;
        LeaveCriticalSection(&g_cs);

        return g_origResizeBuffers
            ? g_origResizeBuffers(self, BufferCount, Width, Height,
                NewFormat, SwapChainFlags)
            : S_OK;
    }

    static bool install_hooks(IDXGISwapChain* sc, ID3D11Device* dev)
    {
        void** scVtbl = vtbl_of(sc);
        if (!scVtbl) return false;

        void* oldP = nullptr;
        void* oldRB = nullptr;

        // VMT индексы IDXGISwapChain: Present = 8, ResizeBuffers = 13
        if (!patch_vtable_slot(scVtbl, 8, (void*)hook_Present, &oldP)) return false;
        g_origPresent = (PFN_Present)oldP;

        if (!patch_vtable_slot(scVtbl, 13, (void*)hook_ResizeBuffers, &oldRB)) return false;
        g_origResizeBuffers = (PFN_ResizeBuffers)oldRB;

        void** devVtbl = vtbl_of(dev);
        if (!devVtbl) return false;

        void* oldCT2 = nullptr;
        // VMT индекс ID3D11Device: CreateTexture2D = 5
        if (!patch_vtable_slot(devVtbl, 5, (void*)hook_CreateTexture2D, &oldCT2)) return false;
        g_origCreateTexture2D = (PFN_CreateTexture2D)oldCT2;

        return true;
    }

    static DWORD WINAPI init_thread(LPVOID)
    {
        InitializeCriticalSection(&g_cs);

        // Ждем загрузки библиотек DX11 и DXGI
        for (int i = 0; i < 200 && InterlockedCompareExchange(&g_running, 0, 0); i++)
        {
            if (GetModuleHandleA("d3d11.dll") && GetModuleHandleA("dxgi.dll")) break;
            Sleep(100);
        }
        if (!InterlockedCompareExchange(&g_running, 0, 0)) return 0;

        // Создаем временное окно для инициализации фиктивного SwapChain
        WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0L, 0L, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "FM_DUMMY", NULL };
        RegisterClassExA(&wc);
        HWND hWnd = CreateWindowA("FM_DUMMY", "FM_DUMMY", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

        IDXGISwapChain* dummySC = nullptr;
        ID3D11Device* dummyDev = nullptr;
        ID3D11DeviceContext* dummyCtx = nullptr;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &dummySC,
            &dummyDev,
            &featureLevel,
            &dummyCtx);

        if (FAILED(hr))
        {
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                0,
                featureLevels,
                2,
                D3D11_SDK_VERSION,
                &sd,
                &dummySC,
                &dummyDev,
                &featureLevel,
                &dummyCtx);
        }

        if (SUCCEEDED(hr) && dummySC && dummyDev)
        {
            EnterCriticalSection(&g_cs);
            install_hooks(dummySC, dummyDev);
            LeaveCriticalSection(&g_cs);

            dummyCtx->Release();
            dummySC->Release();
            dummyDev->Release();
        }

        DestroyWindow(hWnd);
        UnregisterClassA("FM_DUMMY", wc.hInstance);

        return 0;
    }

    void shutdown()
    {
        InterlockedExchange(&g_running, 0);
        InterlockedExchange(&g_pipeStop, 1);
        if (g_pipeEvt) SetEvent(g_pipeEvt);

        if (g_pipeThr)
        {
            WaitForSingleObject(g_pipeThr, 2000);
            CloseHandle(g_pipeThr); g_pipeThr = nullptr;
        }
        if (g_initThr)
        {
            WaitForSingleObject(g_initThr, 3000);
            CloseHandle(g_initThr); g_initThr = nullptr;
        }
        if (g_pipeEvt) { CloseHandle(g_pipeEvt); g_pipeEvt = nullptr; }

        EnterCriticalSection(&g_cs);
        release_shared_color();
        release_shared_depth();
        if (g_depthSrc) { g_depthSrc->Release(); g_depthSrc = nullptr; }
        if (g_context) { g_context->Release();  g_context = nullptr; }
        if (g_device) { g_device->Release();   g_device = nullptr; }
        LeaveCriticalSection(&g_cs);

        DeleteCriticalSection(&g_cs);
    }

    void initialize()
    {
        InterlockedExchange(&g_running, 1);
        g_initThr = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    }

}

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        fm::initialize();
        break;
    case DLL_PROCESS_DETACH:
        fm::shutdown();
        break;
    }
    return TRUE;
}