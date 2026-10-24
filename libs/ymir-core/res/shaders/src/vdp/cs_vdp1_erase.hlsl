#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"
#include "vdp1_erase_params.hlsli"

#include "util/bit_ops.hlsli"

cbuffer CommonRenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
    EraseParams g_eraseParams;
}

RWByteAddressBuffer g_fbramOut : register(u1);

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const bool doubleDensity = BitTest(g_commonParams.displayParams, 3);
static const uint drawFB = BitExtract(g_commonParams.displayParams, 7, 1);
// Internal resolution scale: the framebuffers are kScale times larger on each axis
static const uint kScale = BitExtract(g_commonParams.enhancements, 2, 3) + 1u;
static const uint kFBRAMSizeS = kVDP1FBRAMSize * kScale * kScale;
static const uint drawFBOffset = (drawFB ^ 1) * kVDP1FBSize * kScale * kScale;

static const bool deinterlace = BitTest(g_commonParams.enhancements, 0);
static const bool transparentMeshes = BitTest(g_commonParams.enhancements, 1);

static const bool vblankErase = BitTest(g_eraseParams.vblank, 0);
static const uint vblankEraseMaxY = BitExtract(g_eraseParams.vblank, 1, 9);
static const uint vblankEraseMaxX = BitExtract(g_eraseParams.vblank, 10, 10);
static const uint addressShift = BitExtract(g_eraseParams.erase, 16, 1) + 8u;
static const uint eraseScaleV = BitExtract(g_eraseParams.coords, 31, 1);
static const uint eraseX1 = BitExtract(g_eraseParams.coords, 0, 6) << 3u;
static const uint eraseY1 = BitExtract(g_eraseParams.coords, 6, 9) << eraseScaleV;
static const uint eraseX3 = BitExtract(g_eraseParams.coords, 15, 7) << 3u;
static const uint eraseY3 = BitExtract(g_eraseParams.coords, 22, 9) << eraseScaleV;

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

[numthreads(32, 32, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    // VDP1 erase process writes 16-bit words, but this shader works on 32-bit units to avoid interlocked writes.
    // Scaled position; erase limits are checked on the unscaled position
    const uint2 posS = uint2(id.x * 2 + eraseX1 * kScale, id.y + eraseY1 * kScale);
    const uint2 pos = posS / kScale;

    // Bail out if out of range
    if (pos.x >= eraseX3 + 1 || pos.y > eraseY3 + 1) {
        return;
    }
    // Bail out if pixel exceeds VBlank erase cycle limit
    bool partialWrite = false;
    if (vblankErase) {
        if (pos.y > vblankEraseMaxY) {
            return;
        }
        if (pos.y == vblankEraseMaxY && pos.x > vblankEraseMaxX) {
            return;
        }
        // A partial write occurs if the last VBlank erase occurs at an odd X coordinate.
        // This means we have to write only the first 16-bit word and leave the second word untouched.
        partialWrite = kScale == 1 && pos.y == vblankEraseMaxY && pos.x + 1 > vblankEraseMaxX;
    }

    const uint address = drawFBOffset + ((posS.y * ((1u << addressShift) * kScale)) + posS.x) * 2;
    const uint writeValue = BitExtract(g_eraseParams.erase, 0, 16);
    uint value;
    if (partialWrite) {
        value = g_fbramOut.Load(address);
        value &= ~0xFFFF;
        value |= writeValue;
    } else {
        value = (writeValue << 16u) | writeValue;

    }
    g_fbramOut.Store(address, value);
    if (transparentMeshes) {
        g_fbramOut.Store(address + kFBRAMSizeS * 2, 0);
    }
    if (deinterlace && doubleDensity) {
        g_fbramOut.Store(address + kFBRAMSizeS, value);
        if (transparentMeshes) {
            g_fbramOut.Store(address + kFBRAMSizeS * 3, 0);
        }
    }
}
