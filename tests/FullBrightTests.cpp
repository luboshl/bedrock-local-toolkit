// Synthetic executable pages only; no Minecraft process or module is opened.
#include "../GameMod/FullBright.h"
#include <cstdio>

extern "C" void InvokeFullBrightBridge(void*, void*, void*);
extern "C" void FullBrightContinueTest();

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    struct Allocation
    {
        unsigned char* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, nametag::kAllocationSize,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Allocation() { Check(memory != nullptr, "test allocation"); }
        ~Allocation() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
        uintptr_t Address(size_t offset = 0) const { return reinterpret_cast<uintptr_t>(memory + offset); }
    };
    struct Observation
    {
        uintptr_t registers[6]{};
        uintptr_t flags = 0;
        uint32_t xmm6[4]{};
    };
}

int main()
{
    try
    {
        using namespace fullbright;
        Check(!ValidateProfile(0, true) && !ValidateProfile(1, false), "unknown build/target refused");
        Check(!Toggle() && !EnableByDefault(), "enable without installed profile refused");
        Allocation executable;
        auto* state = reinterpret_cast<BridgeData*>(executable.memory + nametag::kDataOffset);
        state->continuation = reinterpret_cast<uintptr_t>(&FullBrightContinueTest);
        BuildBridge(executable.memory, state);
        DWORD previous = 0;
        Check(VirtualProtect(executable.memory, nametag::kDataOffset, PAGE_EXECUTE_READ, &previous) != FALSE,
            "bridge code RX, separate data RW");
        Check(FlushInstructionCache(GetCurrentProcess(), executable.memory, nametag::kDataOffset) != FALSE, "flush bridge");
        bridge = executable.memory;
        data = state;
        readiness = 2;
        Check(EnableByDefault() && enabled == 1 && state->enabled == 1, "ready feature enabled by default");
        const uint32_t vector[] = {0x11223344, 0x55667788, 0x12345678, 0x76543210};
        for (int mode : {0, 1, 0})
        {
            std::array<unsigned char, 0x54> parameters{};
            parameters.fill(0xA5); // Includes a guard after the native allocation.
            auto expected = parameters;
            if (mode)
            {
                expected[0x24] = 1;
                const float strength = 1;
                std::memcpy(expected.data() + 0x28, &strength, 4);
                expected[0x2C] = 0;
                std::memset(expected.data() + 0x30, 0, 4);
            }
            if (mode != InterlockedCompareExchange(&enabled, 0, 0)) Check(Toggle(), "toggle ready bridge");
            Observation observed{};
            InvokeFullBrightBridge(executable.memory, parameters.data(), &observed);
            Check(parameters == expected, "only temporary brightness fields changed; disabled input preserved");
            Check(observed.registers[0] == 0x1122334455667788 &&
                std::memcmp(observed.xmm6, vector, sizeof(vector)) == 0, "displaced RAX/XMM6 epilogue executed");
            Check(observed.registers[1] == executable.Address() &&
                observed.registers[2] == reinterpret_cast<uintptr_t>(parameters.data()) &&
                observed.registers[3] == reinterpret_cast<uintptr_t>(&observed) &&
                observed.registers[4] == 0x33445566778899AA && observed.registers[5] == observed.registers[2] &&
                (observed.flags & 0x8D5) == 0x44, "live registers and arithmetic flags preserved");
        }
        Check(OverrideCount() == 1, "counts only enabled executions");
        Allocation code;
        std::memcpy(code.memory, kOriginal, sizeof(kOriginal));
        Check(nametag::Jump(patch, code.Address(), kOriginal, sizeof(kOriginal), code.Address(128)), "prepare patch");
        Check(VirtualProtect(code.memory, 4096, PAGE_EXECUTE_READ, &previous) != FALSE, "native code RX");
        Check(ChangePatch(true) && patch.installed, "install patch");
        Check(ChangePatch(false) && nametag::Matches(code.Address(), kOriginal, sizeof(kOriginal)), "restore original epilogue");
        Check(ChangePatch(true), "reinstall patch");
        VirtualProtect(code.memory, 4096, PAGE_READWRITE, &previous);
        code.memory[0] = 0xCC;
        VirtualProtect(code.memory, 4096, PAGE_EXECUTE_READ, &previous);
        Check(!Shutdown() && bridge && code.memory[0] == 0xCC && !Toggle(), "foreign patch retained; unload and toggles refused");
        VirtualProtect(code.memory, 4096, PAGE_READWRITE, &previous);
        std::memcpy(code.memory, patch.replacement.data(), patch.size);
        VirtualProtect(code.memory, 4096, PAGE_EXECUTE_READ, &previous);
        executable.memory = nullptr; // Shutdown owns and releases the bridge.
        Check(Shutdown() && !bridge && !data && OverrideCount() == 1, "shutdown restores hook and releases bridge");
        MEMORY_BASIC_INFORMATION protection{};
        VirtualQuery(code.memory, &protection, sizeof(protection));
        Check(protection.Protect == PAGE_EXECUTE_READ && nametag::Matches(code.Address(), kOriginal, sizeof(kOriginal)),
            "original bytes and RX protection restored");
        std::puts("Full Bright tests passed: emitted x64 bridge, toggles, bounded render writes, registers/flags, build refusal and safe restoration.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Full Bright test failed: %s\n", error.what());
        return 1;
    }
}
