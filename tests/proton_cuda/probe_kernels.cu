#include <stdint.h>

#define DEVI __device__ __forceinline__

struct RawParams
{
    uint64_t input;
    uint64_t output;
    uint32_t count;
    uint32_t pad;
};

struct ImageParams
{
    uint64_t texture;
    uint64_t surface;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t pad;
};

struct TypedParams
{
    uint64_t texture;
    uint64_t output;
    uint32_t count;
    uint32_t pad;
};

DEVI uint4 Fetch2D(uint64_t texture, int x, int y, int layer)
{
    uint32_t a, b, c, d;
    asm("tex.a2d.v4.u32.s32 {%0,%1,%2,%3}, [%4, {%5, %6, %7, %7}];"
        : "=r"(a), "=r"(b), "=r"(c), "=r"(d) : "l"(texture), "r"(layer), "r"(x), "r"(y));
    return make_uint4(a, b, c, d);
}

DEVI uint4 Fetch1D(uint64_t texture, int element)
{
    uint32_t a, b, c, d;
    asm("tex.1d.v4.u32.s32 {%0,%1,%2,%3}, [%4, {%5}];" : "=r"(a), "=r"(b), "=r"(c), "=r"(d) : "l"(texture), "r"(element));
    return make_uint4(a, b, c, d);
}

DEVI void StoreTexel(uint64_t surface, int x, int y, int layer, uint4 v)
{
    asm volatile("sust.p.a2d.v4.b32.zero [%0, {%1, %2, %3, %3}], {%4, %5, %6, %7};"
        :: "l"(surface), "r"(layer), "r"(x), "r"(y), "r"(v.x), "r"(v.y), "r"(v.z), "r"(v.w) : "memory");
}

DEVI uint4 Transform(uint4 v)
{
    return make_uint4(v.x + 1u, v.y ^ 0xA5A5A5A5u, v.z * 3u, v.w - 5u);
}

DEVI uint4 TransformImage(uint4 v)
{
    return make_uint4(v.x + 0x1000u, v.y ^ 0xFFFF0000u, v.z * 5u, v.w + 9u);
}

extern "C" __global__ void __launch_bounds__(128) probe_raw(const RawParams p)
{
    const uint32_t i = blockIdx.x * 128u + threadIdx.x;
    if (i >= p.count) return;
    const uint4* input = reinterpret_cast<const uint4*>(p.input);
    uint4* output = reinterpret_cast<uint4*>(p.output);
    output[i] = Transform(input[i]);
}

extern "C" __global__ void __launch_bounds__(128) probe_image(const ImageParams p)
{
    const uint32_t t = blockIdx.x * 128u + threadIdx.x;
    if (t >= p.width * p.height * p.layers) return;
    const int x = int(t % p.width);
    const int y = int((t / p.width) % p.height);
    const int layer = int(t / (p.width * p.height));
    StoreTexel(p.surface, x, y, layer, TransformImage(Fetch2D(p.texture, x, y, layer)));
}

extern "C" __global__ void __launch_bounds__(128) probe_typed(const TypedParams p)
{
    const uint32_t i = blockIdx.x * 128u + threadIdx.x;
    if (i >= p.count) return;
    uint4* output = reinterpret_cast<uint4*>(p.output);
    output[i] = Transform(Fetch1D(p.texture, int(i)));
}
