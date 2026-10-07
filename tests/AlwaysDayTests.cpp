// Execute production bridge instructions only on synthetic executable pages.
#include "../GameMod/AlwaysDay.h"
#include <cstdio>
extern "C" void InvokeAlwaysDayTime(void*, void*, uintptr_t, uintptr_t);
extern "C" void AlwaysDayTimeContinue();
extern "C" void InvokeAlwaysDayRender(void*, void*, void*);
extern "C" void AlwaysDayRenderContinue();
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Allocation {
    unsigned char* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, nametag::kAllocationSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Allocation() { Check(memory != nullptr, "allocation"); }
    ~Allocation() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
    uintptr_t Address(size_t offset = 0) const { return reinterpret_cast<uintptr_t>(memory + offset); }
};
struct TimeObservation {
    int32_t tick = 18000; float partial = 0.75f;
    uintptr_t tickAfter = 0, product = 0, r11 = 0;
    uint32_t partialAfter[4]{};
};
struct RenderObservation { uint32_t rgba[4]{}; uintptr_t rax, rcx, rdx, r8, r11, flags; };
}
int main() {
try {
    using namespace alwaysday;
    Check(!ValidateProfile(0, true) && !ValidateProfile(1, false) && !Toggle(), "unsupported/toggle refused");
    Allocation executable;
    auto* state = new (executable.memory + nametag::kDataOffset) BridgeData{};
    state->continuation = reinterpret_cast<uintptr_t>(&AlwaysDayTimeContinue);
    state->starsContinuation = state->cloudContinuation = state->celestialContinuation = reinterpret_cast<uintptr_t>(&AlwaysDayRenderContinue);
    state->lightImageCaller = kLightImageReturnRva;
    state->skyBrightnessCaller = kSkyBrightnessReturnRva;
    for (size_t i = 0; i < 3; ++i) state->sunriseCallers[i] = kSunriseParents[i];
    state->skyColourCaller = kSkyColourParent;
    for (size_t i = 0; i < kCallerCount; ++i) state->callers[i] = kRenderCallers[i].returnRva;
    BuildBridge(executable.memory, state);
    DWORD previous = 0;
    Check(VirtualProtect(executable.memory, nametag::kDataOffset, PAGE_EXECUTE_READ, &previous) != FALSE, "RX bridge");
    Check(FlushInstructionCache(GetCurrentProcess(), executable.memory, nametag::kDataOffset) != FALSE, "flush");
    bridge = executable.memory; data = state; readiness = 2;
    for (int mode : {0, 1, 0}) {
        if (mode != enabled) Check(Toggle(), "toggle");
        for (int tick : {0, 6000, 12000, 18000, 23999})
        for (size_t i = 0; i <= kCallerCount; ++i) {
            TimeObservation out{};
            out.tick = tick;
            const uintptr_t caller = i < kCallerCount ? state->callers[i] : 0x1234;
            const bool noon = mode && i < kCallerCount;
            const uintptr_t parent = caller == kSunriseReturnRva ? kSunriseParents[0] :
                caller == kSkyColourReturnRva ? kSkyColourParent : kLightImageReturnRva;
            InvokeAlwaysDayTime(bridge, &out, caller, parent);
            Check(out.tick == tick && out.partial == 0.75f, "source clock inputs unchanged");
            Check(out.tickAfter == static_cast<uintptr_t>(noon ? 6000 : tick) &&
                out.product == out.tickAfter * 0x57619F1 && out.r11 == 0x33445566778899AA,
                "allowlisted noon input, native multiply and R11");
            const uint32_t partial = noon ? 0 : 0x3F400000;
            Check(out.partialAfter[0] == partial, "partial tick");
        }
        for (const auto& nested : {std::make_pair(kBrightnessReturnRva, kLightImageReturnRva),
            std::make_pair(kBrightnessReturnRva, kSkyBrightnessReturnRva),
            std::make_pair(kSunriseReturnRva, kSunriseParents[0]),
            std::make_pair(kSunriseReturnRva, kSunriseParents[1]),
            std::make_pair(kSunriseReturnRva, kSunriseParents[2]),
            std::make_pair(kSkyColourReturnRva, kSkyColourParent)}) {
            TimeObservation out{}, unrelated{};
            InvokeAlwaysDayTime(bridge, &out, nested.first, nested.second);
            Check(out.tickAfter == (mode ? 6000 : 18000), "approved renderer parent");
            for (uintptr_t foreign : {uintptr_t{0x1234}, kSunriseReturnRva, kSkyColourReturnRva}) {
                InvokeAlwaysDayTime(bridge, &unrelated, nested.first, foreign);
                Check(unrelated.tickAfter == 18000 && unrelated.partialAfter[0] == 0x3F400000, "shared helper foreign parent refused");
            }
        }
        for (int kind : {0, 1, 2}) {
            const bool cloud = kind == 1, celestial = kind == 2;
            std::array<unsigned char, 0xB0> source{}; source.fill(0xA5);
            const uint32_t colour[] = {0x3E800000, 0x3F000000, 0x3E000000, 0x3F200000};
            std::memcpy(source.data(), colour, 16);
            std::memcpy(source.data() + 0x88, colour, 16);
            auto expected = source;
            if (!cloud) {
                const uint32_t intensity = mode ? 0 : colour[0];
                std::memcpy(expected.data() + (celestial ? 0x14 : 0x10), &intensity, 4);
            }
            RenderObservation out{};
            auto* entry = bridge + (cloud ? kCloudBridgeOffset : celestial ? kCelestialBridgeOffset : kStarsBridgeOffset);
            InvokeAlwaysDayRender(entry, source.data(), &out);
            Check(source == expected, "only temporary star/angle output written; source and guards preserved");
            const uint32_t rgb = mode && cloud ? 0x3F800000 : colour[0];
            if (cloud) Check(out.rgba[0] == rgb && out.rgba[1] == (mode ? rgb : colour[1]) &&
                out.rgba[2] == (mode ? rgb : colour[2]) && out.rgba[3] == colour[3], "white RGB, unchanged alpha, disabled colour restored");
            else Check(out.rgba[0] == (mode ? 0 : colour[0]), "stars hidden and celestial angle at noon when enabled");
            Check(out.rax == 0x1122334455667788 && out.r11 == 0x33445566778899AA &&
                out.rcx == reinterpret_cast<uintptr_t>(entry) && out.rdx == reinterpret_cast<uintptr_t>(source.data()) &&
                out.r8 == reinterpret_cast<uintptr_t>(&out) && (out.flags & 0x8D5) == 0x44, "render registers/flags");
        }
    }
    Check(StarsOverrideCount() == 1 && CloudOverrideCount() == 1 && CelestialOverrideCount() == 1, "enabled-only counters");
    for (size_t i = 0; i < kCallerCount; ++i) {
        const uintptr_t ret = kRenderCallers[i].returnRva;
        Check(OverrideCount(i) == (ret == kBrightnessReturnRva ? 7 : ret == kSunriseReturnRva ? 8 :
            ret == kSkyColourReturnRva ? 6 : 5), "noon counts");
    }
    Allocation original;
    const std::array<nametag::Patch*, 4> sites{&patch, &starsPatch, &cloudPatch, &celestialPatch};
    const unsigned char* originals[]{kOriginal, kStarsOriginal, kCloudOriginal, kCelestialOriginal};
    const size_t sizes[]{sizeof(kOriginal), sizeof(kStarsOriginal), sizeof(kCloudOriginal), sizeof(kCelestialOriginal)};
    for (size_t i = 0; i < sites.size(); ++i) {
        std::memcpy(original.memory + 64 * i, originals[i], sizes[i]);
        Check(nametag::Jump(*sites[i], original.Address(64 * i), originals[i], sizes[i], original.Address(512)), "prepare site");
    }
    Check(VirtualProtect(original.memory, 4096, PAGE_EXECUTE_READ, &previous) != FALSE, "RX originals");
    Check(ChangePatch(true) && ChangePatch(false) && ChangePatch(true), "four-site install/restore/reinstall");
    VirtualProtect(original.memory, 4096, PAGE_READWRITE, &previous);
    original.memory[128] = 0xCC;
    VirtualProtect(original.memory, 4096, PAGE_EXECUTE_READ, &previous);
    Check(!Shutdown() && bridge && original.memory[128] == 0xCC && !Toggle() &&
        patch.installed && starsPatch.installed && celestialPatch.installed, "foreign cloud patch refuses all restoration and unload");
    VirtualProtect(original.memory, 4096, PAGE_READWRITE, &previous);
    std::memcpy(original.memory + 128, cloudPatch.replacement.data(), cloudPatch.size);
    VirtualProtect(original.memory, 4096, PAGE_EXECUTE_READ, &previous);
    executable.memory = nullptr;
    Check(Shutdown() && !bridge && !data && StarsOverrideCount() == 1 && CloudOverrideCount() == 1 &&
        CelestialOverrideCount() == 1, "shutdown drains bridge");
    for (size_t i = 0; i < sites.size(); ++i) Check(nametag::Matches(sites[i]->address, originals[i], sizes[i]), "original code restored");
    MEMORY_BASIC_INFORMATION protection{};
    VirtualQuery(original.memory, &protection, sizeof(protection));
    Check(protection.Protect == PAGE_EXECUTE_READ, "RX protection restored");
    std::puts("Always day tests passed: render-only noon/parents, celestial angle, stars, cloud RGB/alpha and four-site restoration.");
    return 0;
} catch (const std::exception& error) { std::fprintf(stderr, "Always day test failed: %s\n", error.what()); return 1; }
}
