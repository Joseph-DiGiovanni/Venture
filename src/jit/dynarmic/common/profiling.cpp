// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Backend-neutral profiling hooks (interface/profiling.h).

#include <chrono>

#include "dynarmic/common/code_registration.h"

namespace Dynarmic {

std::atomic<CodeRegistrationCallback> code_registration{nullptr};

namespace {
std::atomic<CompileTimingCallback> compile_timing_callback{nullptr};
thread_local CompileTimingScope* current_compile = nullptr;
std::uint64_t CompileNow() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
void SetCompileTimingCallback(CompileTimingCallback cb) { compile_timing_callback.store(cb); }
CompileTimingScope::CompileTimingScope(std::uint64_t pc, bool a32, std::uint64_t descriptor) noexcept {
    callback = compile_timing_callback.load(std::memory_order_relaxed);
    if (!callback) return;
    timing.guest_pc = pc; timing.a32 = a32; timing.location_descriptor = descriptor;
    timing.begin_ns = last_ns = CompileNow();
    previous = current_compile; current_compile = this;
}
void SetCompilePhase(CompilePhase phase) noexcept {
    if (!current_compile) return;
    const auto now = CompileNow();
    current_compile->timing.phase_ns[std::size_t(current_compile->phase)] += now - current_compile->last_ns;
    current_compile->last_ns = now; current_compile->phase = phase;
}
CompileTimingScope::~CompileTimingScope() {
    if (!callback) return;
    SetCompilePhase(phase);
    timing.end_ns = last_ns;
    current_compile = previous;
    // Exclude a nested compilation from its parent's exclusive phase time.
    if (previous) {
        const auto nested = timing.end_ns - timing.begin_ns;
        previous->last_ns += nested;
    }
    callback(timing);
}
void SetCodeRegistrationCallback(CodeRegistrationCallback cb) { code_registration.store(cb); }

}  // namespace Dynarmic
