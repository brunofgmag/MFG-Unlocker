#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "xmx/xmx_context.h"
#include "xmx/xmx_generator.h"
#include "xmx/xmx_status.h"

using Microsoft::WRL::ComPtr;

namespace
{
unsigned gChecks = 0, gFailures = 0;
unsigned gCreates = 0, gInitializes = 0, gExecutes = 0;
char gHandles[64];
std::vector<unsigned> gInitFrames;
std::vector<std::string> gLogs;
unsigned gRefuseMultiHeight = 0, gRefuseAllHeight = 0;

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
};

struct Rig
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::map<UINT, Textures> sized;
    xmx::Rvas rvas{};
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

bool OpenRig(Rig& rig)
{
    const HRESULT created = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&rig.device));
    std::printf("D3D12CreateDevice at feature level 11_0 returned 0x%08lx\n", static_cast<unsigned long>(created));
    if (FAILED(created)) return false;
    if (FAILED(rig.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&rig.allocator)))
        || FAILED(rig.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rig.allocator.Get(), nullptr,
            IID_PPV_ARGS(&rig.list))))
        return false;
    HMODULE fake = reinterpret_cast<HMODULE>(&gHandles);
    xmx::generator::Attach({fake, &rig.rvas, xmx::kMaxFrames});
    return true;
}

bool Evaluate(Rig& rig, UINT width, UINT height, int depthInverted = 0, unsigned frames = 0)
{
    auto& textures = rig.sized[width];
    if (!textures.backbuffer)
    {
        auto* device = rig.device.Get();
        textures.backbuffer = Texture(device, width, height, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_NONE);
        textures.interpolated = Texture(device, width, height, DXGI_FORMAT_R10G10B10A2_UNORM,
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

int Execute(HMODULE, const Rvas&, void*, ID3D12GraphicsCommandList*, ExecuteParams&) noexcept
{
    ++gExecutes;
    return 0;
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
    else std::printf("FAIL unknown mode\n");
    std::printf("XMX_GENERATOR checks=%u failures=%u\n", gChecks, gFailures);
    if (code == 2) return 2;
    return gFailures || code ? 1 : 0;
}
