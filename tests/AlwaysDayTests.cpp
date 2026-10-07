// Execute production bridge instructions only on synthetic executable pages.
#include "../GameMod/AlwaysDay.h"
#include <cstdio>
#include <cmath>
extern "C" void InvokeAlwaysDayTime(void*, void*, uintptr_t, uintptr_t);
extern "C" void AlwaysDayTimeContinue();
extern "C" float InvokeAlwaysDayPhase(void*, int, float, uintptr_t);
extern "C" void AlwaysDayPhaseReturn();
extern "C" void InvokeAlwaysDayRender(void*, void*, void*);
extern "C" void AlwaysDayRenderContinue();
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Allocation {
    unsigned char* memory;
    explicit Allocation(size_t size = nametag::kAllocationSize) : memory(static_cast<unsigned char*>(
        VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) { Check(memory != nullptr, "allocation"); }
    ~Allocation() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
    uintptr_t Address(size_t offset = 0) const { return reinterpret_cast<uintptr_t>(memory + offset); }
};
struct TimeObservation {
    int32_t tick = 18000; float partial = 0.75f;
    uintptr_t tickAfter = 0, product = 0, r11 = 0;
    uint32_t partialAfter[4]{};
};
// Run the pinned production instructions, including interpolation and smoothing,
// rather than stopping immediately after the hook's replacement multiply.
void CheckNativeNoon(const alwaysday::BridgeData& templateState) {
    using namespace alwaysday;
    Allocation native(0x42000), emitted;
    auto* state = new (emitted.memory + nametag::kDataOffset) BridgeData{};
    std::memcpy(state, &templateState, sizeof(*state));
    state->continuation = native.Address(10);
    BuildBridge(emitted.memory, state);
    // The native leaf uses RIP-relative constants and a 65536-entry sine table.
    // Relocate its exact profiled bytes to a synthetic image with equivalent data.
    for (size_t copy : {size_t{0}, size_t{512}}) {
        std::memcpy(native.memory + copy, kFunctionBytes, sizeof(kFunctionBytes));
        for (size_t i = 0; i + 8 <= sizeof(kFunctionBytes); ++i) {
            if (kFunctionBytes[i] != 0xF3 || kFunctionBytes[i+1] != 0x0F ||
                (kFunctionBytes[i+3] & 0xC7) != 5) continue;
            int32_t relative; std::memcpy(&relative, kFunctionBytes + i + 4, 4);
            const uintptr_t target = kFunctionRva + i + 8 + relative;
            float value = 0;
            switch (target) {
            case 0xE7FEF84: value = 24000.0f; break;
            case 0xE7E14EC: value = -0.25f; break;
            case 0xE5A7060: value = 1.0f; break;
            case 0xE5E9578: value = -1.0f; break;
            case 0xE6022B0: value = 3.1415927410125732f; break;
            case 0xE6AE484: value = 10430.3779296875f; break;
            case 0xE6AE488: value = 16384.0f; break;
            case 0xE6022D0: value = -0.5f; break;
            case 0xE6022AC: value = 3.0f; break;
            default: Check(false, "unrecognized native constant");
            }
            const size_t offset = 1024 + i * 4;
            std::memcpy(native.memory + offset, &value, 4);
            relative = static_cast<int32_t>(offset - (copy + i + 8));
            std::memcpy(native.memory + copy + i + 4, &relative, 4);
        }
        for (size_t i = 0; i + 7 <= sizeof(kFunctionBytes); ++i) {
            if (kFunctionBytes[i] == 0x48 && kFunctionBytes[i+1] == 0x8D && kFunctionBytes[i+2] == 0x0D) {
                const int32_t relative = static_cast<int32_t>(4096 - (copy + i + 7));
                std::memcpy(native.memory + copy + i + 3, &relative, 4);
            }
        }
    }
    auto* sine = reinterpret_cast<float*>(native.memory + 4096);
    for (size_t i = 0; i < 65536; ++i) sine[i] = static_cast<float>(std::sin(i * 6.283185307179586 / 65536));
    nametag::Patch site{};
    Check(nametag::Jump(site, native.Address(3), kOriginal, sizeof(kOriginal), emitted.Address()), "native phase hook");
    std::memcpy(native.memory + 3, site.replacement.data(), site.size);
    DWORD previous;
    Check(VirtualProtect(native.memory, 0x42000, PAGE_EXECUTE_READ, &previous) &&
        VirtualProtect(emitted.memory, nametag::kDataOffset, PAGE_EXECUTE_READ, &previous) &&
        FlushInstructionCache(GetCurrentProcess(), native.memory, 0x42000) &&
        FlushInstructionCache(GetCurrentProcess(), emitted.memory, nametag::kDataOffset), "native phase executable");
    const float noon = InvokeAlwaysDayPhase(native.memory + 512, 6000, 0, 0);
    Check(noon == 0.0f, "native noon has zero celestial rotation");
    Check(InvokeAlwaysDayPhase(native.memory + 512, 18000, 0, 0) == 0.5f,
        "native midnight places the opposite celestial body overhead");
    for (int mode : {0, 1}) {
        state->enabled = mode;
        for (size_t i = 0; i < kCallerCount; ++i) {
            const uintptr_t rva = kRenderCallers[i].returnRva;
            const uintptr_t parent = rva == kBrightnessReturnRva ? kLightImageReturnRva :
                rva == kSunriseReturnRva ? kSunriseParents[0] :
                rva == kSkyColourReturnRva ? kSkyColourParent :
                rva == kDirectionReturnRva ? kDirectionParents[0] : kSunFacingParents[0];
            const uintptr_t saved = state->callers[i];
            state->callers[i] = reinterpret_cast<uintptr_t>(&AlwaysDayPhaseReturn);
            for (int tick : {0, 6000, 12000, 18000, 23999, 24000, 48000}) {
                const float original = InvokeAlwaysDayPhase(native.memory + 512, tick, 0.75f, parent);
                const float actual = InvokeAlwaysDayPhase(native.memory, tick, 0.75f, parent);
                Check(actual == (mode ? noon : original), "complete native cycle stays at noon and restores when disabled");
                if (rva == kBrightnessReturnRva || rva == kSunriseReturnRva ||
                    rva == kSkyColourReturnRva || rva == kDirectionReturnRva || rva == kSunFacingReturnRva)
                    Check(InvokeAlwaysDayPhase(native.memory, tick, 0.75f, 0x1234) == original,
                        "complete native cycle leaves foreign consumers unchanged");
            }
            state->callers[i] = saved;
        }
        Check(InvokeAlwaysDayPhase(native.memory, 18000, 0.75f, 0) ==
            InvokeAlwaysDayPhase(native.memory + 512, 18000, 0.75f, 0), "unapproved native caller unchanged");
    }
}
struct RenderObservation { uint32_t rgba[4]{}; uintptr_t rax, rcx, rdx, r8, r11, flags; };
}
int main() {
try {
    using namespace alwaysday;
    Check(!ValidateProfile(0, true) && !ValidateProfile(1, false) && !Toggle() && !EnableByDefault(),
        "unsupported/toggle refused");
    Allocation executable;
    auto* state = new (executable.memory + nametag::kDataOffset) BridgeData{};
    state->continuation = reinterpret_cast<uintptr_t>(&AlwaysDayTimeContinue);
    state->starsContinuation = state->cloudContinuation = state->celestialContinuation = reinterpret_cast<uintptr_t>(&AlwaysDayRenderContinue);
    state->lightImageCaller = kLightImageReturnRva;
    state->skyBrightnessCaller = kSkyBrightnessReturnRva;
    for (size_t i = 0; i < 3; ++i) state->sunriseCallers[i] = kSunriseParents[i];
    state->skyColourCaller = kSkyColourParent;
    state->cameraSkyColourCaller = kCameraSkyColourParent;
    state->cameraSunriseCaller = kCameraSunriseParent;
    for (size_t i = 0; i < 2; ++i) {
        state->directionCallers[i] = kDirectionParents[i];
        state->sunFacingCallers[i] = kSunFacingParents[i];
    }
    for (size_t i = 0; i < kCallerCount; ++i) state->callers[i] = kRenderCallers[i].returnRva;
    BuildBridge(executable.memory, state);
    DWORD previous = 0;
    Check(VirtualProtect(executable.memory, nametag::kDataOffset, PAGE_EXECUTE_READ, &previous) != FALSE, "RX bridge");
    Check(FlushInstructionCache(GetCurrentProcess(), executable.memory, nametag::kDataOffset) != FALSE, "flush");
    bridge = executable.memory; data = state; readiness = 2;
    Check(EnableByDefault() && enabled == 1 && state->enabled == 1, "ready feature enabled by default");
    for (int mode : {0, 1, 0}) {
        if (mode != enabled) Check(Toggle(), "toggle");
        for (int tick : {0, 6000, 12000, 18000, 23999})
        for (size_t i = 0; i <= kCallerCount; ++i) {
            TimeObservation out{};
            out.tick = tick;
            const uintptr_t caller = i < kCallerCount ? state->callers[i] : 0x1234;
            const bool noon = mode && i < kCallerCount;
            const uintptr_t parent = caller == kSunriseReturnRva ? kSunriseParents[0] :
                caller == kSkyColourReturnRva ? kSkyColourParent :
                caller == kDirectionReturnRva ? kDirectionParents[0] :
                caller == kSunFacingReturnRva ? kSunFacingParents[0] : kLightImageReturnRva;
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
            std::make_pair(kSkyColourReturnRva, kSkyColourParent),
            std::make_pair(kSkyColourReturnRva, kCameraSkyColourParent),
            std::make_pair(kSunriseReturnRva, kCameraSunriseParent),
            std::make_pair(kDirectionReturnRva, kDirectionParents[0]),
            std::make_pair(kDirectionReturnRva, kDirectionParents[1]),
            std::make_pair(kSunFacingReturnRva, kSunFacingParents[0]),
            std::make_pair(kSunFacingReturnRva, kSunFacingParents[1])}) {
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
    CheckNativeNoon(*state);
    Check(StarsOverrideCount() == 1 && CloudOverrideCount() == 1 && CelestialOverrideCount() == 1, "enabled-only counters");
    for (size_t i = 0; i < kCallerCount; ++i) {
        const uintptr_t ret = kRenderCallers[i].returnRva;
        Check(OverrideCount(i) == (ret == kBrightnessReturnRva ? 7 : ret == kSunriseReturnRva ? 9 :
            ret == kSkyColourReturnRva ? 7 :
            ret == kDirectionReturnRva || ret == kSunFacingReturnRva ? 7 : 5), "noon counts");
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
    std::puts("Always day tests passed: complete native day cycle, Fancy rotation/camera parents, render-only noon, celestial angle, stars, cloud RGB/alpha and four-site restoration.");
    return 0;
} catch (const std::exception& error) { std::fprintf(stderr, "Always day test failed: %s\n", error.what()); return 1; }
}
