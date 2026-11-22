// Additional provider-proxy cases; existing D3D12 test recordings remain unchanged.
#define main XefgLegacyMain
#include "cuda_route_tests.cpp"
#undef main
#include "xess/cuda/provider_list.h"
#include "proxy_recording.h"
#include <set>

struct ProbeLog { std::vector<std::pair<ID3D12GraphicsCommandList*, xefg_cuda::DispatchSample>> samples; };

void RecordProbe(ID3D12GraphicsCommandList* recordedOn, const xefg_cuda::DispatchSample& sample, void* context) noexcept
{
    static_cast<ProbeLog*>(context)->samples.emplace_back(recordedOn, sample);
}

std::vector<uint32_t> ProbeRun(Gpu& g, const Spec& spec, const Case& c, Program& p, xefg_cuda::ProviderList& proxy, ID3D12PipelineState* pso, int mode)
{
    if (!proxy.Bind(g.list.Get())) throw std::runtime_error("probe Bind refused");
    auto output = RunProxy(g, spec, c, p, pso, &proxy, mode);
    proxy.Unbind();
    return output;
}

size_t ProbeFailures(Gpu& g, const Spec& spec, const Case& c, Program& p, xefg_cuda::ProviderList& proxy, const std::vector<uint32_t>& reference)
{
    ProbeLog log; const xefg_cuda::DispatchProbe probe{RecordProbe, &log};
    size_t failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL probe %s %s\n", c.label.c_str(), what); } };
    auto matches = [&](const std::vector<uint32_t>& output, const char* what) {
        size_t poison{}; expect(!Mismatches(reference, output, poison), what);
    };
    const auto layer = xefg_cuda::LayerOf(p.routed.Get());
    xefg_cuda::SetDispatchProbe(&probe);
    matches(ProbeRun(g, spec, c, p, proxy, p.routed.Get(), 0), "launched output");
    matches(ProbeRun(g, spec, c, p, proxy, p.routed.Get(), 1), "stale output");
    matches(ProbeRun(g, spec, c, p, proxy, p.reference.Get(), 0), "reference output");
    xefg_cuda::SetDispatchProbe(nullptr);
    expect(log.samples.size() == 3, "three samples while the probe is set");
    if (log.samples.size() == 3) {
        const auto& [launchedList, launched] = log.samples[0];
        expect(launched.launched && launched.layer == layer && layer != 0, "launched sample flags");
        expect(launched.pipeline == p.routed.Get(), "launched sample pipeline");
        expect(launched.x == c.groupsX && launched.y == c.groupsY && launched.z == c.groupsZ, "launched sample groups");
        expect(launchedList == g.list.Get(), "launched sample list");
        const auto& [staleList, stale] = log.samples[1];
        expect(!stale.launched && stale.layer == layer, "stale sample flags");
        expect(stale.pipeline == p.routed.Get(), "stale sample pipeline");
        expect(stale.x == c.groupsX && stale.y == c.groupsY && stale.z == c.groupsZ, "stale sample groups");
        expect(staleList == g.list.Get(), "stale sample list");
        const auto& [referenceList, plain] = log.samples[2];
        expect(!plain.launched && plain.layer == 0, "reference sample flags");
        expect(plain.pipeline == p.reference.Get(), "reference sample pipeline");
        expect(referenceList == g.list.Get(), "reference sample list");
    }
    matches(ProbeRun(g, spec, c, p, proxy, p.routed.Get(), 0), "unprobed output");
    expect(log.samples.size() == 3, "no sample after the probe is cleared");
    return failures;
}

int main(int argc, char** argv)
{
    try {
        std::vector<fs::path> dirs; fs::path fatbinPath;
        std::string test = "equivalence"; bool dred = false, agility = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&] { if (++i == argc) throw std::runtime_error("missing argument"); return std::string(argv[i]); };
            if (arg == "--fatbin") fatbinPath = value();
            else if (arg == "--programs") dirs.emplace_back(value());
            else if (arg == "--test") test = value();
            else if (arg == "--dred") dred = true;
            else if (arg == "--expect-agility") agility = true;
            else if (arg != "--proxy-list") throw std::runtime_error("unknown argument " + arg);
        }
        if (dred) {
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
            Check(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)), "DRED settings");
            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        Gpu g; g.MakeList(false);
        // Agility moves from its class table to the list's own table on the first Reset.
        // Establish that runtime-owned transition before measuring proxy table writes.
        g.Submit();
        if (agility) {
            wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const auto expected = fs::path(exe).parent_path() / L"D3D12" / L"D3D12Core.dll";
            if (_wcsicmp(LoadedRuntime().c_str(), expected.c_str())) throw std::runtime_error("Agility runtime not loaded");
        }
        std::map<std::string, std::vector<uint8_t>> bySha;
        for (const auto& dir : dirs) for (const auto& entry : fs::directory_iterator(dir)) if (entry.is_regular_file()) {
            auto data = ReadFile(entry.path()); if (!data.empty()) bySha.emplace(Sha256(data), std::move(data));
        }
        std::map<std::string, Program> programs;
        for (const auto& layer : xefg_cuda::Layers()) {
            if (!bySha.count(layer.sha256)) throw std::runtime_error("missing program fixture");
            auto& p = programs[layer.program]; p.dxil = bySha.at(layer.sha256);
            Check(g.device->CreateRootSignature(0, p.dxil.data(), p.dxil.size(), IID_PPV_ARGS(&p.root)), "root");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
            Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p.reference)), "reference pipeline");
        }
        const auto fatbin = ReadFile(fatbinPath); std::string reason;
        if (!xefg_cuda::Install(g.device.Get(), GetModuleHandleW(nullptr), fatbin, reason, false)) throw std::runtime_error("install: " + reason);
        for (auto& [name, p] : programs) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
            Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p.routed)), "routed pipeline");
            if (!xefg_cuda::LayerOf(p.routed.Get())) throw std::runtime_error("unrecognized routed pipeline");
        }
        if (test == "tokens") {
            std::set<ID3D12PipelineState*> seen;
            std::vector<ComPtr<ID3D12PipelineState>> keep;
            uint64_t previous = 0; ComPtr<ID3D12PipelineState> first;
            for (int context = 0; context < 20; ++context) {
                const auto token = xefg_cuda::NewPipelineToken();
                xefg_cuda::PipelineScope scope(token);
                for (auto& [name, p] : programs) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
                    ComPtr<ID3D12PipelineState> pipeline;
                    Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "token pipeline");
                    if (!xefg_cuda::LayerOf(pipeline.Get())) throw std::runtime_error("token pipeline unrecognized");
                    seen.insert(pipeline.Get()); keep.push_back(pipeline);
                    if (!first) first = pipeline;
                }
                if (previous) {
                    auto survivor = keep.back(); xefg_cuda::ForgetPipelines(previous);
                    if (!xefg_cuda::LayerOf(survivor.Get()) || (context == 1 && xefg_cuda::LayerOf(first.Get())))
                        throw std::runtime_error("pipeline token isolation failed");
                }
                previous = token;
            }
            xefg_cuda::ForgetPipelines(previous);
            if (seen.size() <= 256) throw std::runtime_error("pipeline fixture did not create more than 256 identities");
            std::printf("PROXY_TOKENS_PASS distinct=%zu contexts=20\n", seen.size()); return 0;
        }
        if (test == "tokens-offthread") {
            // cudaheld.1 (issue 174): XeFG builds a context's pipelines on its own threads, where no PipelineScope is
            // open. They must still belong to the context opened last, and go when that context is forgotten:
            // before the fix they stayed under token 0 and the table filled after nine contexts.
            size_t distinct = 0;
            for (int context = 0; context < 24; ++context) {
                const auto token = xefg_cuda::NewPipelineToken();
                std::vector<ComPtr<ID3D12PipelineState>> made;
                std::string failure;
                const auto build = [&] {
                    try {
                        for (auto& [name, p] : programs) {
                            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
                            ComPtr<ID3D12PipelineState> pipeline;
                            Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "off-thread pipeline");
                            made.push_back(pipeline);
                        }
                    } catch (const std::exception& error) { failure = error.what(); }
                };
                {
                    xefg_cuda::PipelineScope scope(token);   // the provider's thread, around the context's creation
                    std::thread(build).join();               // XeFG's thread, inside the scope's lifetime
                }
                const size_t inScope = made.size();
                std::thread(build).join();                   // and a late compile, after the scope closed
                if (!failure.empty()) throw std::runtime_error(failure);
                for (const auto& pipeline : made)
                    if (!xefg_cuda::LayerOf(pipeline.Get())) throw std::runtime_error("off-thread pipeline unrecognized (table full?)");
                distinct += made.size();
                const ComPtr<ID3D12PipelineState> early = made.front(), late = made.back();
                if (inScope == 0 || inScope == made.size()) throw std::runtime_error("off-thread fixture made no pipelines");
                xefg_cuda::ForgetPipelines(token);
                if (xefg_cuda::LayerOf(early.Get()) || xefg_cuda::LayerOf(late.Get()))
                    throw std::runtime_error("off-thread pipelines survived their context");
            }
            for (auto& [name, p] : programs)
                if (!xefg_cuda::LayerOf(p.routed.Get())) throw std::runtime_error("an unscoped pipeline was forgotten with a context");
            if (distinct <= 256) throw std::runtime_error("off-thread fixture did not create more than 256 identities");
            std::printf("PROXY_TOKENS_OFFTHREAD_PASS distinct=%zu contexts=24\n", distinct); return 0;
        }
        if (test == "tokens-reclaim") {
            // cudaheld.1: pipelines no context owns (the XeFG output route opens no scope) whose creator released
            // them. Once the table is full they are reclaimed instead of refusing every later pipeline.
            size_t created = 0;
            for (int round = 0; round < 24; ++round)
                for (auto& [name, p] : programs) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
                    ComPtr<ID3D12PipelineState> pipeline;
                    Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "reclaim pipeline");
                    if (!xefg_cuda::LayerOf(pipeline.Get())) throw std::runtime_error("pipeline unrecognized once the table filled");
                    ++created;   // released here: only the route still holds it
                }
            for (auto& [name, p] : programs)
                if (!xefg_cuda::LayerOf(p.routed.Get())) throw std::runtime_error("a pipeline still in use was reclaimed");
            if (created <= 256) throw std::runtime_error("reclaim fixture did not create more than 256 pipelines");
            std::printf("PROXY_TOKENS_RECLAIM_PASS created=%zu\n", created); return 0;
        }
        xefg_cuda::ProviderList proxy, wrapper;
        void** const table = *reinterpret_cast<void***>(g.list.Get());
        const auto tableSnapshot = std::vector<void*>(table, table + 67);
        size_t cases = 0, failures = 0;
        for (const auto& spec : kSpecs) {
            auto& p = programs.at(spec.name);
            for (const auto size : {std::pair{37u,29u},std::pair{101u,67u},std::pair{16u,8u}})
                for (int mode = 0; mode < 5; ++mode) for (int realistic = 0; realistic < 2; ++realistic) {
                    if (mode == 4 && (spec.family != Family::Flow || spec.nout != 8)) continue;
                    if (test != "equivalence" && cases) break;
                    const auto c = Make(spec, size.first, size.second, 99, realistic != 0, mode);
                    const auto reference = RunProxy(g, spec, c, p, p.reference.Get(), nullptr, 0);
                    if (test == "probe") { failures += ProbeFailures(g, spec, c, p, proxy, reference); ++cases; continue; }
                    if (!proxy.Bind(g.list.Get())) throw std::runtime_error("proxy Bind refused");
                    if (test == "stale") {
                        RunProxy(g, spec, c, p, p.routed.Get(), &proxy, 0);
                        proxy.Unbind(); if (!proxy.Bind(g.list.Get())) throw std::runtime_error("second Bind refused");
                    }
                    if (test == "escape") {
                        void* forwarded{}; const auto hr = proxy.QueryInterface(__uuidof(ID3D12GraphicsCommandList4), &forwarded);
                        void* actual{}; const auto expected = g.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), &actual);
                        if (hr != expected || forwarded != actual) throw std::runtime_error("unknown IID did not forward");
                        if (forwarded) static_cast<IUnknown*>(forwarded)->Release();
                        if (actual) static_cast<IUnknown*>(actual)->Release();
                    }
                    xefg_cuda::ProviderList* recorder = &proxy;
                    if (test == "wrapped") {
                        proxy.Unbind();
                        if (!wrapper.Bind(g.list.Get())) throw std::runtime_error("wrapper Bind refused");
                        void* escaped{}; wrapper.QueryInterface(__uuidof(ID3D12GraphicsCommandList4), &escaped);
                        if (escaped) static_cast<IUnknown*>(escaped)->Release();
                        if (proxy.Bind(wrapper.Interface())) throw std::runtime_error("wrapped list admitted");
                        recorder = &wrapper;
                    }
                    const auto before = xefg_cuda::Snapshot();
                    const auto output = RunProxy(g, spec, c, p, p.routed.Get(), recorder, test == "stale" ? 1 : 0);
                    const auto after = xefg_cuda::Snapshot();
                    proxy.Unbind(); wrapper.Unbind();
                    const auto launches = test == "stale" || test == "escape" || test == "wrapped" ? 0 : 1;
                    const auto fallbacks = test == "stale" ? 1 : 0;
                    size_t poison{}; const auto bad = Mismatches(reference, output, poison);
                    const bool ok = !bad && after.launches - before.launches == launches && after.fallbacks - before.fallbacks == fallbacks;
                    ++cases; failures += !ok;
                    if (!ok) std::printf("FAIL %s bad=%zu launches=%llu fallbacks=%llu\n", c.label.c_str(), bad,
                        after.launches-before.launches, after.fallbacks-before.fallbacks);
                }
            if (test != "equivalence") break;
        }
        if (*reinterpret_cast<void***>(g.list.Get()) != table || std::vector<void*>(table,table+67) != tableSnapshot) {
            std::printf("TABLE_DIAGNOSTIC before=%p after=%p\n", table, *reinterpret_cast<void***>(g.list.Get()));
            for (size_t slot = 0; slot < 67; ++slot) if (table[slot] != tableSnapshot[slot])
                std::printf("TABLE_SLOT %zu before=%p after=%p\n", slot, tableSnapshot[slot], table[slot]);
            throw std::runtime_error("proxy wrote a command list table");
        }
        std::printf("PROXY_%s cases=%zu failures=%zu table_slots=67 untouched=true\n", test.c_str(), cases, failures);
        return failures ? 1 : 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"PROXY_SETUP_FAILURE %s\n",error.what()); return 2; }
}
