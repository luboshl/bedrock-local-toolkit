// Runs only in this test process, with synthetic options and executable pages.
// No Minecraft process is opened and no game module is loaded.
#include "../GameMod/Nametag.h"
#include <cstdio>
#include <stdexcept>

extern "C" void InvokeOwnBridge(void*, uintptr_t, uintptr_t, void*);
extern "C" void InvokeDepthBridge(void*, uintptr_t, uintptr_t, void*);
extern "C" void OwnContinueTest();
extern "C" void OwnSkipTest();
extern "C" void DepthContinueTest();
extern "C" bool ClobberTrue();
extern "C" bool ClobberFalse();
extern "C" void InvokeHudBridge(void*, uintptr_t, uintptr_t, void*);
extern "C" void InvokeLoopExitBridge(void*, uintptr_t, uintptr_t, void*);
extern "C" void HudContinueTest();
extern "C" void LoopContinueTest();
extern "C" void OtherLabelsTest();
extern "C" void HudCleanupTest();
extern "C" void InvokeNamesMaskBridge(void*, uintptr_t, uintptr_t, void*);
extern "C" void NamesMaskContinueTest();

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    struct Allocation
    {
        unsigned char* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Allocation() { Check(memory != nullptr, "allocate test pages"); }
        ~Allocation() { VirtualFree(memory, 0, MEM_RELEASE); }
        uintptr_t Address(size_t offset = 0) const { return reinterpret_cast<uintptr_t>(memory + offset); }
        void Pointer(size_t offset, uintptr_t value) { std::memcpy(memory + offset, &value, 8); }
    };
    struct Observation
    {
        uintptr_t registers[7]{};
        uintptr_t flags = 0;
        uint32_t xmm[24]{};
        unsigned char selected = 0;
    };
    HANDLE callbackEntered = nullptr;
    HANDLE releaseCallback = nullptr;
    struct Invocation { void* code; uintptr_t actor; Observation observation; };
    bool __fastcall BlockingInclude(uintptr_t, uintptr_t)
    {
        SetEvent(callbackEntered);
        WaitForSingleObject(releaseCallback, 5'000);
        return true;
    }
    DWORD WINAPI InvokeOnThread(void* argument)
    {
        auto& invocation = *static_cast<Invocation*>(argument);
        InvokeOwnBridge(invocation.code, invocation.actor, invocation.actor, &invocation.observation);
        return 0;
    }
    void CheckRegisters(const Observation& value, void* stub, uintptr_t actor, uintptr_t localPlayer, bool nativeCl = false, bool nativeEcx = false)
    {
        Check(nativeEcx || (value.registers[1] & (nativeCl ? ~uintptr_t{0xFF} : ~uintptr_t{0})) ==
            (reinterpret_cast<uintptr_t>(stub) & (nativeCl ? ~uintptr_t{0xFF} : ~uintptr_t{0})), "RCX restored except native CL result");
        Check(value.registers[2] == actor && value.registers[3] == localPlayer, "RDX and R8 restored");
        Check(value.registers[4] == reinterpret_cast<uintptr_t>(&value), "R9 restored");
        Check(value.registers[5] == 0x2233445566778899 && value.registers[6] == 0x33445566778899AA, "R10 and R11 restored");
        const uint32_t vector[] = {0x11223344, 0x55667788, 0x12345678, 0x76543210};
        for (int reg = 0; reg < 6; ++reg)
            Check(std::memcmp(value.xmm + reg * 4, vector, 16) == 0, "XMM0..5 restored after clobbering callback");
    }
}

int main()
{
    try
    {
        using namespace nametag;
        Check(!Initialize(0, false) && readiness == 3 && bridge == nullptr && !EnableByDefault(),
            "unsupported build refuses hooks");
        Allocation options;
        imageBase = 0x10000000;
        options.Pointer(0, imageBase + fov::kOptionsVtableRva);
        options.Pointer(kPerspectiveOffset, options.Address(512));
        options.Pointer(512, imageBase + kEnumVtableRva);
        options.Pointer(520, options.Address(1024));
        const int enumValues[] = {2, 0, 0, 0};
        std::memcpy(options.memory + 528, enumValues, sizeof(enumValues));
        std::memcpy(options.memory + 2000, "game_thirdperson", 17);
        options.Pointer(1024 + fov::kKeyOffset, options.Address(2000));
        options.Pointer(1024 + fov::kKeyOffset + 16, 16);
        options.Pointer(1024 + fov::kKeyOffset + 24, 31);
        Check(Perspective(options.Address()) == 0, "first-person option identity");
        bridge = options.memory;
        data = reinterpret_cast<BridgeData*>(options.memory + kDataOffset);
        for (auto& patch : patches) patch.installed = true;
        readiness = 1;
        enableByDefaultPending = 1;
        BindOptions(options.Address());
        Check(readiness == 2 && enabled == 1 && enableByDefaultPending == 0,
            "default activation waits for verified camera options");
        enabled = 0;
        BindOptions(options.Address());
        Check(enabled == 0, "manual disable is preserved after startup");
        bridge = nullptr;
        data = nullptr;
        patches = {};
        readiness = 1;
        optionsAddress = static_cast<LONG64>(options.Address());
        enabled = 1;
        Check(!ShouldInclude(0x1000, 0x1000), "own name suppressed in first person");
        for (int perspective : {1, 2})
        {
            std::memcpy(options.memory + 536, &perspective, 4);
            Check(HudSuppression(0x1000, 1) == 0 && SkipOtherHudLabels(), "allow local name but keep other HUD labels hidden");
            Check(NamesHudMask(1) == 1 && NamesHudMask(0) == 1 && NamesHudMask(2) == 3,
                "names metadata bypasses only canonical hidden-HUD value in both cameras");
            Check(ShouldInclude(0x1000, 0x1000), "own name included in BOTH third-person perspectives");
            Check(!ShouldInclude(0x2000, 0x1000), "HUD-hidden override does not reveal other actors");
            Check(UseDepth(0x1000) && !UseDepth(0x2000), "depth test applies only to current native local player");
        }
        showWhenHudHidden = 0;
        Check(NamesHudMask(1) == 0, "disabled setting retains native names metadata mask");
        Check(HudSuppression(0x1000, 1) == 1, "disabled setting retains native hidden-HUD gate");
        showWhenHudHidden = 1;
        Check(HudSuppression(0, 1) == 1 && HudSuppression(0, 0) == 1,
            "missing local player never bypasses suppression");
        Check(HudSuppression(0x1000, 2) == 2, "unexpected native hidden value cannot bypass suppression");
        Check(HudSuppression(0x1000, 0) == 0 && !SkipOtherHudLabels() && ShouldInclude(0x2000, 0x1000),
            "visible HUD restores other actors and labels in the next frame");
        enabled = 0;
        Check(NamesHudMask(1) == 0, "F7 disable retains native names metadata mask");
        Check(HudSuppression(0x1000, 1) == 1, "F7 disable retains native suppression");
        HudSuppression(0x1000, 0);
        Check(!ShouldInclude(0x1000, 0x1000) && ShouldInclude(0x2000, 0x1000), "toggle leaves other actors alone");
        Check(UseDepth(0x1000), "occlusion survives toggle between inclusion and preparation");
        enabled = 1;
        options.memory[2000] = 'x';
        Check(!ShouldInclude(0x1000, 0x1000), "foreign option metadata refused");
        options.memory[2000] = 'g';
        options.Pointer(1024 + fov::kKeyOffset + 16, 15);
        Check(Perspective(options.Address()) == -1, "incorrect metadata length refused");
        options.Pointer(1024 + fov::kKeyOffset + 16, 16);
        int invalid = 3; std::memcpy(options.memory + 536, &invalid, 4);
        Check(!ShouldInclude(0x1000, 0x1000), "unknown perspective refused");
        Check(HudSuppression(0x1000, 1) == 1, "unknown perspective cannot bypass native HUD gate");
        Check(NamesHudMask(1) == 0, "invalid camera cannot bypass names metadata mask");
        int firstPerson = 0; std::memcpy(options.memory + 536, &firstPerson, 4);
        Check(HudSuppression(0x1000, 1) == 1, "first person retains native HUD-hidden suppression");
        Check(NamesHudMask(1) == 0, "first person retains names metadata mask");
        DWORD old = 0;
        VirtualProtect(options.memory + 4096, 4096, PAGE_NOACCESS, &old);
        Check(Perspective(options.Address(4096)) == -1, "unreadable options refused");
        optionsAddress = 0;
        Check(!ShouldInclude(0x1000, 0x1000) && !Toggle(), "missing target/refused readiness cannot enable");

        Allocation executable;
        auto* state = reinterpret_cast<BridgeData*>(executable.memory + kDataOffset);
        state->ownContinue = reinterpret_cast<uintptr_t>(&OwnContinueTest);
        state->ownSkip = reinterpret_cast<uintptr_t>(&OwnSkipTest);
        state->depthContinue = reinterpret_cast<uintptr_t>(&DepthContinueTest);
        state->hudContinue = reinterpret_cast<uintptr_t>(&HudContinueTest);
        state->loopContinue = reinterpret_cast<uintptr_t>(&LoopContinueTest);
        state->otherLabels = reinterpret_cast<uintptr_t>(&OtherLabelsTest);
        state->hudCleanup = reinterpret_cast<uintptr_t>(&HudCleanupTest);
        state->namesMaskContinue = reinterpret_cast<uintptr_t>(&NamesMaskContinueTest);
        state->ownCallback = reinterpret_cast<void*>(&ClobberTrue);
        state->depthCallback = reinterpret_cast<void*>(&ClobberTrue);
        BuildBridges(executable.memory, state);
        Check(VirtualProtect(executable.memory, 4096, PAGE_EXECUTE_READ, &old) != FALSE, "RX bridge code");
        FlushInstructionCache(GetCurrentProcess(), executable.memory, 4096);
        uintptr_t actorValue = 0xABCDEF0123456789;
        const uintptr_t actor = reinterpret_cast<uintptr_t>(&actorValue);
        Observation own{};
        InvokeOwnBridge(executable.memory, actor, actor, &own);
        Check(own.selected == 1 && own.registers[0] == actorValue, "own callback allows displaced native load");
        Check((own.flags & 0x40) != 0 && state->active == 0, "original equal flags restored and callback drained");
        CheckRegisters(own, executable.memory, actor, actor);
        Observation other{};
        InvokeOwnBridge(executable.memory, actor, actor + 8, &other);
        Check(other.selected == 1 && !(other.flags & 0x40), "original unequal flags restored");
        CheckRegisters(other, executable.memory, actor, actor + 8);
        state->ownCallback = reinterpret_cast<void*>(&ClobberFalse);
        Observation denied{};
        InvokeOwnBridge(executable.memory, actor, actor, &denied);
        Check(denied.selected == 0 && denied.registers[0] == 0x1122334455667788, "skip path preserves RAX");
        state->ownCallback = nullptr;
        InvokeOwnBridge(executable.memory, actor, actor, &denied);
        InvokeOwnBridge(executable.memory, actor, actor + 8, &other);
        Check(denied.selected == 0 && other.selected == 1, "cleared callback reproduces original self exclusion");

        for (void* callback : std::array<void*, 3>{reinterpret_cast<void*>(&ClobberTrue), reinterpret_cast<void*>(&ClobberFalse), nullptr})
        {
            state->depthCallback = callback;
            Observation depth{};
            InvokeDepthBridge(executable.memory + 512, actor, actor + 8, &depth);
            Check(depth.selected == (callback == reinterpret_cast<void*>(&ClobberTrue) ? 1 : 0), "correct original stack argument and alignment");
            Check(depth.registers[0] == 0x1122334455667788 && (depth.flags & 0x40), "depth bridge preserves RAX and flags");
            CheckRegisters(depth, executable.memory + 512, actor, actor + 8);
            Check(state->active == 0, "depth callback drained");
        }

        for (void* callback : std::array<void*, 3>{reinterpret_cast<void*>(&ClobberTrue), reinterpret_cast<void*>(&ClobberFalse), nullptr})
        {
            state->hudCallback = callback;
            state->loopExitCallback = callback;
            for (uintptr_t hidden : {uintptr_t{0}, uintptr_t{1}})
            {
                for (uintptr_t player : {uintptr_t{0}, actor})
                {
                    Observation hud{};
                    InvokeHudBridge(executable.memory + 1024, hidden, player, &hud);
                    const unsigned char expected = callback ? (callback == reinterpret_cast<void*>(&ClobberTrue) ? 1 : 0) :
                        static_cast<unsigned char>((player ? 0 : 1) | hidden);
                    Check(hud.selected == expected && ((hud.flags & 0x40) != 0) == (expected == 0),
                        "HUD bridge supplies native CL and branch flags, including null callback");
                    Check(hud.registers[0] == (uintptr_t{0x1122334455667700} | hidden), "HUD bridge preserves original AL and RAX");
                    CheckRegisters(hud, executable.memory + 1024, hidden, player, true);
                    Check(state->active == 0, "HUD callback drains");
                }
                Observation loop{};
                InvokeLoopExitBridge(executable.memory + 1536, hidden, actor, &loop);
                Check(loop.selected == (hidden ? 0 : callback == reinterpret_cast<void*>(&ClobberTrue) ? 2 : 1),
                    "loop bridge preserves fallthrough/native labels and hides other labels only on loop exit");
                Check(((loop.flags & 0x40) != 0) == (hidden == 0) && loop.registers[0] == 0x1122334455667788,
                    "loop bridge preserves incoming flags and RAX");
                CheckRegisters(loop, executable.memory + 1536, hidden, actor);
                Check(state->active == 0, "loop callback drains");
            }
        }

        Allocation code;
        for (void* callback : std::array<void*, 3>{reinterpret_cast<void*>(&ClobberTrue), reinterpret_cast<void*>(&ClobberFalse), nullptr})
        {
            state->namesMaskCallback = callback;
            for (uintptr_t hidden : {uintptr_t{0}, uintptr_t{1}, uintptr_t{2}})
                for (uint32_t allowed : {0u, 1u, 0x12340001u})
                {
                    Observation mask{};
                    InvokeNamesMaskBridge(executable.memory + 2048, hidden, reinterpret_cast<uintptr_t>(&allowed), &mask);
                    const unsigned char expectedMask = callback ? (callback == reinterpret_cast<void*>(&ClobberTrue) ? 1 : 0) :
                        static_cast<unsigned char>(hidden ^ 1);
                    const unsigned char result = static_cast<unsigned char>(allowed) & expectedMask;
                    Check(mask.selected == result && mask.registers[1] == ((allowed & ~0xFFu) | result),
                        "metadata bridge preserves native names preference and ECX load, including native denial");
                    Check(mask.registers[0] == (uintptr_t{0x1122334455667700} | expectedMask), "metadata bridge changes only native AL result");
                    Check(((mask.flags & 0x40) != 0) == (result == 0), "native metadata AND flags preserved");
                    // The displaced native ECX load is checked above.
                    CheckRegisters(mask, executable.memory + 2048, hidden, reinterpret_cast<uintptr_t>(&allowed), false, true);
                    Check(state->active == 0, "metadata callback drains");
                }
        }
        std::memcpy(code.memory, kOwnOriginal, sizeof(kOwnOriginal));
        std::memcpy(code.memory + 32, kDepthOriginal, sizeof(kDepthOriginal));
        std::memcpy(code.memory + 64, hudprofile::kGateOriginal, sizeof(hudprofile::kGateOriginal));
        std::memcpy(code.memory + 96, hudprofile::kLoopExitOriginal, sizeof(hudprofile::kLoopExitOriginal));
        std::memcpy(code.memory + 112, hudprofile::kNamesMaskOriginal, sizeof(hudprofile::kNamesMaskOriginal));
        Check(Jump(patches[0], code.Address(), kOwnOriginal, sizeof(kOwnOriginal), code.Address(128)), "relative own patch");
        Check(Jump(patches[1], code.Address(32), kDepthOriginal, sizeof(kDepthOriginal), code.Address(256)), "relative depth patch");
        Check(Jump(patches[2], code.Address(64), hudprofile::kGateOriginal, sizeof(hudprofile::kGateOriginal), code.Address(384)), "relative HUD gate patch");
        Check(Jump(patches[3], code.Address(96), hudprofile::kLoopExitOriginal, sizeof(hudprofile::kLoopExitOriginal), code.Address(512)), "relative loop exit patch");
        Check(Jump(patches[4], code.Address(112), hudprofile::kNamesMaskOriginal, sizeof(hudprofile::kNamesMaskOriginal), code.Address(640)), "relative metadata mask patch");
        Check(!Jump(patches[0], code.Address(), kOwnOriginal, sizeof(kOwnOriginal), code.Address() + 0x80000006), "out-of-range jump refused");
        VirtualProtect(code.memory, 4096, PAGE_EXECUTE_READ, &old);
        Check(ChangePatches(true) && patches[0].installed && patches[1].installed, "actual coordinated native patch installation");
        Check(ChangePatches(false) && Matches(code.Address(), kOwnOriginal, sizeof(kOwnOriginal)) &&
            Matches(code.Address(32), kDepthOriginal, sizeof(kDepthOriginal)) &&
            Matches(code.Address(64), hudprofile::kGateOriginal, sizeof(hudprofile::kGateOriginal)) &&
            Matches(code.Address(96), hudprofile::kLoopExitOriginal, sizeof(hudprofile::kLoopExitOriginal)) &&
            Matches(code.Address(112), hudprofile::kNamesMaskOriginal, sizeof(hudprofile::kNamesMaskOriginal)), "actual restoration of all five sites");
        Check(ChangePatches(true), "reinstall for foreign-byte test");
        VirtualProtect(code.memory, 4096, PAGE_READWRITE, &old);
        code.memory[32] = 0xCC;
        VirtualProtect(code.memory, 4096, PAGE_EXECUTE_READ, &old);
        Check(!ChangePatches(false) && code.memory[32] == 0xCC && patches[0].installed, "foreign edit preserved; partial restoration refused");
        VirtualProtect(code.memory, 4096, PAGE_READWRITE, &old);
        std::memcpy(code.memory + 32, patches[1].replacement.data(), patches[1].size);
        VirtualProtect(code.memory, 4096, PAGE_EXECUTE_READ, &old);
        Check(ChangePatches(false), "restore after foreign edit removed");
        MEMORY_BASIC_INFORMATION protection{};
        VirtualQuery(code.memory, &protection, sizeof(protection));
        Check(protection.Protect == PAGE_EXECUTE_READ, "original RX protection restored");
        state->ownCallback = reinterpret_cast<void*>(&BlockingInclude);
        callbackEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        releaseCallback = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(callbackEntered && releaseCallback, "callback synchronization events");
        Invocation invocation{executable.memory, actor, {}};
        HANDLE callbackThread = CreateThread(nullptr, 0, InvokeOnThread, &invocation, 0, nullptr);
        Check(callbackThread && WaitForSingleObject(callbackEntered, 2'000) == WAIT_OBJECT_0, "callback is executing on another thread");
        bridge = executable.memory;
        data = state;
        executable.memory = nullptr; // Transfer ownership to the real shutdown code.
        Check(!Shutdown() && bridge && data->active == 1 && !data->ownCallback && !data->depthCallback &&
            !data->hudCallback && !data->loopExitCallback && !data->namesMaskCallback,
            "shutdown clears entry points and retains executing callback code");
        SetEvent(releaseCallback);
        Check(WaitForSingleObject(callbackThread, 2'000) == WAIT_OBJECT_0 && data->active == 0, "callback returns and drains before unload");
        CloseHandle(callbackThread); CloseHandle(callbackEntered); CloseHandle(releaseCallback);
        Check(Shutdown() && !bridge && !data, "real shutdown safely releases bridge after drain");
        std::puts("Nametag tests passed: both perspectives, HUD suppression/local-only override, native names preference, first-person/toggle/refusal, depth selection, five executable bridges/registers/flags/ABI, coordinated restoration and foreign changes.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Nametag test failed: %s\n", error.what());
        return 1;
    }
}
