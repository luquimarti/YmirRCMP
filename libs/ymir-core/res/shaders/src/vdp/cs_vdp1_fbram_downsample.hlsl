#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"

#include "util/bit_ops.hlsli"

// Copies the scaled VDP1 framebuffers (internal resolution enhancement) back to native-sized FBRAM so that the CPU can
// read them. Each native pixel takes the value of the top-left pixel of its scaled block.
// Works on one 32-bit native word per thread, covering both framebuffers.

cbuffer CommonRenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
}

ByteAddressBuffer g_fbramScaled : register(t1);

RWByteAddressBuffer g_fbramNative : register(u1);

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const uint kScale = BitExtract(g_commonParams.enhancements, 2, 3) + 1u;
static const uint kFBSizeS = kVDP1FBSize * kScale * kScale;

static const bool pixel8Bits = BitTest(g_commonParams.displayParams, 2);
static const uint fbWidth = 512u << BitExtract(g_commonParams.displayParams, 0, 1); // pixels per line

static const uint kWordsPerFB = kVDP1FBSize / 4u;

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint wordIndex = id.x;
    if (wordIndex >= kWordsPerFB * 2u) {
        return;
    }

    const uint fb = wordIndex / kWordsPerFB;
    const uint address = (wordIndex % kWordsPerFB) * 4u;

    const uint bpp = pixel8Bits ? 1u : 2u;
    const uint ppw = 4u / bpp;
    const uint bitsPerPixel = bpp * 8u;
    const uint pixelMask = pixel8Bits ? 0xFFu : 0xFFFFu;
    const uint firstPixel = address / bpp;

    uint value = 0;
    for (uint p = 0; p < ppw; ++p) {
        const uint pixel = firstPixel + p;
        const uint x = pixel % fbWidth;
        const uint y = pixel / fbWidth;
        const uint scaledAddress = fb * kFBSizeS + ((y * kScale) * (fbWidth * kScale) + x * kScale) * bpp;
        const uint word = g_fbramScaled.Load(scaledAddress & ~3u);
        const uint pixelValue = (word >> ((scaledAddress & 3u) * 8u)) & pixelMask;
        value |= pixelValue << (p * bitsPerPixel);
    }
    g_fbramNative.Store(fb * kVDP1FBSize + address, value);
}
