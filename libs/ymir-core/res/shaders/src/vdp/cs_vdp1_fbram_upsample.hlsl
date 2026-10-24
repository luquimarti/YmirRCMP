#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"

#include "util/bit_ops.hlsli"

// Expands native-sized FBRAM into the scaled VDP1 framebuffers (internal resolution enhancement). Used when the
// scale changes at runtime: every native pixel becomes a kScale x kScale block.
// Works on one 32-bit native word per thread, covering both framebuffers.

cbuffer CommonRenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
}

ByteAddressBuffer g_fbramNative : register(t1);

RWByteAddressBuffer g_fbramScaled : register(u1);

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
    const uint value = g_fbramNative.Load(fb * kVDP1FBSize + address);

    const uint bpp = pixel8Bits ? 1u : 2u;
    const uint ppw = 4u / bpp;
    const uint bitsPerPixel = bpp * 8u;
    const uint pixelMask = pixel8Bits ? 0xFFu : 0xFFFFu;
    const uint firstPixel = address / bpp;
    const uint x0 = firstPixel % fbWidth;
    const uint y0 = firstPixel / fbWidth;
    const uint lineWidthS = fbWidth * kScale;

    // The ppw native pixels of this word cover ppw * kScale scaled pixels per line = kScale whole words
    for (uint j = 0; j < kScale; ++j) {
        const uint lineBase = fb * kFBSizeS + ((y0 * kScale + j) * lineWidthS + x0 * kScale) * bpp;
        for (uint w = 0; w < kScale; ++w) {
            uint outValue = 0;
            for (uint p = 0; p < ppw; ++p) {
                const uint n = (w * ppw + p) / kScale;
                outValue |= ((value >> (n * bitsPerPixel)) & pixelMask) << (p * bitsPerPixel);
            }
            g_fbramScaled.Store(lineBase + w * 4u, outValue);
        }
    }
}
