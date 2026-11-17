// Parameter block of the XeFG tensor-core kernels (xefg_cuda_layers.cu), shared by the device code
// and the D3D12 host (NvAPI_D3D12_LaunchCubinShader passes these bytes as the kernel's parameter).
//
// Every handle comes from one of XeFG's own shader-visible descriptors, taken at the replaced
// Dispatch from the descriptor tables XeFG bound for that layer:
//   texture handles (NvAPI_D3D12_GetCudaIndependentDescriptorObject, TEXTURE) for its SRVs and
//   the surface handle (SURFACE) for its UAV. A handle names the descriptor slot, like a table entry.
// With bufferMode kBufferAddresses, weights, scale and bias are instead the GPU virtual addresses of
// their buffer views' first element: vkd3d-proton gives no CUDA object for a buffer view.
// Texture reads use integer coordinates: out of range returns 0 (D3D12 Load semantics). In address mode the
// weights, scale and bias reads follow Intel's program layout with no bounds check. Stores use the
// .zero boundary mode: out of range is discarded (D3D12 UAV store semantics).
#pragma once
#include <cstdint>

inline constexpr uint32_t kBufferTextures = 0;
inline constexpr uint32_t kBufferAddresses = 1;

struct XefgCudaParams
{
    uint64_t input;    // t0: input tensor (Texture2DArray<uint4>)
    uint64_t skip;     // t4: decoder skip tensor (Texture2DArray<uint4>); 0 for the other families
    uint64_t weights;  // t1: Intel's weight buffer (uint4 elements)
    uint64_t scale;    // t2: float scale (pooled/decoder) or int32 bias (flow), uint4 elements
    uint64_t bias;     // t3: float bias (pooled/decoder) or int32 negative multiplier (flow), uint4 elements
    uint64_t output;   // u0: output tensor (RWTexture2DArray<uint4>)
    uint32_t limitW;   // output positions Intel's Dispatch covers (its group count x its group footprint);
    uint32_t limitH;   // the kernels write exactly these positions, like the replaced Dispatch
    uint32_t bufferMode;
    uint32_t reserved;
    uint32_t constants[12]; // the layer's root constants (b0)
};
static_assert(sizeof(XefgCudaParams) == 112, "kernel parameter layout");
