// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Venture's extensions of the arm64-host backend: LSE atomics (fastmem and callbacks),
// fast supervisor calls, the host FP environment around callbacks and precise memory
// aborts on the callback-only memory paths.

#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

u64 HostFpcr() {
    u64 value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}
u64 HostFpsr() {
    u64 value;
    asm volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}
void SetHostFpcr(u64 value) { asm volatile("msr fpcr, %0" : : "r"(value)); }
void SetHostFpsr(u64 value) { asm volatile("msr fpsr, %0" : : "r"(value)); }

// Restores the test thread's FP environment.
struct HostFpGuard {
    u64 fpcr = HostFpcr(), fpsr = HostFpsr();
    ~HostFpGuard() {
        SetHostFpcr(fpcr);
        SetHostFpsr(fpsr);
    }
};

// fdiv under `fpcr`, as hardware computes it.
u64 Divide(double a, double b, u64 fpcr) {
    HostFpGuard guard;
    SetHostFpcr(fpcr);
    double result;
    asm volatile("fdiv %d0, %d1, %d2" : "=w"(result) : "w"(a), "w"(b));
    u64 bits;
    std::memcpy(&bits, &result, sizeof(bits));
    return bits;
}

constexpr u32 fpcr_round_toward_zero = 0b11 << 22;
constexpr u32 fpcr_flush_to_zero = 1 << 24;
constexpr u32 fpsr_inexact = 1 << 4;

// Encodings.
constexpr u32 Svc(u32 imm) { return 0xd4000001 | imm << 5; }
constexpr u32 Lse(u32 size, bool acquire, bool release, u32 rs, u32 op, u32 rn, u32 rt) {
    return 0x38200000 | size << 30 | u32(acquire) << 23 | u32(release) << 22 | rs << 16 | (op >> 3) << 15 | (op & 7) << 12 | rn << 5 | rt;
}
constexpr u32 Cas(u32 size, bool acquire, bool release, u32 rs, u32 rn, u32 rt) {
    return 0x08a07c00 | size << 30 | u32(acquire) << 22 | rs << 16 | u32(release) << 15 | rn << 5 | rt;
}
constexpr u32 Casp(bool sz, bool acquire, bool release, u32 rs, u32 rn, u32 rt) {
    return 0x08207c00 | u32(sz) << 30 | u32(acquire) << 22 | rs << 16 | u32(release) << 15 | rn << 5 | rt;
}
static_assert(Lse(0, true, true, 1, 0, 0, 2) == 0x38e10002);  // ldaddalb w1, w2, [x0]
static_assert(Lse(3, true, false, 1, 8, 0, 2) == 0xf8a18002);  // swpa x1, x2, [x0]
static_assert(Cas(0, true, true, 1, 0, 2) == 0x08e1fc02);      // casalb w1, w2, [x0]
static_assert(Casp(true, true, true, 2, 0, 4) == 0x4862fc04);  // caspal x2, x3, x4, x5, [x0]

constexpr u32 stop_svc = 0xff;
constexpr u32 STOP = Svc(stop_svc);
constexpr u32 ADD_X1_X0_1 = 0x91000401;
constexpr u32 ADD_X1_X4_1 = 0x91000481;
constexpr u32 ADD_X1_X1_1 = 0x91000421;
constexpr u32 MOVZ_X5_7 = 0xd28000e5;
constexpr u32 FDIV_D2_D0_D1 = 0x1e611802;
constexpr u32 FDIV_D3_D0_D1 = 0x1e611803;
constexpr u32 LDR_X2_X3 = 0xf9400062;
constexpr u32 STR_X1_X3 = 0xf9000061;
constexpr u32 LDXR_X2_X3 = 0xc85f7c62;
constexpr u32 LDADD_X1_X2_X3 = 0xf8210062;
constexpr std::array<u32, 4> LDAXR_W2_X0{0x085ffc02, 0x485ffc02, 0x885ffc02, 0xc85ffc02};  // by size: b, h, w, x
constexpr std::array<u32, 4> STLXR_W4_W2_X0{0x0804fc02, 0x4804fc02, 0x8804fc02, 0xc804fc02};
constexpr u32 ADD_X2_X2_1 = 0x91000442;
constexpr u32 LDXR_X2_X0 = 0xc85f7c02;
constexpr u32 STXR_W4_X2_X0 = 0xc8047c02;
constexpr u32 LDXP_X2_X3_X0 = 0xc87f0c02;
constexpr u32 STXP_W4_X5_X6_X0 = 0xc8241805;
constexpr u32 CLREX = 0xd5033f5f;

// Guest memory: code page, two data pages and an inaccessible page whose contents the
// callbacks keep (fastmem faults there).
struct Arena {
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const u64 data = page;
    const u64 device = 3 * page;
    u8* base = nullptr;

    Arena() {
        base = static_cast<u8*>(mmap(nullptr, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        REQUIRE(base != MAP_FAILED);
        REQUIRE(mprotect(base + device, page, PROT_NONE) == 0);
    }
    ~Arena() { munmap(base, 4 * page); }
};

u64 Apply(A64::AtomicOp op, std::size_t bits, u64 old, u64 value, u64 desired) {
    const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
    old &= mask;
    value &= mask;
    const auto sext = [bits](u64 v) { return bits == 64 ? s64(v) : s64(v << (64 - bits)) >> (64 - bits); };
    using Op = A64::AtomicOp;
    switch (op) {
    case Op::Add: return (old + value) & mask;
    case Op::Clear: return old & ~value;
    case Op::Eor: return old ^ value;
    case Op::Set: return old | value;
    case Op::SMax: return sext(old) > sext(value) ? old : value;
    case Op::SMin: return sext(old) < sext(value) ? old : value;
    case Op::UMax: return old > value ? old : value;
    case Op::UMin: return old < value ? old : value;
    case Op::Swap: return value;
    case Op::CompareAndSwap: return old == value ? desired & mask : old;
    }
    return old;
}

class A64Env final : public A64::UserCallbacks {
public:
    explicit A64Env(Arena& arena) : arena(arena), device(arena.page) {}

    Arena& arena;
    std::vector<u8> device;
    A64::Jit* jit = nullptr;

    std::size_t atomic_calls = 0, cas128_calls = 0, svc_calls = 0, fast_calls = 0, known_calls = 0, memory_calls = 0;
    std::optional<u64> abort_address;  // a callback access here raises a memory abort
    std::function<void(u32)> on_svc, on_fast;
    std::function<void(u32, const void*, void*)> on_known;
    std::function<void()> on_memory;
    std::vector<u64> exceptions;

    void Load(const std::vector<u32>& code, u64 at = 0) {
        std::memcpy(arena.base + at, code.data(), code.size() * sizeof(u32));
        if (jit) {
            jit->ClearCache();
        }
    }

    u8* Host(u64 vaddr) {
        if (vaddr >= arena.device && vaddr < arena.device + arena.page) {
            return device.data() + (vaddr - arena.device);
        }
        return arena.base + vaddr;
    }
    bool Abort(u64 vaddr) {
        ++memory_calls;
        if (on_memory) {
            on_memory();
        }
        if (abort_address && vaddr == *abort_address) {
            jit->HaltExecution(HaltReason::MemoryAbort);
            return true;
        }
        return false;
    }
    template<typename T>
    bool WriteExclusive(u64 vaddr, T value, T expected) {
        if (Abort(vaddr)) {
            return false;
        }
        T current;
        std::memcpy(&current, Host(vaddr), sizeof(T));
        if (std::memcmp(&current, &expected, sizeof(T)) != 0) {
            return false;
        }
        std::memcpy(Host(vaddr), &value, sizeof(T));
        return true;
    }

    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        u32 value;
        std::memcpy(&value, arena.base + vaddr, sizeof(value));
        return value;
    }
    u64 MemoryRead(u64 vaddr, std::size_t size) override {
        u64 value = 0;
        if (!Abort(vaddr)) {
            std::memcpy(&value, Host(vaddr), size);
        }
        return value;
    }
    Vector MemoryRead128(u64 vaddr) override {
        Vector value{};
        if (!Abort(vaddr)) {
            std::memcpy(&value, Host(vaddr), sizeof(value));
        }
        return value;
    }
    void MemoryWrite(u64 vaddr, u64 value, std::size_t size) override {
        if (!Abort(vaddr)) {
            std::memcpy(Host(vaddr), &value, size);
        }
    }
    void MemoryWrite128(u64 vaddr, Vector value) override {
        if (!Abort(vaddr)) {
            std::memcpy(Host(vaddr), &value, sizeof(value));
        }
    }
    bool MemoryWriteExclusive8(u64 vaddr, u8 value, u8 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive16(u64 vaddr, u16 value, u16 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive32(u64 vaddr, u32 value, u32 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive64(u64 vaddr, u64 value, u64 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive128(u64 vaddr, Vector value, Vector expected) override { return WriteExclusive(vaddr, value, expected); }

    u64 MemoryAtomic(u64 vaddr, std::size_t bytes, A64::AtomicOp op, u64 value, u64 desired) override {
        ++atomic_calls;
        if (Abort(vaddr)) {
            return 0;
        }
        u64 old = 0;
        std::memcpy(&old, Host(vaddr), bytes);
        const u64 result = Apply(op, bytes * 8, old, value, desired);
        std::memcpy(Host(vaddr), &result, bytes);
        return old;
    }
    Vector MemoryAtomicCompareAndSwap128(u64 vaddr, Vector expected, Vector desired) override {
        ++cas128_calls;
        Vector old{};
        if (Abort(vaddr)) {
            return old;
        }
        std::memcpy(&old, Host(vaddr), sizeof(old));
        if (old == expected) {
            std::memcpy(Host(vaddr), &desired, sizeof(desired));
        }
        return old;
    }

    void CallSVC(u32 swi) override {
        ++svc_calls;
        if (on_svc) {
            on_svc(swi);
        }
        if (swi == stop_svc) {
            jit->HaltExecution(HaltReason::UserDefined2);
        }
    }
    void CallSVCFast(u32 swi) override {
        ++fast_calls;
        if (on_fast) {
            on_fast(swi);
        }
    }
    void CallSVCFastKnown(u32 swi, const void* metadata, void* state) override {
        ++known_calls;
        if (on_known) {
            on_known(swi, metadata, state);
        }
    }
    void ExceptionRaised(u64 pc, A64::Exception) override {
        exceptions.push_back(pc);
        jit->HaltExecution(HaltReason::UserDefined3);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1'000'000; }
    u64 GetCNTPCT() override { return 0; }
};

A64::UserConfig MakeConfig(A64Env& env, bool fastmem) {
    A64::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    conf.code_cache_size = 16 * 1024 * 1024;
    conf.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    conf.unsafe_optimizations = true;
    if (fastmem) {
        conf.fastmem_pointer = reinterpret_cast<std::uintptr_t>(env.arena.base);
        conf.fastmem_address_space_bits = 64;
        conf.silently_mirror_fastmem = false;
        conf.recompile_on_fastmem_failure = true;
    }
    return conf;
}

HaltReason RunFrom(A64::Jit& jit, u64 pc) {
    jit.SetPC(pc);
    return jit.Run();
}

constexpr std::array<u64, 2> atomic_values{0xF0E1D2C3B4A59687, 0x0123456789ABCDEF};

void CheckAtomics(bool fastmem) {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, fastmem)};
    env.jit = &jit;

    for (u32 size = 0; size < 4; ++size) {
        const std::size_t bits = 8u << size;
        const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
        for (u32 op = 0; op <= 8; ++op) {
            for (u32 order = 0; order < 4; ++order) {
                for (std::size_t swap = 0; swap < 2; ++swap) {
                    const bool acquire = order & 1, release = order & 2;
                    const u64 old = atomic_values[swap], value = atomic_values[1 - swap];
                    const u64 address = arena.data + 8 * (op + 1);
                    std::memcpy(arena.base + address, &old, sizeof(old));
                    env.Load({Lse(size, acquire, release, 1, op, 0, 2), STOP});
                    jit.SetRegister(0, address);
                    jit.SetRegister(1, value);
                    jit.SetRegister(2, 0xdeadbeefdeadbeef);
                    const std::size_t calls = env.atomic_calls;

                    INFO("size " << bits << " op " << op << " order " << order << " swap " << swap);
                    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
                    REQUIRE(jit.GetRegister(2) == (old & mask));
                    u64 memory;
                    std::memcpy(&memory, arena.base + address, sizeof(memory));
                    const u64 expected = (old & ~mask) | Apply(static_cast<A64::AtomicOp>(op), bits, old, value, 0);
                    REQUIRE(memory == expected);
                    REQUIRE(env.atomic_calls == calls + (fastmem ? 0 : 1));
                }
            }
        }
    }
}

void CheckCompareAndSwap(bool fastmem) {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, fastmem)};
    env.jit = &jit;
    const u64 address = arena.data + 0x40;

    for (u32 size = 0; size < 4; ++size) {
        const std::size_t bits = 8u << size;
        const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
        for (u32 order = 0; order < 4; ++order) {
            for (const bool match : {true, false}) {
                const u64 old = 0x8877665544332211, desired = 0x0102030405060708;
                const u64 expected = match ? old : old ^ 1;
                std::memcpy(arena.base + address, &old, sizeof(old));
                env.Load({Cas(size, order & 1, order & 2, 1, 0, 2), STOP});
                jit.SetRegister(0, address);
                jit.SetRegister(1, expected | (bits == 64 ? 0 : 0xabcd'0000'0000'0000));
                jit.SetRegister(2, desired);
                const std::size_t calls = env.atomic_calls;

                INFO("size " << bits << " order " << order << " match " << match);
                REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
                REQUIRE(jit.GetRegister(1) == (old & mask));
                u64 memory;
                std::memcpy(&memory, arena.base + address, sizeof(memory));
                REQUIRE(memory == (match ? (old & ~mask) | (desired & mask) : old));
                REQUIRE(env.atomic_calls == calls + (fastmem ? 0 : 1));
            }
        }
    }

    // CASP: X2:X3 expected and old, X4:X5 desired.
    for (const bool sz : {false, true}) {
        for (const bool match : {true, false}) {
            const std::array<u64, 2> old{0x1111222233334444, 0x5555666677778888};
            const std::array<u64, 2> desired{0x9999aaaabbbbcccc, 0xddddeeeeffff0000};
            std::memcpy(arena.base + address, old.data(), sizeof(old));
            env.Load({Casp(sz, true, true, 2, 0, 4), STOP});
            jit.SetRegister(0, address);
            if (sz) {
                jit.SetRegister(2, old[0] ^ (match ? 0 : 1));
                jit.SetRegister(3, old[1]);
                jit.SetRegister(4, desired[0]);
                jit.SetRegister(5, desired[1]);
            } else {
                jit.SetRegister(2, u32(old[0]) ^ (match ? 0 : 1));
                jit.SetRegister(3, u32(old[0] >> 32));
                jit.SetRegister(4, u32(desired[0]));
                jit.SetRegister(5, u32(desired[0] >> 32));
            }
            const std::size_t calls = env.atomic_calls + env.cas128_calls;

            INFO("casp sz " << sz << " match " << match);
            REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
            std::array<u64, 2> memory;
            std::memcpy(memory.data(), arena.base + address, sizeof(memory));
            if (sz) {
                REQUIRE(jit.GetRegister(2) == old[0]);
                REQUIRE(jit.GetRegister(3) == old[1]);
                REQUIRE(memory == (match ? desired : old));
            } else {
                REQUIRE(jit.GetRegister(2) == u32(old[0]));
                REQUIRE(jit.GetRegister(3) == u32(old[0] >> 32));
                REQUIRE(memory[0] == (match ? desired[0] : old[0]));
                REQUIRE(memory[1] == old[1]);
            }
            REQUIRE(env.atomic_calls + env.cas128_calls == calls + (fastmem ? 0 : 1));
        }
    }
}

}  // namespace

TEST_CASE("arm64: LSE atomics through the MemoryAtomic callback", "[a64][arm64][atomics]") {
    CheckAtomics(false);
}

TEST_CASE("arm64: LSE atomics on fastmem", "[a64][arm64][atomics]") {
    CheckAtomics(true);
}

TEST_CASE("arm64: CAS and CASP through the callbacks", "[a64][arm64][atomics]") {
    CheckCompareAndSwap(false);
}

TEST_CASE("arm64: CAS and CASP on fastmem", "[a64][arm64][atomics]") {
    CheckCompareAndSwap(true);
}

TEST_CASE("arm64: fastmem atomics fall back on faults and misalignment", "[a64][arm64][atomics]") {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, true)};
    env.jit = &jit;

    // A fault at the LSE instruction: the callback completes it, the recompiled block calls it directly.
    const u64 device_address = arena.device + 0x10;
    u64 old = 40;
    std::memcpy(env.Host(device_address), &old, sizeof(old));
    env.Load({Lse(3, true, true, 1, 0, 0, 2), STOP});
    for (int run = 0; run < 2; ++run) {
        jit.SetRegister(0, device_address);
        jit.SetRegister(1, 2);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(2) == old);
        old += 2;
        u64 memory;
        std::memcpy(&memory, env.Host(device_address), sizeof(memory));
        REQUIRE(memory == old);
        REQUIRE(env.atomic_calls == std::size_t(run + 1));
    }

    // CASP faulting.
    const std::array<u64, 2> pair_old{1, 2}, pair_new{3, 4};
    std::memcpy(env.Host(device_address), pair_old.data(), sizeof(pair_old));
    env.Load({Casp(true, false, false, 2, 0, 4), STOP});
    jit.SetRegister(0, device_address);
    jit.SetRegister(2, 1);
    jit.SetRegister(3, 2);
    jit.SetRegister(4, 3);
    jit.SetRegister(5, 4);
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(env.cas128_calls == 1);
    std::array<u64, 2> pair_memory;
    std::memcpy(pair_memory.data(), env.Host(device_address), sizeof(pair_memory));
    REQUIRE(pair_memory == pair_new);

    // Misaligned: LSE requires natural alignment, the callback does not.
    const u64 misaligned = arena.data + 2;
    const u32 word = 0x10000000;
    std::memcpy(arena.base + misaligned, &word, sizeof(word));
    env.Load({Lse(2, false, false, 1, 0, 0, 2), STOP});
    jit.SetRegister(0, misaligned);
    jit.SetRegister(1, 5);
    const std::size_t calls = env.atomic_calls;
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(env.atomic_calls == calls + 1);
    REQUIRE(jit.GetRegister(2) == word);
    u32 memory;
    std::memcpy(&memory, arena.base + misaligned, sizeof(memory));
    REQUIRE(memory == word + 5);
}

TEST_CASE("arm64: fast supervisor calls stay in Run", "[a64][arm64][svc]") {
    Arena arena;
    A64Env env{arena};
    static constexpr int token = 0;
    auto conf = MakeConfig(env, false);
    conf.svc_fast = 0x10;
    conf.svc_lookup = [](u64 pc, u32 imm) -> const void* { return pc == 0x40 && imm == 0x10 ? &token : nullptr; };
    A64::Jit jit{conf};
    env.jit = &jit;

    SECTION("register round trip through the Jit interface") {
        env.on_fast = [&](u32 swi) {
            REQUIRE(swi == 0x10);
            jit.SetRegister(0, jit.GetRegister(0) * 2);
        };
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP});
        jit.SetRegister(0, 21);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(env.fast_calls == 1);
        REQUIRE(env.svc_calls == 1);  // the stop
        REQUIRE(jit.GetRegister(0) == 42);
        REQUIRE(jit.GetRegister(1) == 43);
        REQUIRE(jit.GetPC() == 12);
    }

    SECTION("known thunk gets its metadata and the JitState") {
        env.on_known = [&](u32 swi, const void* metadata, void* state) {
            REQUIRE(swi == 0x10);
            REQUIRE(metadata == &token);
            auto& registers = static_cast<Backend::Arm64::A64JitState*>(state)->reg;
            registers[0] += 5;
        };
        // The lookup names a thunk by its block's PC.
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP}, 0x40);
        jit.SetRegister(0, 1);
        REQUIRE(RunFrom(jit, 0x40) == HaltReason::UserDefined2);
        REQUIRE(env.fast_calls == 0);
        REQUIRE(env.known_calls == 1);
        REQUIRE(jit.GetRegister(0) == 6);
        REQUIRE(jit.GetRegister(1) == 7);
    }

    SECTION("a halt inside the callback stops after the call") {
        env.on_fast = [&](u32) { jit.HaltExecution(HaltReason::UserDefined1); };
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP});
        jit.SetRegister(0, 1);
        jit.SetRegister(1, 99);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined1);
        REQUIRE(env.fast_calls == 1);
        REQUIRE(jit.GetRegister(1) == 99);
        REQUIRE(jit.GetPC() == 4);
    }

    SECTION("a callback may redirect the PC") {
        env.on_fast = [&](u32) { jit.SetPC(8); };
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP});
        jit.SetRegister(1, 99);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 99);
    }
}

TEST_CASE("arm64: callbacks run in the host FP environment", "[a64][arm64][fp]") {
    HostFpGuard guard;
    Arena arena;
    A64Env env{arena};
    auto conf = MakeConfig(env, false);
    conf.svc_fp_transfer = 0x12;
    A64::Jit jit{conf};
    env.jit = &jit;

    const double a = 1.0, b = 10.0;
    const u64 guest_result = Divide(a, b, fpcr_round_toward_zero);
    const u64 host_result = Divide(a, b, fpcr_flush_to_zero);
    REQUIRE(guest_result != host_result);

    for (const u32 imm : {0x20u, 0x12u}) {
        INFO("svc " << imm);
        std::optional<u64> fpcr_inside, fpsr_inside, divide_inside;
        std::optional<u32> guest_fpsr_inside;
        const auto record = [&](u32 swi) {
            if (swi != imm) {
                return;
            }
            fpcr_inside = HostFpcr();
            fpsr_inside = HostFpsr();
            guest_fpsr_inside = jit.GetFpsr();
            volatile double x = a, y = b;
            const double r = x / y;
            std::memcpy(&divide_inside.emplace(), &r, sizeof(r));
        };
        env.on_svc = record;
        env.on_fast = record;
        env.Load({FDIV_D2_D0_D1, Svc(imm), FDIV_D3_D0_D1, STOP});
        jit.SetVector(0, {std::bit_cast<u64>(a), 0});
        jit.SetVector(1, {std::bit_cast<u64>(b), 0});
        jit.SetFpcr(fpcr_round_toward_zero);
        // The first Run services the cache clear, whose host code may raise host flags.
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        jit.SetFpsr(0);

        SetHostFpcr(fpcr_flush_to_zero);
        SetHostFpsr(0);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        const u64 host_fpcr_after = HostFpcr(), host_fpsr_after = HostFpsr();

        REQUIRE(fpcr_inside == fpcr_flush_to_zero);
        REQUIRE(fpsr_inside == 0);
        REQUIRE(guest_fpsr_inside.has_value());
        REQUIRE((*guest_fpsr_inside & fpsr_inexact) != 0);  // the first FDIV's flag, merged
        REQUIRE(divide_inside == host_result);
        REQUIRE(jit.GetVector(2)[0] == guest_result);
        REQUIRE(jit.GetVector(3)[0] == guest_result);  // guest rounding mode reinstalled
        REQUIRE((jit.GetFpsr() & fpsr_inexact) != 0);
        REQUIRE(host_fpcr_after == fpcr_flush_to_zero);
        REQUIRE(host_fpsr_after == 0);  // guest flags stay in the guest FPSR
    }

    SECTION("a callback's SetFpcr applies after it") {
        env.on_svc = [&](u32 swi) {
            if (swi == 0x20) {
                jit.SetFpcr(0);
            }
        };
        env.Load({FDIV_D2_D0_D1, Svc(0x20), FDIV_D3_D0_D1, STOP});
        jit.SetFpcr(fpcr_round_toward_zero);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetVector(2)[0] == guest_result);
        REQUIRE(jit.GetVector(3)[0] == Divide(a, b, 0));
    }

    SECTION("memory callbacks") {
        std::optional<u64> fpcr_inside;
        env.on_memory = [&] { fpcr_inside = HostFpcr(); };
        env.Load({LDR_X2_X3, FDIV_D3_D0_D1, STOP});
        jit.SetRegister(3, arena.data);
        jit.SetFpcr(fpcr_round_toward_zero);
        SetHostFpcr(fpcr_flush_to_zero);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(fpcr_inside == fpcr_flush_to_zero);
        REQUIRE(jit.GetVector(3)[0] == guest_result);
    }
}

TEST_CASE("arm64: svc_fast and svc_fp_guest keep the guest FP environment", "[a64][arm64][fp]") {
    HostFpGuard guard;
    Arena arena;
    A64Env env{arena};
    auto conf = MakeConfig(env, false);
    conf.svc_fast = 0x10;
    conf.svc_fp_guest = 0x13;
    A64::Jit jit{conf};
    env.jit = &jit;

    for (const u32 imm : {0x10u, 0x13u}) {
        INFO("svc " << imm);
        std::optional<u64> fpcr_inside;
        env.on_fast = [&](u32) {
            fpcr_inside = HostFpcr();
            // Host math in the callback rounds in the guest mode; its flags are the guest's.
            volatile double x = 2.0, y = 3.0;
            volatile double r = x / y;
            (void)r;
        };
        env.Load({Svc(imm), STOP});
        jit.SetFpcr(fpcr_round_toward_zero);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        jit.SetFpsr(0);
        SetHostFpcr(0);
        SetHostFpsr(0);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        const u64 host_fpcr_after = HostFpcr(), host_fpsr_after = HostFpsr();
        REQUIRE(fpcr_inside == fpcr_round_toward_zero);
        REQUIRE((jit.GetFpsr() & fpsr_inexact) != 0);
        REQUIRE(host_fpsr_after == 0);
        REQUIRE(host_fpcr_after == 0);
    }
}

TEST_CASE("arm64: memory aborts on callback paths are precise", "[a64][arm64][abort]") {
    Arena arena;
    A64Env env{arena};
    ExclusiveMonitor monitor{1};
    auto conf = MakeConfig(env, false);
    conf.global_monitor = &monitor;
    conf.check_halt_on_memory_access = true;
    conf.cold_memory_abort_state = A64::UserConfig::ColdAbortState::Live;

    for (const bool fastmem : {false, true}) {
        auto run_conf = conf;
        if (fastmem) {
            run_conf.fastmem_pointer = reinterpret_cast<std::uintptr_t>(arena.base);
            run_conf.fastmem_address_space_bits = 64;
            run_conf.silently_mirror_fastmem = false;
        }
        A64::Jit jit{run_conf};
        env.jit = &jit;
        // Fastmem faults in the inaccessible page and its fallback callback aborts.
        const u64 address = fastmem ? arena.device + 0x20 : arena.data + 0x20;
        env.abort_address = address;

        for (const u32 access : {LDR_X2_X3, STR_X1_X3, LDXR_X2_X3, LDADD_X1_X2_X3}) {
            INFO("fastmem " << fastmem << " access " << std::hex << access);
            // X1's first value is overwritten after the access: it reaches the JitState
            // only on the abort exit (A64ColdSetX).
            env.Load({ADD_X1_X4_1, access, ADD_X1_X1_1, MOVZ_X5_7, STOP});
            jit.SetRegister(1, 0);
            jit.SetRegister(3, address);
            jit.SetRegister(4, 100);
            jit.SetRegister(5, 0);
            const HaltReason hr = RunFrom(jit, 0);
            REQUIRE(Has(hr, HaltReason::MemoryAbort));
            REQUIRE(jit.GetPC() == 4);
            REQUIRE(jit.GetRegister(1) == 101);
            REQUIRE(jit.GetRegister(5) == 0);
        }
    }
}

TEST_CASE("arm64: fastmem exclusives keep the global monitor", "[a64][arm64][exclusive]") {
    for (const bool ignore_monitor : {false, true}) {
        INFO("ignore monitor " << ignore_monitor);
        Arena arena;
        A64Env env{arena};
        ExclusiveMonitor monitor{1};
        if (ignore_monitor) {
            monitor.EnableGenerations();
        }
        auto conf = MakeConfig(env, true);
        conf.unsafe_optimizations = ignore_monitor;
        conf.global_monitor = &monitor;
        conf.fastmem_exclusive_access = true;
        conf.elide_unchanged_exclusive_stores = true;
        A64::Jit jit{conf};
        env.jit = &jit;
        const u64 address = arena.data + 0x80;
        const auto memory = [&](u64 at) {
            u64 value;
            std::memcpy(&value, env.Host(at), sizeof(value));
            return value;
        };
        const auto set_memory = [&](u64 at, u64 value) { std::memcpy(env.Host(at), &value, sizeof(value)); };

        // An exclusive increment of each size, on fastmem and through a fault's callbacks.
        for (const u64 at : {address, arena.device + 0x80}) {
            for (u32 size = 0; size < 4; ++size) {
                INFO("at " << std::hex << at << " size " << size);
                const std::size_t bits = 8u << size;
                const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
                const u64 old = 0x8877665544332211 | mask;  // the low `bits` wrap
                set_memory(at, old);
                env.Load({LDAXR_W2_X0[size], ADD_X2_X2_1, STLXR_W4_W2_X0[size], STOP});
                jit.SetRegister(0, at);
                jit.SetRegister(4, 7);
                REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
                REQUIRE(jit.GetRegister(4) == 0);
                REQUIRE(jit.GetRegister(2) == (old & mask) + 1);
                REQUIRE(memory(at) == (old & ~mask));
            }
        }

        // CLREX between the pair fails the store.
        set_memory(address, 5);
        env.Load({LDXR_X2_X0, CLREX, STXR_W4_X2_X0, STOP});
        jit.SetRegister(0, address);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(4) == 1);
        REQUIRE(memory(address) == 5);

        // An unchanged value (elided store) succeeds.
        env.Load({LDXR_X2_X0, STXR_W4_X2_X0, STOP});
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(4) == 0);
        REQUIRE(memory(address) == 5);

        // 128-bit pair.
        set_memory(address, 0x1111);
        set_memory(address + 8, 0x2222);
        env.Load({LDXP_X2_X3_X0, STXP_W4_X5_X6_X0, STOP});
        jit.SetRegister(5, 0x3333);
        jit.SetRegister(6, 0x4444);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(2) == 0x1111);
        REQUIRE(jit.GetRegister(3) == 0x2222);
        REQUIRE(jit.GetRegister(4) == 0);
        REQUIRE(memory(address) == 0x3333);
        REQUIRE(memory(address + 8) == 0x4444);

        // Breaking the reservation between the pair fails the store: another CPU's
        // exclusive store clears it in the monitor; value-compared exclusives
        // (IgnoreGlobalMonitor) see the changed value.
        set_memory(address, 9);
        env.Load({LDXR_X2_X0, ADD_X2_X2_1, STXR_W4_X2_X0, STOP});
        jit.SetPC(0);
        REQUIRE(Has(jit.Step(), HaltReason::Step));
        if (ignore_monitor) {
            set_memory(address, 10);
        } else {
            monitor.ClearProcessor(0);
        }
        REQUIRE(jit.Run() == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(4) == 1);
    }
}

namespace {

class A32Env final : public A32::UserCallbacks {
public:
    explicit A32Env(Arena& arena) : arena(arena) {}

    Arena& arena;
    A32::Jit* jit = nullptr;
    std::size_t fast_calls = 0, known_calls = 0;
    std::optional<u64> fpcr_inside;

    u64 MemoryRead(u32 vaddr, std::size_t size) override {
        u64 value = 0;
        std::memcpy(&value, arena.base + vaddr, size);
        return value;
    }
    void MemoryWrite(u32 vaddr, u64 value, std::size_t size) override { std::memcpy(arena.base + vaddr, &value, size); }
    void CallSVC(u32 swi) override {
        if (swi == stop_svc) {
            jit->HaltExecution(HaltReason::UserDefined2);
        }
    }
    void CallSVCFast(u32) override {
        ++fast_calls;
        fpcr_inside = HostFpcr();
        jit->Regs()[0] *= 2;
    }
    void CallSVCFastKnown(u32, const void*, void* state) override {
        ++known_calls;
        static_cast<Backend::Arm64::A32JitState*>(state)->regs[0] += 5;
    }
    void ExceptionRaised(u32, A32::Exception) override { jit->HaltExecution(HaltReason::UserDefined3); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1'000'000; }
};

}  // namespace

TEST_CASE("arm64: A32 fast supervisor calls", "[a32][arm64][svc]") {
    HostFpGuard guard;
    Arena arena;
    A32Env env{arena};
    static constexpr int token = 0;
    A32::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    conf.code_cache_size = 16 * 1024 * 1024;
    conf.svc_fast = 0x10;
    conf.svc_fp_transfer = 0x12;
    conf.svc_lookup = [](u32 pc, u32 imm) -> const void* { return pc == 0x40 && imm == 0x10 ? &token : nullptr; };
    A32::Jit jit{conf};
    env.jit = &jit;

    constexpr std::array<u32, 3> program{0xef000010, 0xe2801001, 0xef0000ff};  // svc #0x10; add r1, r0, #1; svc #0xff
    std::memcpy(arena.base, program.data(), sizeof(program));
    jit.Regs()[0] = 21;
    jit.Regs()[15] = 0;
    jit.SetCpsr(0x000001d0);
    jit.SetFpscr(fpcr_round_toward_zero);
    SetHostFpcr(0);
    REQUIRE(jit.Run() == HaltReason::UserDefined2);
    REQUIRE(env.fast_calls == 1);
    REQUIRE(jit.Regs()[0] == 42);
    REQUIRE(jit.Regs()[1] == 43);
    REQUIRE(env.fpcr_inside == fpcr_round_toward_zero);  // svc_fast keeps the guest's

    constexpr std::array<u32, 3> transfer{0xef000012, 0xe2801001, 0xef0000ff};
    std::memcpy(arena.base, transfer.data(), sizeof(transfer));
    jit.ClearCache();
    jit.Regs()[15] = 0;
    REQUIRE(jit.Run() == HaltReason::UserDefined2);
    REQUIRE(env.fpcr_inside == 0);  // svc_fp_transfer switches to the host's

    std::memcpy(arena.base + 0x40, program.data(), sizeof(program));
    jit.ClearCache();
    jit.Regs()[0] = 1;
    jit.Regs()[15] = 0x40;
    REQUIRE(jit.Run() == HaltReason::UserDefined2);
    REQUIRE(env.known_calls == 1);
    REQUIRE(jit.Regs()[1] == 7);
}
