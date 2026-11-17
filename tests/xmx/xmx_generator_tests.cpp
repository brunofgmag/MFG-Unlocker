#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "xmx/xmx_context.h"
#include "xmx/xmx_generator.h"
#include "xmx/xmx_hud_composite.h"
#include "xmx/xmx_status.h"
#ifdef XMX_HUD_PASS_STUB
#include "hud_pass_stub.h"
#endif

using Microsoft::WRL::ComPtr;

namespace
{
unsigned gChecks = 0, gFailures = 0;
unsigned gCreates = 0, gInitializes = 0, gExecutes = 0;
char gHandles[64];
std::vector<unsigned> gInitFrames;
std::vector<std::string> gLogs;
unsigned gRefuseMultiHeight = 0, gRefuseAllHeight = 0;

struct ExecuteRecord
{
    ID3D12Resource* nextBackbuffer{};
    ID3D12Resource* nextHudless{};
    ID3D12Resource* previousBackbuffer{};
    ID3D12Resource* previousHudless{};
    DXGI_FORMAT nextFormat{};
    DXGI_FORMAT previousFormat{};
};

ExecuteRecord gLastExecute;
int gExecuteResult = 0;
ID3D12Resource* gFill = nullptr;
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT kHudlessFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

struct Parameters final : NVSDK_NGX_Parameter
{
    std::map<std::string, ID3D12Resource*> resources;
    std::map<std::string, unsigned int> numbers;
    std::map<std::string, int> signedNumbers;

    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char* name, unsigned int value) override { numbers[name] = value; }
    void Set(const char* name, int value) override { signedNumbers[name] = value; }
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char* name, ID3D12Resource* value) override { resources[name] = value; }
    void Set(const char*, void*) override {}
    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override
    {
        const auto found = numbers.find(name);
        if (found == numbers.end()) return NVSDK_NGX_Result_Fail;
        *value = found->second;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* name, int* value) const override
    {
        const auto found = signedNumbers.find(name);
        if (found == signedNumbers.end()) return NVSDK_NGX_Result_Fail;
        *value = found->second;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const override
    {
        const auto found = resources.find(name);
        if (found == resources.end()) return NVSDK_NGX_Result_Fail;
        *value = found->second;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_Fail; }
    void Reset() override {}
};

struct Textures
{
    ComPtr<ID3D12Resource> backbuffer, interpolated, depth, motion;
    std::map<std::pair<UINT, DXGI_FORMAT>, ComPtr<ID3D12Resource>> hudless;
};

struct Rig
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::map<UINT, Textures> sized;
    xmx::Rvas rvas{};
    DXGI_FORMAT backbufferFormat{kBackbufferFormat};
    DXGI_FORMAT outputFormat{kBackbufferFormat};
};

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&resource));
    return resource;
}

void AttachRig(Rig& rig)
{
    HMODULE fake = reinterpret_cast<HMODULE>(&gHandles);
    xmx::generator::Attach({fake, &rig.rvas, xmx::kMaxFrames});
}

bool OpenRig(Rig& rig)
{
    const HRESULT created = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&rig.device));
    std::printf("D3D12CreateDevice at feature level 11_0 returned 0x%08lx\n", static_cast<unsigned long>(created));
    if (FAILED(created)) return false;
    if (FAILED(rig.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&rig.allocator)))
        || FAILED(rig.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rig.allocator.Get(), nullptr,
            IID_PPV_ARGS(&rig.list))))
        return false;
    AttachRig(rig);
    return true;
}

bool Evaluate(Rig& rig, UINT width, UINT height, int depthInverted = 0, unsigned frames = 0, UINT hudlessWidth = 0,
    DXGI_FORMAT hudlessFormat = kHudlessFormat)
{
    auto& textures = rig.sized[width];
    if (!textures.backbuffer)
    {
        auto* device = rig.device.Get();
        textures.backbuffer = Texture(device, width, height, rig.backbufferFormat, D3D12_RESOURCE_FLAG_NONE);
        textures.interpolated = Texture(device, width, height, rig.outputFormat,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        textures.depth = Texture(device, width, height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        textures.motion = Texture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    }
    Parameters params;
    params.Set("DLSSG.DepthInverted", depthInverted);
    if (frames != 0) params.Set("DLSSG.MultiFrameCount", frames);
    params.resources["DLSSG.Backbuffer"] = textures.backbuffer.Get();
    params.resources["DLSSG.OutputInterpolated"] = textures.interpolated.Get();
    params.resources["DLSSG.Depth"] = textures.depth.Get();
    params.resources["DLSSG.MVecs"] = textures.motion.Get();
    if (hudlessWidth != 0)
    {
        auto& hudless = textures.hudless[{hudlessWidth, hudlessFormat}];
        if (!hudless)
            hudless = Texture(rig.device.Get(), hudlessWidth, height, hudlessFormat, D3D12_RESOURCE_FLAG_NONE);
        params.resources["DLSSG.HUDLess"] = hudless.Get();
    }
    return xmx::generator::Evaluate(rig.list.Get(), &params);
}

int Reuse()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kHeightA = 360, kHeightB = 450, kHeightC = 540;
    Check(Evaluate(rig, 640, kHeightA), "the first shape gets its context at once");
    Check(gCreates == 1 && gInitializes == 1, "one context created and initialized for the first shape");

    Check(!Evaluate(rig, 800, kHeightB), "a new shape is declined while it settles");
    Check(!Evaluate(rig, 800, kHeightB), "it is still declined on the next frame");
    Check(gCreates == 1 && gInitializes == 1, "no context is made while the shape settles");

    Sleep(1100);
    Check(Evaluate(rig, 800, kHeightB), "the shape that stayed for a second is generated");
    Check(gCreates == 2 && gInitializes == 2, "the settled shape gets its own context");

    Check(Evaluate(rig, 640, kHeightA), "going back to the first shape is generated at once");
    Check(Evaluate(rig, 800, kHeightB), "and so is going to the second again");
    Check(Evaluate(rig, 640, kHeightA), "and back");
    Check(gCreates == 2 && gInitializes == 2, "cached shapes reuse their contexts: two creates, not four");

    Check(!Evaluate(rig, 960, kHeightC), "a third shape is declined while it settles");
    Check(Evaluate(rig, 640, kHeightA), "returning to the current shape is generated");
    Sleep(1100);
    Check(!Evaluate(rig, 960, kHeightC), "the wait for the third shape starts over after the current shape came back");
    Check(Evaluate(rig, 800, kHeightB), "switching to a cached shape is generated");
    Sleep(1100);
    Check(!Evaluate(rig, 960, kHeightC), "the wait for the third shape starts over after a cached switch");
    Check(gCreates == 2 && gInitializes == 2, "a shape that did not settle makes no context");
    Check(Evaluate(rig, 640, kHeightA), "the first shape again");
    Check(!Evaluate(rig, 640, kHeightA, 1), "the same size with other context flags is a new shape and settles");
    Check(gCreates == 2 && gInitializes == 2, "other flags do not reuse the cached context");
    Check(Evaluate(rig, 640, kHeightA), "the cached flags are generated at once");

    Evaluate(rig, 640, kHeightA);
    Evaluate(rig, 640, kHeightA);
    const unsigned executes = gExecutes;
    Evaluate(rig, 800, kHeightB);
    Check(gExecutes == executes, "the first frame after a switch has no pair and runs no model");
    Evaluate(rig, 800, kHeightB);
    Check(gExecutes == executes + 1, "the second frame after a switch runs the model");

    const unsigned before = gExecutes;
    Evaluate(rig, 960, kHeightC);
    Evaluate(rig, 800, kHeightB);
    Check(gExecutes == before, "a frame declined in between breaks the pair");
    return 0;
}

unsigned CountLogs(const char* prefix)
{
    unsigned count = 0;
    for (const auto& line : gLogs) count += line.rfind(prefix, 0) == 0;
    return count;
}

bool ContextLine(const std::string& line, unsigned width, unsigned height, unsigned frames)
{
    unsigned readWidth = 0, readHeight = 0, readFrames = 0;
    long long before = 0, after = 0, delta = 0, budget = 0;
    const int parsed = sscanf_s(line.c_str(), "context %ux%u frames %u: used %lld -> %lld MB (%lld), budget %lld MB",
        &readWidth, &readHeight, &readFrames, &before, &after, &delta, &budget);
    return parsed == 7 && readWidth == width && readHeight == height && readFrames == frames
        && delta == after - before && before >= 0 && after >= 0 && budget >= 0;
}

std::string LastContextLine()
{
    for (auto line = gLogs.rbegin(); line != gLogs.rend(); ++line)
        if (line->rfind("context ", 0) == 0) return *line;
    return {};
}

int Frames()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kHeight = 360;
    Check(Evaluate(rig, 640, kHeight, 0, 3), "the first frame asking for 3 generated frames is generated at once");
    Check(gCreates == 1 && gInitializes == 1, "one context for the first shape");
    Check(gInitFrames.back() == 3, "it is initialized for 3 frames, not the model's 5");
    Check(ContextLine(LastContextLine(), 640, kHeight, 3), "the creation logged its memory cost with the frames it holds");
    Check(CountLogs("context ") == 1, "one cost line for one context");

    Check(Evaluate(rig, 640, kHeight, 0, 3), "the same request again");
    Check(Evaluate(rig, 640, kHeight, 0, 2), "a request for 2 is served by the context of 3");
    Check(Evaluate(rig, 640, kHeight, 0, 1), "a request for 1 is served by the context of 3");
    Check(gCreates == 1 && gInitializes == 1, "a smaller request does not re-initialize");
    Check(CountLogs("context ") == 1, "and logs no new cost line");
    Check(CountLogs("reusing") == 0, "the current context is kept, not switched to");

    Check(!Evaluate(rig, 640, kHeight, 0, 4), "a request for 4 settles before it makes a context");
    Check(gCreates == 1 && gInitializes == 1, "nothing is made while it settles");
    Sleep(1100);
    Check(Evaluate(rig, 640, kHeight, 0, 4), "the request for 4 is generated after the settle time");
    Check(gCreates == 2 && gInitializes == 2, "it gets a second context");
    Check(gInitFrames.back() == 4, "initialized for 4 frames");
    Check(ContextLine(LastContextLine(), 640, kHeight, 4), "the second creation logged its cost");
    Check(CountLogs("context ") == 2, "two cost lines for two contexts");

    Check(Evaluate(rig, 640, kHeight, 0, 3), "a request for 3 is now served by the current context of 4");
    Check(Evaluate(rig, 640, kHeight, 0, 4), "and so is 4");
    Check(gCreates == 2 && gInitializes == 2, "no third context");

    Check(Evaluate(rig, 640, kHeight, 0, 5) == false, "a request for 5 settles");
    Sleep(1100);
    Check(Evaluate(rig, 640, kHeight, 0, 5), "and is generated");
    Check(gCreates == 3 && gInitFrames.back() == 5, "the third context holds 5");
    Check(Evaluate(rig, 640, kHeight, 0, 9), "a request beyond the model's 5 is served by the context of 5");
    Check(gCreates == 3, "the request is capped at the model's frames, so no new context");
    return 0;
}

int Refused()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kHeightA = 360, kHeightB = 450, kHeightC = 540;
    gRefuseAllHeight = kHeightB;
    Check(Evaluate(rig, 640, kHeightA, 0, 3), "the good shape gets its context");
    Check(!Evaluate(rig, 800, kHeightB, 0, 3), "the shape the model will refuse settles first");
    Sleep(1100);
    Check(Evaluate(rig, 800, kHeightB, 0, 3), "the refused shape repeats the real frame");
    Check(gCreates == 3 && gInitializes == 3, "the refusal tried the multi-frame model and then the 2x one");
    const unsigned afterB = gCreates;

    Check(Evaluate(rig, 640, kHeightA, 0, 3), "the good shape comes back from the cache");
    Check(Evaluate(rig, 800, kHeightB, 0, 3), "the refused shape is answered at once");
    Check(Evaluate(rig, 640, kHeightA, 0, 3), "the good shape again");
    Check(Evaluate(rig, 800, kHeightB, 0, 3), "the refused shape again");
    Check(gCreates == afterB, "the refused shape makes no new context");

    gRefuseAllHeight = kHeightC;
    Check(!Evaluate(rig, 960, kHeightC, 0, 3), "a second refused shape settles first");
    Sleep(1100);
    Check(Evaluate(rig, 960, kHeightC, 0, 3), "the second refused shape repeats the real frame");
    const unsigned afterC = gCreates;
    Check(afterC > afterB, "the second refusal made its contexts");
    Check(Evaluate(rig, 800, kHeightB, 0, 3), "the first refused shape is still refused");
    Check(Evaluate(rig, 960, kHeightC, 0, 3), "the second refused shape is still refused");
    Check(Evaluate(rig, 640, kHeightA, 0, 3), "the good shape");
    Check(Evaluate(rig, 800, kHeightB, 0, 3), "the first refused shape once more");
    Check(Evaluate(rig, 960, kHeightC, 0, 3), "the second refused shape once more");
    Check(gCreates == afterC, "neither refused shape makes a context again");
    return 0;
}

int RefusedAlternating()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kHeightA = 360, kHeightB = 450;
    gRefuseAllHeight = kHeightB;
    Check(Evaluate(rig, 640, kHeightA, 0, 1), "the good shape asks for 2x");
    Check(!Evaluate(rig, 800, kHeightB, 0, 1), "the refused shape settles first");
    Sleep(1100);
    Check(Evaluate(rig, 800, kHeightB, 0, 1), "the refused shape repeats the real frame");
    Check(gCreates == 3, "one create for the good shape, and the refused one tried with 2 frames and then the 2x model");
    for (int i = 0; i < 3; ++i)
    {
        Check(Evaluate(rig, 640, kHeightA, 0, 1), "back to the good shape");
        Check(Evaluate(rig, 800, kHeightB, 0, 1), "and to the refused one");
    }
    Check(gCreates == 3, "alternating a refused shape with a good one creates nothing more");
    return 0;
}

int CopyMismatch()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    auto* device = rig.device.Get();
    const auto backbuffer = Texture(device, 640, 360, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_NONE);
    const auto matching = Texture(device, 640, 360, DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto larger = Texture(device, 800, 450, DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Parameters params;
    params.resources["DLSSG.Backbuffer"] = backbuffer.Get();
    params.resources["DLSSG.OutputInterpolated"] = larger.Get();
    Check(!xmx::generator::Passthrough(rig.list.Get(), &params),
        "a repeated frame whose output is another size than the backbuffer is declined");
    params.resources["DLSSG.OutputInterpolated"] = matching.Get();
    Check(xmx::generator::Passthrough(rig.list.Get(), &params), "an output of the backbuffer's size is repeated");
    params.resources["DLSSG.OutputReal"] = larger.Get();
    Check(!xmx::generator::Passthrough(rig.list.Get(), &params),
        "a real-frame output of another size is declined too");

    const auto depth = Texture(device, 640, 360, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    const auto motion = Texture(device, 640, 360, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    Parameters generated;
    generated.resources["DLSSG.Backbuffer"] = backbuffer.Get();
    generated.resources["DLSSG.OutputInterpolated"] = matching.Get();
    generated.resources["DLSSG.Depth"] = depth.Get();
    generated.resources["DLSSG.MVecs"] = motion.Get();
    Check(xmx::generator::Evaluate(rig.list.Get(), &generated), "a generated frame of the backbuffer's size is shown");
    generated.resources["DLSSG.OutputReal"] = larger.Get();
    Check(!xmx::generator::Evaluate(rig.list.Get(), &generated),
        "a generated frame whose real-frame output is another size is declined");
    generated.resources.erase("DLSSG.OutputReal");
    generated.resources["DLSSG.OutputInterpolated"] = larger.Get();
    xmx::generator::Evaluate(rig.list.Get(), &generated);
    Sleep(1100);
    Check(!xmx::generator::Evaluate(rig.list.Get(), &generated),
        "a generated frame whose output is another size than the backbuffer is declined");
    return 0;
}

int Fallback()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kHeightA = 360, kHeightF = 450, kHeightG = 540;
    gRefuseMultiHeight = kHeightF;
    Check(Evaluate(rig, 640, kHeightA, 1, 3), "the good shape gets 3 frames");
    Check(gInitFrames.back() == 3, "initialized for 3");
    Check(!Evaluate(rig, 800, kHeightF, 1, 3), "the shape the multi-frame model refuses settles first");
    Sleep(1100);
    Check(Evaluate(rig, 800, kHeightF, 1, 3), "it is generated by the 2x model");
    Check(gCreates == 3 && gInitializes == 3, "the multi-frame try and the 2x fallback each made a context");
    Check(gInitFrames[1] == 3 && gInitFrames[2] == 1, "tried with 3 frames, then with 1");
    Check(ContextLine(LastContextLine(), 800, kHeightF, 1), "the cost line names the frames the context holds");

    Check(Evaluate(rig, 640, kHeightA, 1, 3), "back to the good shape");
    Check(Evaluate(rig, 800, kHeightF, 1, 3), "the fallback shape is generated at once");
    Check(Evaluate(rig, 640, kHeightA, 1, 3), "and back");
    Check(Evaluate(rig, 800, kHeightF, 1, 5), "a larger request is served by the fallback context too");
    Check(gCreates == 3 && gInitializes == 3, "a fallback shape visited again creates nothing");

    Check(!Evaluate(rig, 960, kHeightG, 1, 3), "a new shape settles");
    Sleep(1100);
    Check(Evaluate(rig, 960, kHeightG, 1, 3), "and is generated");
    Check(gInitFrames.back() == 3, "the fallback did not lower the frames of later shapes");
    return 0;
}

int MemoryEmpty()
{
    Check(xmx::generator::MemoryLine().empty(), "with no device the VRAM line is empty");
    return 0;
}

struct Memory
{
    unsigned long long used = 0, budget = 0, copies = 0, outputs = 0;
    unsigned contexts = 0;
    bool parsed = false;
};

Memory ParseMemory(const std::string& line)
{
    Memory memory;
    memory.parsed = sscanf_s(line.c_str(),
        "VRAM 10 s: used %llu MB of budget %llu MB, contexts %u, copies %llu MB, outputs %llu MB", &memory.used,
        &memory.budget, &memory.contexts, &memory.copies, &memory.outputs) == 5;
    return memory;
}

unsigned long long Allocated(Rig& rig, ID3D12Resource* resource)
{
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    return rig.device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}

unsigned long long InputBytes(Rig& rig, UINT width)
{
    const auto& textures = rig.sized[width];
    return Allocated(rig, textures.backbuffer.Get()) + Allocated(rig, textures.depth.Get())
        + Allocated(rig, textures.motion.Get());
}

int MemoryDevice()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kWidthA = 1920, kHeightA = 1080, kWidthB = 1280, kHeightB = 720;
    constexpr unsigned kFrames = 3;
    Check(xmx::generator::MemoryLine().empty(), "before a frame reaches the generator the VRAM line is empty");
    Check(Evaluate(rig, kWidthA, kHeightA, 0, kFrames), "a frame is generated on the real device");
    const std::string first = xmx::generator::MemoryLine();
    std::printf("%s\n", first.c_str());
    if (first.empty())
    {
        std::printf("SETUP the adapter's video memory could not be queried here\n");
        return 2;
    }
    const Memory memory = ParseMemory(first);
    Check(memory.parsed && memory.budget > 0 && memory.contexts == 1, "the line reports a budget above zero and one context");

    Check(Evaluate(rig, kWidthA, kHeightA, 0, kFrames), "the second frame of the pair is generated");
    const unsigned long long outputA = Allocated(rig, rig.sized[kWidthA].interpolated.Get());
    const unsigned long long inputsA = InputBytes(rig, kWidthA);
    const Memory both = ParseMemory(xmx::generator::MemoryLine());
    Check(both.parsed && both.copies > 0 && both.copies == (2 * inputsA) >> 20,
        "the copies are the backbuffer, depth and motion copies of both frame slots");
    Check(both.outputs > 0 && both.outputs == (kFrames * outputA) >> 20, "the outputs are one texture per generated frame");

    Evaluate(rig, kWidthB, kHeightB, 0, kFrames);
    Sleep(1100);
    Check(Evaluate(rig, kWidthB, kHeightB, 0, kFrames), "the settled shape is generated");
    Check(Evaluate(rig, kWidthB, kHeightB, 0, kFrames), "and its second frame too");
    const unsigned long long outputB = Allocated(rig, rig.sized[kWidthB].interpolated.Get());
    const unsigned long long inputsB = InputBytes(rig, kWidthB);
    const Memory switched = ParseMemory(xmx::generator::MemoryLine());
    Check(switched.parsed && switched.contexts == 2, "the new shape has its own context");
    Check(switched.copies == (2 * inputsB + 2 * inputsA) >> 20,
        "the copies of the retired context are held until their commands finish");
    Check(switched.outputs == (kFrames * outputB) >> 20, "the outputs are the live ones, not the retired context's");
    return 0;
}

constexpr UINT kHudSize = 64;
constexpr UINT kTileSize = 67;
constexpr uint32_t kGrey = 0xFF808080u;
constexpr uint32_t kFaint = 0xFF828282u;
constexpr uint32_t kStrong = 0xFF848484u;
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kMagenta = 0xFFFF00FFu;
constexpr uint32_t kFake = 0xFF204060u;
constexpr float kModelTolerance = 3.5f / 255.0f;
constexpr auto kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kUnorderedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

constexpr uint32_t Ten(uint32_t level)
{
    return level | (level << 10) | (level << 20) | (3u << 30);
}

constexpr uint32_t kTenGrey = Ten(512);
constexpr uint32_t kTenWhite = Ten(1023);
constexpr uint32_t kTenBlack = Ten(0);

struct Bench
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 value{};
    std::vector<ComPtr<ID3D12Resource>> keep;

    ~Bench()
    {
        if (event) CloseHandle(event);
    }
};

bool OpenBench(Bench& bench)
{
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    const HRESULT created = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&bench.device));
    std::printf("D3D12CreateDevice at feature level 11_0 returned 0x%08lx\n", static_cast<unsigned long>(created));
    if (FAILED(created)) return false;
    bench.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return bench.event && SUCCEEDED(bench.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&bench.queue)))
        && SUCCEEDED(bench.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&bench.allocator)))
        && SUCCEEDED(bench.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, bench.allocator.Get(), nullptr,
            IID_PPV_ARGS(&bench.list)))
        && SUCCEEDED(bench.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&bench.fence)));
}

bool Submit(Bench& bench)
{
    if (FAILED(bench.list->Close())) return false;
    ID3D12CommandList* lists[] = {bench.list.Get()};
    bench.queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++bench.value;
    if (FAILED(bench.queue->Signal(bench.fence.Get(), value))) return false;
    if (bench.fence->GetCompletedValue() < value)
    {
        if (FAILED(bench.fence->SetEventOnCompletion(value, bench.event))) return false;
        WaitForSingleObject(bench.event, 30000);
    }
    if (bench.fence->GetCompletedValue() < value) return false;
    bench.keep.clear();
    return SUCCEEDED(bench.allocator->Reset()) && SUCCEEDED(bench.list->Reset(bench.allocator.Get(), nullptr));
}

ComPtr<ID3D12Resource> HostBuffer(Bench& bench, D3D12_HEAP_TYPE type, UINT64 size)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    bench.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&buffer));
    return buffer;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

bool ToState(Bench& bench, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    const auto barrier = Transition(resource, from, to);
    bench.list->ResourceBarrier(1, &barrier);
    return Submit(bench);
}

UINT Pitch(UINT width)
{
    return (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
        * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
}

D3D12_TEXTURE_COPY_LOCATION BufferLocation(ID3D12Resource* buffer, UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint.Footprint = {format, width, height, 1, Pitch(width)};
    return location;
}

D3D12_TEXTURE_COPY_LOCATION TextureLocation(ID3D12Resource* texture)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = texture;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return location;
}

bool Upload(Bench& bench, ID3D12Resource* texture, const std::vector<uint32_t>& pixels, UINT width, UINT height,
    D3D12_RESOURCE_STATES final, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
{
    auto upload = HostBuffer(bench, D3D12_HEAP_TYPE_UPLOAD, static_cast<UINT64>(Pitch(width)) * height);
    void* mapped = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, &mapped))) return false;
    for (UINT row = 0; row < height; ++row)
        std::memcpy(static_cast<uint8_t*>(mapped) + static_cast<size_t>(row) * Pitch(width),
            pixels.data() + static_cast<size_t>(row) * width, width * 4);
    upload->Unmap(0, nullptr);
    const auto toCopy = Transition(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    bench.list->ResourceBarrier(1, &toCopy);
    const auto destination = TextureLocation(texture);
    const auto source = BufferLocation(upload.Get(), width, height, format);
    bench.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (final != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        const auto toFinal = Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, final);
        bench.list->ResourceBarrier(1, &toFinal);
    }
    bench.keep.push_back(upload);
    return Submit(bench);
}

bool ReadBack(Bench& bench, ID3D12Resource* texture, D3D12_RESOURCE_STATES state, UINT width, UINT height,
    std::vector<uint32_t>& pixels, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
{
    auto readback = HostBuffer(bench, D3D12_HEAP_TYPE_READBACK, static_cast<UINT64>(Pitch(width)) * height);
    if (!readback) return false;
    const auto toSource = Transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bench.list->ResourceBarrier(1, &toSource);
    const auto destination = BufferLocation(readback.Get(), width, height, format);
    const auto source = TextureLocation(texture);
    bench.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    const auto back = Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    bench.list->ResourceBarrier(1, &back);
    if (!Submit(bench)) return false;
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
    pixels.resize(static_cast<size_t>(width) * height);
    for (UINT row = 0; row < height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * width,
            static_cast<const uint8_t*>(mapped) + static_cast<size_t>(row) * Pitch(width), width * 4);
    readback->Unmap(0, nullptr);
    return true;
}

std::vector<uint32_t> Filled(UINT width, UINT height, uint32_t color)
{
    return std::vector<uint32_t>(static_cast<size_t>(width) * height, color);
}

void PaintBlock(std::vector<uint32_t>& pixels, UINT size, uint32_t color)
{
    for (UINT y = 20; y < 24; ++y)
        for (UINT x = 20; x < 24; ++x) pixels[y * size + x] = color;
}

struct Scene
{
    ComPtr<ID3D12Resource> real, hudless, output;
    DXGI_FORMAT format{DXGI_FORMAT_R8G8B8A8_UNORM};
    UINT size{kHudSize};
    std::vector<uint32_t> realPixels, hudlessPixels;
    uint32_t initial{kBlack};
    uint32_t background{kBlack};
};

Scene BlockScene()
{
    Scene scene;
    scene.realPixels = Filled(kHudSize, kHudSize, kGrey);
    PaintBlock(scene.realPixels, kHudSize, kWhite);
    scene.realPixels[10 * kHudSize + 10] = kStrong;
    scene.realPixels[50 * kHudSize + 50] = kFaint;
    scene.hudlessPixels = Filled(kHudSize, kHudSize, kGrey);
    return scene;
}

Scene TileScene()
{
    Scene scene;
    scene.size = kTileSize;
    scene.realPixels = Filled(kTileSize, kTileSize, kGrey);
    scene.realPixels[(kTileSize - 1) * kTileSize + kTileSize - 1] = kStrong;
    scene.hudlessPixels = Filled(kTileSize, kTileSize, kGrey);
    return scene;
}

Scene TenBitScene()
{
    Scene scene;
    scene.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    scene.initial = kTenBlack;
    scene.background = kTenBlack;
    scene.realPixels = Filled(kHudSize, kHudSize, kTenGrey);
    PaintBlock(scene.realPixels, kHudSize, kTenWhite);
    scene.hudlessPixels = Filled(kHudSize, kHudSize, kTenGrey);
    return scene;
}

bool BuildScene(Bench& bench, Scene& scene, UINT hudlessSize, D3D12_RESOURCE_STATES hudlessState = kReadState)
{
    if (hudlessSize != scene.size)
        scene.hudlessPixels = Filled(hudlessSize, hudlessSize, scene.hudlessPixels[0]);
    auto* device = bench.device.Get();
    scene.real = Texture(device, scene.size, scene.size, scene.format, D3D12_RESOURCE_FLAG_NONE);
    scene.hudless = Texture(device, hudlessSize, hudlessSize, scene.format, D3D12_RESOURCE_FLAG_NONE);
    scene.output = Texture(device, scene.size, scene.size, scene.format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return scene.real && scene.hudless && scene.output
        && Upload(bench, scene.real.Get(), scene.realPixels, scene.size, scene.size, kReadState, scene.format)
        && Upload(bench, scene.hudless.Get(), scene.hudlessPixels, hudlessSize, hudlessSize, hudlessState, scene.format)
        && Upload(bench, scene.output.Get(), Filled(scene.size, scene.size, scene.initial), scene.size, scene.size,
            kUnorderedState, scene.format);
}

bool ApplyAndRead(Bench& bench, xmx::hud_composite::Pass& pass, Scene& scene, bool debug, bool& applied,
    std::vector<uint32_t>& pixels)
{
    applied = pass.Apply(bench.list.Get(), bench.device.Get(), scene.output.Get(), scene.real.Get(),
        scene.hudless.Get(), debug);
    return ReadBack(bench, scene.output.Get(), kUnorderedState, scene.size, scene.size, pixels, scene.format);
}

struct Rgb
{
    float r, g, b;
};

Rgb Decode(DXGI_FORMAT format, uint32_t texel)
{
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return {(texel & 1023u) / 1023.f, ((texel >> 10) & 1023u) / 1023.f, ((texel >> 20) & 1023u) / 1023.f};
    return {(texel & 255u) / 255.f, ((texel >> 8) & 255u) / 255.f, ((texel >> 16) & 255u) / 255.f};
}

bool Differs(const Scene& scene, size_t index)
{
    const auto real = Decode(scene.format, scene.realPixels[index]);
    const auto hudless = Decode(scene.format, scene.hudlessPixels[index]);
    return std::max({std::fabs(real.r - hudless.r), std::fabs(real.g - hudless.g), std::fabs(real.b - hudless.b)})
        > kModelTolerance;
}

bool HudAt(const Scene& scene, UINT x, UINT y)
{
    const int last = static_cast<int>(scene.size) - 1;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int column = std::clamp(static_cast<int>(x) + dx, 0, last);
            const int row = std::clamp(static_cast<int>(y) + dy, 0, last);
            if (Differs(scene, static_cast<size_t>(row) * scene.size + column)) return true;
        }
    return false;
}

uint32_t ExpectedPixel(const Scene& scene, UINT x, UINT y, bool debug)
{
    if (!HudAt(scene, x, y)) return scene.background;
    return debug ? kMagenta : scene.realPixels[static_cast<size_t>(y) * scene.size + x];
}

bool Matches(const std::vector<uint32_t>& pixels, const Scene& scene, bool debug)
{
    for (UINT y = 0; y < scene.size; ++y)
        for (UINT x = 0; x < scene.size; ++x)
        {
            const uint32_t expected = ExpectedPixel(scene, x, y, debug);
            const uint32_t actual = pixels[static_cast<size_t>(y) * scene.size + x];
            if (actual != expected)
            {
                std::printf("pixel %u,%u is 0x%08x, expected 0x%08x\n", x, y, actual, expected);
                return false;
            }
        }
    return true;
}

bool Untouched(const std::vector<uint32_t>& pixels, const Scene& scene)
{
    for (const auto pixel : pixels)
        if (pixel != scene.initial) return false;
    return true;
}

int HudComposite()
{
    Bench bench;
    if (!OpenBench(bench)) return 2;
    std::vector<uint32_t> pixels;
    bool applied = false;

    {
        auto scene = BlockScene();
        xmx::hud_composite::Pass pass;
        if (!Check(BuildScene(bench, scene, kHudSize), "the scene is uploaded")) return 1;
        Check(ApplyAndRead(bench, pass, scene, false, applied, pixels), "the pass ran and the output was read back");
        Check(applied && !pass.Failed(), "the pass applies to matching sizes");
        Check(Matches(pixels, scene, false),
            "the HUD pixels and their one-pixel ring carry the real frame's pixels and the rest stays untouched");
        Check(pixels[20 * kHudSize + 20] == kWhite && pixels[19 * kHudSize + 19] == kGrey
            && pixels[18 * kHudSize + 18] == kBlack && pixels[24 * kHudSize + 24] == kGrey
            && pixels[25 * kHudSize + 25] == kBlack,
            "the 4x4 block is white, its ring is the real frame's grey, and the ring is one pixel wide");
        Check(pixels[10 * kHudSize + 10] == kStrong && pixels[50 * kHudSize + 50] == kBlack,
            "a difference of 4/255 counts as HUD and a difference of 2/255 does not");
    }

    {
        auto scene = BlockScene();
        xmx::hud_composite::Pass pass;
        if (!Check(BuildScene(bench, scene, kHudSize), "the debug scene is uploaded")) return 1;
        Check(ApplyAndRead(bench, pass, scene, true, applied, pixels), "the debug pass ran and was read back");
        Check(applied && Matches(pixels, scene, true), "with debug on the HUD region is magenta and the rest untouched");
    }

    {
        auto scene = TileScene();
        xmx::hud_composite::Pass pass;
        if (!Check(BuildScene(bench, scene, kTileSize), "the partial-tile scene is uploaded")) return 1;
        Check(ApplyAndRead(bench, pass, scene, false, applied, pixels), "the partial-tile pass was read back");
        constexpr UINT kLast = kTileSize - 1;
        Check(applied && Matches(pixels, scene, false),
            "a size that is not a multiple of 8 is composed over its whole area");
        Check(pixels[kLast * kTileSize + kLast] == kStrong && pixels[(kLast - 1) * kTileSize + kLast - 1] == kGrey
            && pixels[(kLast - 2) * kTileSize + kLast - 2] == kBlack,
            "the HUD pixel in the last partial tile and its ring are composed, and the pixel past the ring is not");
    }

    {
        auto scene = TenBitScene();
        xmx::hud_composite::Pass pass;
        if (!Check(BuildScene(bench, scene, kHudSize), "the 10-bit scene is uploaded")) return 1;
        Check(ApplyAndRead(bench, pass, scene, false, applied, pixels), "the 10-bit pass was read back");
        Check(applied && Matches(pixels, scene, false),
            "a R10G10B10A2_UNORM frame is composed like an 8-bit one");
        Check(pixels[20 * kHudSize + 20] == kTenWhite && pixels[19 * kHudSize + 19] == kTenGrey
            && pixels[18 * kHudSize + 18] == kTenBlack, "with its own texel values");
    }

    {
        auto scene = BlockScene();
        xmx::hud_composite::Pass pass;
        if (!Check(BuildScene(bench, scene, kHudSize / 2), "the mismatched scene is uploaded")) return 1;
        Check(ApplyAndRead(bench, pass, scene, false, applied, pixels), "the mismatched pass was read back");
        Check(!applied && !pass.Failed() && pass.Fault().empty(),
            "a HUD-less color of another size is declined without latching a failure");
        Check(Untouched(pixels, scene), "and the output is left untouched");
    }

    {
        auto scene = BlockScene();
        xmx::hud_composite::Pass pass;
        if (!Check(BuildScene(bench, scene, kHudSize), "the failed-pass scene is uploaded")) return 1;
        Check(!pass.Apply(bench.list.Get(), nullptr, scene.output.Get(), scene.real.Get(), scene.hudless.Get(), false),
            "a pass given no device fails to initialize");
        Check(pass.Failed() && !pass.Fault().empty(), "the failure is latched with its fault");
        Check(!pass.Prepare(bench.device.Get()), "a failed pass is not prepared on a good device");
        Check(ApplyAndRead(bench, pass, scene, false, applied, pixels), "the failed pass was read back");
        Check(!applied && pass.Failed(), "a failed pass never retries on a good device");
        Check(Untouched(pixels, scene), "and leaves the output untouched");
    }

    {
        xmx::hud_composite::Pass pass;
        Check(pass.Prepare(bench.device.Get()) && !pass.Failed(), "a pass is prepared on a good device");
        Check(pass.BoundTo(bench.device.Get()), "and is bound to it");
        Check(pass.Prepare(bench.device.Get()), "preparing again keeps it prepared");
        Bench other;
        if (!Check(OpenBench(other) && other.device.Get() != bench.device.Get(), "a second device is opened"))
            return 1;
        Check(!pass.BoundTo(other.device.Get()) && !pass.Prepare(other.device.Get()),
            "a pass bound to one device is not prepared on another");
        Check(!pass.Failed(), "and is not latched failed by it");
        const auto objects = pass.Release();
        Check(objects[0] && objects[1] && objects[2], "releasing hands over the root signature, pipeline and heap");
        Check(pass.BoundTo(other.device.Get()) && pass.Prepare(other.device.Get()) && !pass.Failed(),
            "the released pass starts fresh and prepares on the second device");
    }
    return 0;
}

DXGI_FORMAT FormatOf(ID3D12Resource* resource)
{
    return resource != nullptr ? resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
}

void FillOutput(ID3D12GraphicsCommandList* list, ID3D12Resource* output)
{
    const auto toCopy = Transition(output, kUnorderedState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopy);
    list->CopyResource(output, gFill);
    const auto back = Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, kUnorderedState);
    list->ResourceBarrier(1, &back);
}

unsigned long long HudComposed()
{
    return xmx::status::hudComposed.load();
}

bool FedHudless()
{
    return gLastExecute.nextFormat == kHudlessFormat && gLastExecute.previousFormat == kHudlessFormat
        && gLastExecute.nextBackbuffer != nullptr && gLastExecute.nextBackbuffer == gLastExecute.nextHudless
        && gLastExecute.previousBackbuffer != nullptr && gLastExecute.previousBackbuffer == gLastExecute.previousHudless;
}

bool FedBackbuffers()
{
    return gLastExecute.nextFormat == kBackbufferFormat && gLastExecute.previousFormat == kBackbufferFormat
        && gLastExecute.nextBackbuffer != nullptr && gLastExecute.nextBackbuffer != gLastExecute.nextHudless
        && gLastExecute.previousBackbuffer != gLastExecute.previousHudless;
}

bool NotFedHudless()
{
    return gLastExecute.nextBackbuffer != nullptr && gLastExecute.nextBackbuffer != gLastExecute.nextHudless
        && gLastExecute.previousBackbuffer != gLastExecute.previousHudless;
}

int HudExecute()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kWidth = 640, kHeight = 360;
    xmx::status::hudWanted.store(true);
    xmx::status::hudDebug.store(false);
    gExecuteResult = 0;

    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth / 2);
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth / 2), "a frame with a HUD-less color of another size is generated");
    Check(CountLogs("HUD composition declined") == 0, "a decline before the HUD was ever composed is not logged");

    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth);
    unsigned long long composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth), "a frame with a HUD-less color of the back buffer's size is generated");
    Check(FedHudless(), "XeFG is given the HUD-less colors, in their own format, in place of the back buffers");
    Check(HudComposed() == composed + 1, "the generated frame is counted as HUD composed");
    Check(CountLogs("HUD: XeFG interpolates the HUD-less color") == 1, "the composition is announced once");
    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth);
    Check(CountLogs("HUD: XeFG interpolates the HUD-less color") == 1, "and not again on the next frame");
    Check(CountLogs("HUD composition unavailable") == 0, "the pass was available");
    Check(CountLogs("HUD composition declined") == 0, "and nothing was declined");

    gExecuteResult = 1;
    composed = HudComposed();
    const unsigned executes = gExecutes;
    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth);
    Check(gExecutes == executes + 1, "a frame is executed with a non-zero result");
    Check(HudComposed() == composed, "a model that produced no output gets no HUD composed over it");
    gExecuteResult = 0;

    xmx::status::hudWanted.store(false);
    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth);
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth), "with xmxHud off the frame is generated");
    Check(FedBackbuffers() && HudComposed() == composed, "XeFG gets the back buffers and nothing is composed");
    Check(CountLogs("HUD composition declined") == 0, "turning xmxHud off is not logged as a decline");
    xmx::status::hudWanted.store(true);

    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth / 2);
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth / 2), "a HUD-less color of another size is generated without the HUD");
    Check(FedBackbuffers() && HudComposed() == composed, "XeFG gets the back buffers and nothing is composed");
    Check(CountLogs("HUD composition declined") == 1, "the first decline after composing is logged by its reason");
    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth / 2);
    Check(CountLogs("HUD composition declined") == 1, "and the same reason is not logged again");

    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
        "a typed sRGB HUD-less color is generated without the HUD");
    Check(FedBackbuffers() && HudComposed() == composed, "XeFG gets the back buffers and nothing is composed");
    Check(CountLogs("HUD composition declined") == 2, "a different reason is logged once more");

    rig.backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    Evaluate(rig, 800, 450, 0, 0, 800);
    Sleep(1100);
    Evaluate(rig, 800, 450, 0, 0, 800);
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, 800, 450, 0, 0, 800), "a typed sRGB back buffer is generated without the HUD");
    Check(NotFedHudless() && HudComposed() == composed, "XeFG gets the back buffers and nothing is composed");

    rig.backbufferFormat = kBackbufferFormat;
    rig.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    Evaluate(rig, 960, 540, 0, 0, 960);
    Sleep(1100);
    Evaluate(rig, 960, 540, 0, 0, 960);
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, 960, 540, 0, 0, 960), "a typed sRGB output is generated without the HUD");
    Check(NotFedHudless() && HudComposed() == composed, "XeFG gets the back buffers and nothing is composed");
    rig.outputFormat = kBackbufferFormat;

    Evaluate(rig, kWidth, kHeight);
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight), "a frame without a HUD-less color is generated");
    Check(FedBackbuffers() && HudComposed() == composed, "XeFG gets the back buffers and nothing is composed");
    Check(CountLogs("HUD composition declined") == 2, "a frame without a HUD-less color is not a decline");
    return 0;
}

int HudDevice()
{
    Rig first, second;
    if (!OpenRig(first)) return 2;
    if (!OpenRig(second)) return 2;
    if (!Check(first.device.Get() != second.device.Get(), "the two rigs have their own devices")) return 1;
    constexpr UINT kWidth = 640, kHeight = 360;
    xmx::status::hudWanted.store(true);
    xmx::status::hudDebug.store(false);
    AttachRig(first);

    Evaluate(first, kWidth, kHeight, 0, 0, kWidth);
    unsigned long long composed = HudComposed();
    Check(Evaluate(first, kWidth, kHeight, 0, 0, kWidth), "a frame is generated on the first device");
    Check(HudComposed() == composed + 1, "the HUD is composed on the first device");
    Check(CountLogs("HUD composition: the device changed") == 0, "the pass was not rebuilt yet");

    AttachRig(second);
    Evaluate(second, kWidth, kHeight, 0, 0, kWidth);
    Sleep(1100);
    Evaluate(second, kWidth, kHeight, 0, 0, kWidth);
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(second, kWidth, kHeight, 0, 0, kWidth), "a frame is generated on the second device");
    Check(HudComposed() == composed + 1, "the HUD keeps being composed on the new device");
    Check(FedHudless(), "XeFG is given the HUD-less colors on the new device");
    Check(CountLogs("HUD composition: the device changed") == 1, "the old pass was handed to the retire queue and rebuilt once");
    Check(CountLogs("HUD composition unavailable") == 0, "the device change did not latch the pass off");
    Check(Evaluate(second, kWidth, kHeight, 0, 0, kWidth), "the next frame is generated");
    Check(HudComposed() == composed + 2, "and composed");
    Check(CountLogs("HUD composition: the device changed") == 1, "without rebuilding the pass again");
    return 0;
}

bool EvaluateScene(Bench& bench, const Scene& scene, ID3D12Resource* depth, ID3D12Resource* motion, unsigned index)
{
    Parameters params;
    params.Set("DLSSG.MultiFrameCount", 2u);
    params.Set("DLSSG.MultiFrameIndex", index);
    params.resources["DLSSG.Backbuffer"] = scene.real.Get();
    params.resources["DLSSG.HUDLess"] = scene.hudless.Get();
    params.resources["DLSSG.OutputInterpolated"] = scene.output.Get();
    params.resources["DLSSG.Depth"] = depth;
    params.resources["DLSSG.MVecs"] = motion;
    return xmx::generator::Evaluate(bench.list.Get(), &params);
}

bool EvaluatePair(Bench& bench, const Scene& scene, ID3D12Resource* depth, ID3D12Resource* motion)
{
    const bool first = EvaluateScene(bench, scene, depth, motion, 1);
    return EvaluateScene(bench, scene, depth, motion, 2) && first;
}

int HudGenerated()
{
    Bench bench;
    if (!OpenBench(bench)) return 2;
    Rig rig;
    rig.device = bench.device;
    rig.allocator = bench.allocator;
    rig.list = bench.list;
    AttachRig(rig);
    xmx::status::hudWanted.store(true);
    xmx::status::hudDebug.store(false);
    gExecuteResult = 0;

    auto scene = BlockScene();
    scene.background = kFake;
    auto* device = bench.device.Get();
    const auto fill = Texture(device, kHudSize, kHudSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    const auto depth = Texture(device, kHudSize, kHudSize, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    const auto motion = Texture(device, kHudSize, kHudSize, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    const bool ready = fill && depth && motion && BuildScene(bench, scene, kHudSize, D3D12_RESOURCE_STATE_COPY_DEST)
        && Upload(bench, fill.Get(), Filled(kHudSize, kHudSize, kFake), kHudSize, kHudSize,
            D3D12_RESOURCE_STATE_COPY_SOURCE)
        && ToState(bench, depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
        && ToState(bench, motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!Check(ready, "the back buffer, the HUD-less color, the output, the fake model's colour and the inputs are uploaded"))
        return 1;
    gFill = fill.Get();

    Check(EvaluatePair(bench, scene, depth.Get(), motion.Get()), "the first pair of frames is shown");
    const unsigned executes = gExecutes;
    unsigned long long composed = HudComposed();
    Check(EvaluatePair(bench, scene, depth.Get(), motion.Get()), "the second pair of frames is shown");
    Check(gExecutes == executes + 2 && gLastExecute.nextBackbuffer != nullptr,
        "the model ran for both generated frames of the pair");
    Check(HudComposed() == composed + 2, "and the HUD was composed over each");
    std::vector<uint32_t> pixels;
    Check(ReadBack(bench, scene.output.Get(), kUnorderedState, kHudSize, kHudSize, pixels), "the shown frame is read back");
    Check(Matches(pixels, scene, false),
        "the shown frame carries the real frame's HUD over the model's colour and nothing else of the real frame");

    xmx::status::hudDebug.store(true);
    composed = HudComposed();
    Check(EvaluatePair(bench, scene, depth.Get(), motion.Get()), "a pair is shown with HUD debug on");
    Check(HudComposed() == composed + 2, "and composed");
    Check(ReadBack(bench, scene.output.Get(), kUnorderedState, kHudSize, kHudSize, pixels), "the debug frame is read back");
    Check(Matches(pixels, scene, true), "the HUD region of the shown frame is magenta and the rest is the model's colour");
    xmx::status::hudDebug.store(false);
    gFill = nullptr;
    return 0;
}

#ifdef XMX_HUD_PASS_STUB
int HudUnprepared()
{
    Rig rig;
    if (!OpenRig(rig)) return 2;
    constexpr UINT kWidth = 640, kHeight = 360;
    namespace stub = xmx::hud_composite::stub;
    xmx::status::hudWanted.store(true);
    xmx::status::hudDebug.store(false);
    stub::prepare = false;
    stub::fault = "first fault";

    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth);
    unsigned long long composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth), "a frame is generated while the pass cannot prepare");
    Check(FedBackbuffers() && HudComposed() == composed,
        "XeFG is given the back buffers, never the HUD-less colors, and nothing is composed");
    Check(CountLogs("HUD composition unavailable: first fault") == 1, "the fault is logged");
    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth);
    Check(CountLogs("HUD composition unavailable") == 1, "the same fault is not logged again");
    stub::fault = "second fault";
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth), "the next frame is generated");
    Check(FedBackbuffers(), "and still gets the back buffers");
    Check(CountLogs("HUD composition unavailable: second fault") == 1 && CountLogs("HUD composition unavailable") == 2,
        "a different fault is logged once more");
    Evaluate(rig, kWidth, kHeight, 0, 0, kWidth);
    Check(CountLogs("HUD composition unavailable") == 2, "and not repeated");

    stub::prepare = true;
    composed = HudComposed();
    gLastExecute = {};
    Check(Evaluate(rig, kWidth, kHeight, 0, 0, kWidth), "a frame is generated once the pass prepares");
    Check(FedHudless() && HudComposed() == composed + 1, "XeFG gets the HUD-less colors and the HUD is composed");
    return 0;
}
#endif
}

namespace xmx
{
void Log(const std::string& text) noexcept
{
    gLogs.push_back(text);
    std::printf("log: %s\n", text.c_str());
}

void LogSummary(const std::string& text) noexcept
{
    Log(text);
}
}

namespace xmx::context
{
int Guarded(void*, void*, void*, void*) noexcept
{
    return 0;
}

bool Create(HMODULE, const Rvas&, ID3D12Device*, void*& handle, std::string&) noexcept
{
    handle = &gHandles[gCreates++];
    return true;
}

int Initialize(HMODULE, const Rvas&, void*, InitParams& init) noexcept
{
    ++gInitializes;
    gInitFrames.push_back(init.maxFrames);
    if (init.height == gRefuseAllHeight) return -1;
    if (init.height == gRefuseMultiHeight && init.maxFrames > 1) return -1;
    return 0;
}

int Execute(HMODULE, const Rvas&, void*, ID3D12GraphicsCommandList* list, ExecuteParams& params) noexcept
{
    ++gExecutes;
    gLastExecute = {params.next.backbuffer.resource, params.next.hudless.resource, params.previous.backbuffer.resource,
        params.previous.hudless.resource, FormatOf(params.next.backbuffer.resource),
        FormatOf(params.previous.backbuffer.resource)};
    if (gFill != nullptr) FillOutput(list, params.outputs[params.firstFrame]);
    return gExecuteResult;
}
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    int code = 1;
    if (mode == L"reuse") code = Reuse();
    else if (mode == L"frames") code = Frames();
    else if (mode == L"refused") code = Refused();
    else if (mode == L"refused-alternating") code = RefusedAlternating();
    else if (mode == L"fallback") code = Fallback();
    else if (mode == L"copy-mismatch") code = CopyMismatch();
    else if (mode == L"memory-empty") code = MemoryEmpty();
    else if (mode == L"memory-device") code = MemoryDevice();
    else if (mode == L"hud-composite") code = HudComposite();
    else if (mode == L"hud-execute") code = HudExecute();
    else if (mode == L"hud-device") code = HudDevice();
    else if (mode == L"hud-generated") code = HudGenerated();
#ifdef XMX_HUD_PASS_STUB
    else if (mode == L"hud-unprepared") code = HudUnprepared();
#endif
    else std::printf("FAIL unknown mode\n");
    std::printf("XMX_GENERATOR checks=%u failures=%u\n", gChecks, gFailures);
    if (code == 2) return 2;
    return gFailures || code ? 1 : 0;
}
