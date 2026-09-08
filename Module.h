#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

#define PIPE_NAME    L"\\\\.\\pipe\\qwleyshade"
#define PAYLOAD_SIZE 0x40u

//Roblox pointer chain

#define OFF_VISUAL_ENGINE_PTR   0x08351408ULL
#define OFF_VE_TO_RENDER_VIEW   0x0C30ULL
#define OFF_RV_TO_DEVICE        0x0008ULL
#define OFF_DEV_TO_SWAPCHAIN    0xC8ULL

// vtable slots
#define VT_PRESENT          8
#define VT_RESIZEBUFFERS    13
#define VT_CREATETEXTURE2D  5

// Depth signature
#define DEPTH_FORMAT_EXPECTED   (DXGI_FORMAT_R32_TYPELESS)
#define DEPTH_BINDFLAG_EXPECTED (0x48u)

// ─── Payload
#pragma pack(push, 1)
struct PipePayload
{
    uint64_t depthHandle;   // 0x00
    uint64_t colorHandle;   // 0x08
    uint32_t depthWidth;    // 0x10
    uint32_t depthHeight;   // 0x14
    uint32_t colorWidth;    // 0x18
    uint32_t colorHeight;   // 0x1C
    uint32_t depthFormat;   // 0x20
    uint32_t colorFormat;   // 0x24
    uint32_t frameCount;    // 0x28
    uint32_t flags;         // 0x2C
    uint32_t vpX;           // 0x30
    uint32_t vpY;           // 0x34
    uint32_t vpWidth;       // 0x38
    uint32_t vpHeight;      // 0x3C
};
#pragma pack(pop)
static_assert(sizeof(PipePayload) == PAYLOAD_SIZE, "PipePayload size mismatch");

// ─── Flags
#define FLAG_COLOR_OK    0x01u
#define FLAG_DEPTH_OK    0x02u
#define FLAG_FPS_OK      0x04u
#define FLAG_VIEWPORT_OK 0x10u