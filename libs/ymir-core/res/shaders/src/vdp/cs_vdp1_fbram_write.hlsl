#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"
#include "vdp1_write_params.hlsli"

#include "util/bit_ops.hlsli"

struct FBRAMWriteParams {
    uint writeCount;
};

cbuffer CommonRenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
    FBRAMWriteParams g_writeParams;
}

StructuredBuffer<FBRAMWrite> g_fbramWrites : register(t1);

RWByteAddressBuffer g_fbramOut : register(u1);

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const bool doubleDensity = BitTest(g_commonParams.displayParams, 3);
static const uint drawFB = BitExtract(g_commonParams.displayParams, 7, 1);

static const bool deinterlace = BitTest(g_commonParams.enhancements, 0);
static const bool transparentMeshes = BitTest(g_commonParams.enhancements, 1);

// Internal resolution scale: the framebuffers are kScale times larger on each axis
static const uint kScale = BitExtract(g_commonParams.enhancements, 2, 3) + 1u;
static const uint kFBRAMSizeS = kVDP1FBRAMSize * kScale * kScale;
static const uint drawFBOffset = drawFB * kVDP1FBSize * kScale * kScale;

static const bool pixel8Bits = BitTest(g_commonParams.displayParams, 2);
static const uint fbWidth = 512u << BitExtract(g_commonParams.displayParams, 0, 1); // pixels per line

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

// Applies a write to the word at the given address and to its copies in the auxiliary buffers.
void ApplyWrite(uint address, uint andMask, uint orMask) {
    uint value = g_fbramOut.Load(address);
    value &= andMask;
    value |= orMask;

    // Write updated value to FBRAM.
    // Clear corresponding pixels in the mesh buffers.
    // Replicate to the deinterlace buffers.
    g_fbramOut.Store(address, value);
    if (transparentMeshes) {
        uint meshValue = g_fbramOut.Load(address + kFBRAMSizeS * 2);
        meshValue &= andMask;
        g_fbramOut.Store(address + kFBRAMSizeS * 2, meshValue);
    }
    if (deinterlace && doubleDensity) {
        g_fbramOut.Store(address + kFBRAMSizeS, value);
        if (transparentMeshes) {
            uint meshValue = g_fbramOut.Load(address + kFBRAMSizeS * 3);
            meshValue &= andMask;
            g_fbramOut.Store(address + kFBRAMSizeS * 3, meshValue);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint index = id.x;
    if (index >= g_writeParams.writeCount) {
        return;
    }

    const FBRAMWrite write = g_fbramWrites[index];

    if (kScale == 1) {
        // The address is relative to the start of the current CPU-visible framebuffer
        ApplyWrite(write.address + drawFBOffset, write.andMask, write.orMask);
        return;
    }

    // Scaled framebuffer: each written pixel becomes a kScale x kScale block.
    // The native word holds ppw pixels; their blocks span kScale words per scaled line, always word-aligned.
    const uint bpp = pixel8Bits ? 1u : 2u;     // bytes per pixel
    const uint ppw = 4u / bpp;                 // pixels per 32-bit word
    const uint bitsPerPixel = bpp * 8u;
    const uint pixelMask = pixel8Bits ? 0xFFu : 0xFFFFu;
    const uint firstPixel = write.address / bpp;
    const uint x0 = firstPixel % fbWidth;
    const uint y0 = firstPixel / fbWidth;
    const uint lineWidthS = fbWidth * kScale;

    for (uint j = 0; j < kScale; ++j) {
        const uint lineBase = ((y0 * kScale + j) * lineWidthS + x0 * kScale) * bpp;
        for (uint w = 0; w < kScale; ++w) {
            uint andMask = 0;
            uint orMask = 0;
            for (uint p = 0; p < ppw; ++p) {
                // Scaled pixel (w * ppw + p) of this group comes from native pixel n of the written word
                const uint n = (w * ppw + p) / kScale;
                const uint srcShift = n * bitsPerPixel;
                const uint dstShift = p * bitsPerPixel;
                andMask |= ((write.andMask >> srcShift) & pixelMask) << dstShift;
                orMask |= ((write.orMask >> srcShift) & pixelMask) << dstShift;
            }
            ApplyWrite(drawFBOffset + lineBase + w * 4u, andMask, orMask);
        }
    }
}
