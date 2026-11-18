// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/a64_address_space.h"

#include <fmt/format.h>

#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/code_registration.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::Backend::Arm64 {

namespace {

template<auto callback, typename T>
T ExclusiveRead(const A64::UserConfig& conf, A64::VAddr vaddr) {
    return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
        return T((conf.callbacks->*callback)(vaddr, sizeof(T)));
    });
}

Vector ExclusiveRead128(const A64::UserConfig& conf, A64::VAddr vaddr) {
    return conf.global_monitor->ReadAndMark<Vector>(conf.processor_id, vaddr, [&]() -> Vector {
        return conf.callbacks->MemoryRead128(vaddr);
    });
}

template<auto callback, typename T>
u32 ExclusiveWrite(const A64::UserConfig& conf, A64::VAddr vaddr, T value) {
    return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr, [&](T expected) -> bool {
        return (conf.callbacks->*callback)(vaddr, value, expected);
    }) ? 0 : 1;
}

u32 ExclusiveWrite128(const A64::UserConfig& conf, A64::VAddr vaddr, Vector value) {
    return conf.global_monitor->DoExclusiveOperation<Vector>(conf.processor_id, vaddr, [&](Vector expected) -> bool {
        return conf.callbacks->MemoryWriteExclusive128(vaddr, value, expected);
    }) ? 0 : 1;
}

// LSE atomics that cannot use fastmem. With the global monitor they also break every
// reservation, as an exclusive store does.
template<typename Function>
auto DoAtomic(const A64::UserConfig& conf, Function call) {
    if (!conf.global_monitor || conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return call();
    }
    return conf.global_monitor->DoAtomicOperation(call);
}

template<std::size_t bitsize, bool compare_and_swap>
u64 AtomicMemory(const A64::UserConfig& conf, A64::VAddr vaddr, u64 value, u64 extra) {
    constexpr u64 mask = bitsize == 64 ? ~u64(0) : (u64(1) << bitsize) - 1;
    const auto op = compare_and_swap ? A64::AtomicOp::CompareAndSwap : static_cast<A64::AtomicOp>(extra);
    const u64 desired = compare_and_swap ? extra & mask : 0;
    return DoAtomic(conf, [&] {
        return conf.callbacks->MemoryAtomic(vaddr, bitsize / 8, op, value & mask, desired) & mask;
    });
}

Vector CompareAndSwapMemory128(const A64::UserConfig& conf, A64::VAddr vaddr, Vector expected, Vector desired) {
    return DoAtomic(conf, [&] {
        return conf.callbacks->MemoryAtomicCompareAndSwap128(vaddr, expected, desired);
    });
}

template<typename F>
u64 FunctionAddress(F* fn) {
    return std::bit_cast<u64>(fn);
}

}  // namespace

A64AddressSpace::A64AddressSpace(const A64::UserConfig& conf)
        : AddressSpace(conf.code_cache_size, conf.code_allocation_failed)
        , conf(conf) {
    EmitPrelude();
}

void A64AddressSpace::GenerateIR(IR::Block& ir_block, IR::LocationDescriptor descriptor) const {
    ir_block.Reset(descriptor);
    const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
    inlined_code.clear();
    A64::Translate(ir_block, A64::LocationDescriptor{descriptor}, get_code,
                   {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct, true,
                    conf.enable_bounded_halt_polling && !conf.enable_cycle_counting,
                    conf.inline_outlined_atomics && !conf.diagnostic_entry.record, &inlined_code,
                    conf.diagnostic_entry.record ? conf.diagnostic_entry.selected : std::function<bool(u64)>{}});
    Optimization::Optimize(ir_block, conf, {});
}

void A64AddressSpace::InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges) {
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
}

void A64AddressSpace::EmitPrelude() {
    using namespace oaknut::util;

    UnprotectCodeMemory();

    u32* const prelude_start = code.xptr<u32*>();
    const u64 conf_ptr = std::bit_cast<u64>(&conf);

    // Plain trampolines are called after RegAlloc::PrepareForCall with X1.. already set.
    const auto call = [&](DevirtualizedCall info, CallFp fp = CallFp::Host, const std::function<void()>& args = {}, const std::function<void()>& result = {}) {
        return EmitCallTrampoline(info.this_ptr, info.fn_ptr, 0, fp, args, result);
    };
    const auto conf_call = [&](u64 fn, const std::function<void()>& args = {}, const std::function<void()>& result = {}) {
        return EmitCallTrampoline(conf_ptr, fn, 0, CallFp::Host, args, result);
    };
    const auto read = [&](std::size_t bitsize) {
        return call(Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks), CallFp::Host, [&] { code.MOV(X2, bitsize); });
    };
    const auto write = [&](std::size_t bitsize) {
        return call(Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks), CallFp::Host, [&] { code.MOV(X3, bitsize); });
    };
    const auto result_to_q0 = [&] {
        code.FMOV(D0, X0);
        code.FMOV(V0.D()[1], X1);
    };
    const auto q0_to_x2_x3 = [&] {
        code.FMOV(X2, D0);
        code.FMOV(X3, V0.D()[1]);
    };
    // Wrapped trampolines keep the caller-saved registers: vaddr in Xscratch0, value in Xscratch1.
    const auto wrapped_read = [&](std::size_t bitsize) {
        const auto info = Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks);
        return EmitCallTrampoline(info.this_ptr, info.fn_ptr, ABI_CALLER_SAVE & ~ToRegList(Xscratch0), CallFp::Host, [&] {
                code.MOV(X1, Xscratch0);
                code.MOV(X2, bitsize); }, [&] { code.MOV(Xscratch0, X0); });
    };
    const auto wrapped_write = [&](std::size_t bitsize) {
        const auto info = Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks);
        return EmitCallTrampoline(info.this_ptr, info.fn_ptr, ABI_CALLER_SAVE, CallFp::Host, [&] {
                code.MOV(X1, Xscratch0);
                code.MOV(X2, Xscratch1);
                code.MOV(X3, bitsize); });
    };
    const auto wrapped_atomic = [&](u64 fn) {
        return EmitCallTrampoline(conf_ptr, fn, ABI_CALLER_SAVE & ~ToRegList(Xscratch0), CallFp::Host, [&] {
                code.MOV(X1, Xscratch0);
                code.MOV(X2, Xscratch1);
                code.FMOV(X3, D0); }, [&] { code.MOV(Xscratch0, X0); });
    };
    const auto wrapped_compare_write = [&](DevirtualizedCall info) {
        return EmitCallTrampoline(info.this_ptr, info.fn_ptr, ABI_CALLER_SAVE & ~ToRegList(Xscratch0), CallFp::Host, [&] {
                code.MOV(X1, Xscratch0);
                code.MOV(X2, Xscratch1);
                code.FMOV(X3, D0); }, [&] { code.AND(Xscratch0, X0, 0xff); });
    };

    prelude_info.read_memory_8 = read(sizeof(u8));
    prelude_info.read_memory_16 = read(sizeof(u16));
    prelude_info.read_memory_32 = read(sizeof(u32));
    prelude_info.read_memory_64 = read(sizeof(u64));
    prelude_info.read_memory_128 = call(Devirtualize<&A64::UserCallbacks::MemoryRead128>(conf.callbacks), CallFp::Host, {}, result_to_q0);
    prelude_info.wrapped_read_memory_8 = wrapped_read(sizeof(u8));
    prelude_info.wrapped_read_memory_16 = wrapped_read(sizeof(u16));
    prelude_info.wrapped_read_memory_32 = wrapped_read(sizeof(u32));
    prelude_info.wrapped_read_memory_64 = wrapped_read(sizeof(u64));
    {
        const auto info = Devirtualize<&A64::UserCallbacks::MemoryRead128>(conf.callbacks);
        prelude_info.wrapped_read_memory_128 = EmitCallTrampoline(info.this_ptr, info.fn_ptr, ABI_CALLER_SAVE & ~ToRegList(Q0), CallFp::Host, [&] { code.MOV(X1, Xscratch0); }, result_to_q0);
    }
    prelude_info.exclusive_read_memory_8 = conf_call(FunctionAddress(&ExclusiveRead<&A64::UserCallbacks::MemoryRead, u8>));
    prelude_info.exclusive_read_memory_16 = conf_call(FunctionAddress(&ExclusiveRead<&A64::UserCallbacks::MemoryRead, u16>));
    prelude_info.exclusive_read_memory_32 = conf_call(FunctionAddress(&ExclusiveRead<&A64::UserCallbacks::MemoryRead, u32>));
    prelude_info.exclusive_read_memory_64 = conf_call(FunctionAddress(&ExclusiveRead<&A64::UserCallbacks::MemoryRead, u64>));
    prelude_info.exclusive_read_memory_128 = conf_call(FunctionAddress(&ExclusiveRead128), {}, result_to_q0);
    prelude_info.write_memory_8 = write(sizeof(u8));
    prelude_info.write_memory_16 = write(sizeof(u16));
    prelude_info.write_memory_32 = write(sizeof(u32));
    prelude_info.write_memory_64 = write(sizeof(u64));
    prelude_info.write_memory_128 = call(Devirtualize<&A64::UserCallbacks::MemoryWrite128>(conf.callbacks), CallFp::Host, q0_to_x2_x3);
    prelude_info.wrapped_write_memory_8 = wrapped_write(sizeof(u8));
    prelude_info.wrapped_write_memory_16 = wrapped_write(sizeof(u16));
    prelude_info.wrapped_write_memory_32 = wrapped_write(sizeof(u32));
    prelude_info.wrapped_write_memory_64 = wrapped_write(sizeof(u64));
    {
        const auto info = Devirtualize<&A64::UserCallbacks::MemoryWrite128>(conf.callbacks);
        prelude_info.wrapped_write_memory_128 = EmitCallTrampoline(info.this_ptr, info.fn_ptr, ABI_CALLER_SAVE, CallFp::Host, [&] {
            code.MOV(X1, Xscratch0);
            q0_to_x2_x3();
        });
    }
    prelude_info.exclusive_write_memory_8 = conf_call(FunctionAddress(&ExclusiveWrite<&A64::UserCallbacks::MemoryWriteExclusive8, u8>));
    prelude_info.exclusive_write_memory_16 = conf_call(FunctionAddress(&ExclusiveWrite<&A64::UserCallbacks::MemoryWriteExclusive16, u16>));
    prelude_info.exclusive_write_memory_32 = conf_call(FunctionAddress(&ExclusiveWrite<&A64::UserCallbacks::MemoryWriteExclusive32, u32>));
    prelude_info.exclusive_write_memory_64 = conf_call(FunctionAddress(&ExclusiveWrite<&A64::UserCallbacks::MemoryWriteExclusive64, u64>));
    prelude_info.exclusive_write_memory_128 = conf_call(FunctionAddress(&ExclusiveWrite128), q0_to_x2_x3);
    prelude_info.wrapped_compare_write_memory_8 = wrapped_compare_write(Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive8>(conf.callbacks));
    prelude_info.wrapped_compare_write_memory_16 = wrapped_compare_write(Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive16>(conf.callbacks));
    prelude_info.wrapped_compare_write_memory_32 = wrapped_compare_write(Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive32>(conf.callbacks));
    prelude_info.wrapped_compare_write_memory_64 = wrapped_compare_write(Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive64>(conf.callbacks));
    prelude_info.compare_write_memory_128 = call(Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive128>(conf.callbacks), CallFp::Host, [&] {
            q0_to_x2_x3();
            code.FMOV(X4, D1);
            code.FMOV(X5, V1.D()[1]); }, [&] { code.AND(W0, W0, 0xff); });
    prelude_info.wrapped_atomic_memory_8 = wrapped_atomic(FunctionAddress(&AtomicMemory<8, false>));
    prelude_info.wrapped_atomic_memory_16 = wrapped_atomic(FunctionAddress(&AtomicMemory<16, false>));
    prelude_info.wrapped_atomic_memory_32 = wrapped_atomic(FunctionAddress(&AtomicMemory<32, false>));
    prelude_info.wrapped_atomic_memory_64 = wrapped_atomic(FunctionAddress(&AtomicMemory<64, false>));
    prelude_info.wrapped_compare_and_swap_memory_8 = wrapped_atomic(FunctionAddress(&AtomicMemory<8, true>));
    prelude_info.wrapped_compare_and_swap_memory_16 = wrapped_atomic(FunctionAddress(&AtomicMemory<16, true>));
    prelude_info.wrapped_compare_and_swap_memory_32 = wrapped_atomic(FunctionAddress(&AtomicMemory<32, true>));
    prelude_info.wrapped_compare_and_swap_memory_64 = wrapped_atomic(FunctionAddress(&AtomicMemory<64, true>));
    prelude_info.compare_and_swap_memory_128 = conf_call(FunctionAddress(&CompareAndSwapMemory128), [&] {
            q0_to_x2_x3();
            code.FMOV(X4, D1);
            code.FMOV(X5, V1.D()[1]); }, result_to_q0);
    prelude_info.call_svc = call(Devirtualize<&A64::UserCallbacks::CallSVC>(conf.callbacks));
    prelude_info.call_svc_fast = call(Devirtualize<&A64::UserCallbacks::CallSVCFast>(conf.callbacks), CallFp::Guest);
    prelude_info.call_svc_fast_host_fp = call(Devirtualize<&A64::UserCallbacks::CallSVCFast>(conf.callbacks));
    prelude_info.call_svc_fast_known = call(Devirtualize<&A64::UserCallbacks::CallSVCFastKnown>(conf.callbacks), CallFp::Guest);
    prelude_info.call_svc_fast_known_host_fp = call(Devirtualize<&A64::UserCallbacks::CallSVCFastKnown>(conf.callbacks));
    prelude_info.exception_raised = call(Devirtualize<&A64::UserCallbacks::ExceptionRaised>(conf.callbacks));
    prelude_info.isb_raised = call(Devirtualize<&A64::UserCallbacks::InstructionSynchronizationBarrierRaised>(conf.callbacks));
    prelude_info.ic_raised = call(Devirtualize<&A64::UserCallbacks::InstructionCacheOperationRaised>(conf.callbacks));
    prelude_info.dc_raised = call(Devirtualize<&A64::UserCallbacks::DataCacheOperationRaised>(conf.callbacks));
    prelude_info.get_cntpct = call(Devirtualize<&A64::UserCallbacks::GetCNTPCT>(conf.callbacks));
    prelude_info.add_ticks = call(Devirtualize<&A64::UserCallbacks::AddTicks>(conf.callbacks));
    prelude_info.get_ticks_remaining = call(Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(conf.callbacks));
    if (conf.diagnostic_entry.record) {
        prelude_info.diagnostic_entry = EmitCallTrampoline(std::bit_cast<u64>(conf.diagnostic_entry.user), FunctionAddress(conf.diagnostic_entry.record), 0, CallFp::Host);
    }

    oaknut::Label return_from_run_code, l_return_to_dispatcher;

    const auto emit_run_entry = [&] {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
        if (conf.page_table) {
            code.MOV(Xpagetable, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        EmitEnterGuestFpFromRun();

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (std::size_t i = 0; i < RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }
    };

    prelude_info.run_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        emit_run_entry();

        if (conf.enable_cycle_counting) {
            code.BL(prelude_info.get_ticks_remaining);
            code.MOV(Xticks, X0);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        code.BR(X19);
    }

    prelude_info.step_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        emit_run_entry();

        if (conf.enable_cycle_counting) {
            code.MOV(Xticks, 1);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        oaknut::Label step_hr_loop;
        code.l(step_hr_loop);
        code.LDAXR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);
        code.ORR(Wscratch0, Wscratch0, static_cast<u32>(HaltReason::Step));
        code.STLXR(Wscratch1, Wscratch0, Xhalt);
        code.CBNZ(Wscratch1, step_hr_loop);

        code.BR(X19);
    }

    prelude_info.return_to_dispatcher = code.xptr<void*>();
    {
        oaknut::Label l_this, l_addr;

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, return_from_run_code);
        }

        code.LDR(X0, l_this);
        code.MOV(X1, Xstate);
        code.LDR(Xscratch0, l_addr);
        code.BLR(Xscratch0);
        code.BR(X0);

        const auto fn = [](A64AddressSpace& self, A64JitState& context) -> CodePtr {
            return self.GetOrEmit(context.GetLocationDescriptor());
        };

        code.align(8);
        code.l(l_this);
        code.dx(std::bit_cast<u64>(this));
        code.l(l_addr);
        code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));
    }

    prelude_info.return_from_run_code = code.xptr<void*>();
    {
        code.l(return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
            code.SUB(X1, X1, Xticks);
            code.BL(prelude_info.add_ticks);
        }

        EmitLeaveGuestFpForRun();

        oaknut::Label exit_hr_loop;
        code.l(exit_hr_loop);
        code.LDAXR(W0, Xhalt);
        code.STLXR(Wscratch0, WZR, Xhalt);
        code.CBNZ(Wscratch0, exit_hr_loop);

        ABI_PopRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));
        code.RET();
    }

    code.align(8);
    code.l(l_return_to_dispatcher);
    code.dx(std::bit_cast<u64>(prelude_info.return_to_dispatcher));

    prelude_info.end_of_prelude = code.offset();

    mem.invalidate(prelude_start, code.xptr<std::byte*>() - reinterpret_cast<std::byte*>(prelude_start));
    ProtectCodeMemory();
}

EmitConfig A64AddressSpace::GetEmitConfig() {
    return EmitConfig{
        .optimizations = conf.unsafe_optimizations ? conf.optimizations : conf.optimizations & all_safe_optimizations,

        .hook_isb = conf.hook_isb,

        .cntfreq_el0 = conf.cntfrq_el0,
        .ctr_el0 = conf.ctr_el0,
        .dczid_el0 = conf.dczid_el0,
        .tpidrro_el0 = conf.tpidrro_el0,
        .tpidr_el0 = conf.tpidr_el0,

        .check_halt_on_memory_access = conf.check_halt_on_memory_access,

        .page_table_pointer = std::bit_cast<u64>(conf.page_table),
        .page_table_address_space_bits = conf.page_table_address_space_bits,
        .page_table_pointer_mask = conf.page_table_pointer_mask,
        .page_table_log2_stride = conf.page_table_log2_stride,
        .page_table_marked_bit = conf.page_table_marked_bit,
        .page_table_sign_extension = conf.page_table_sign_extension,
        .silently_mirror_page_table = conf.silently_mirror_page_table,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .detect_misaligned_access_via_page_table = conf.detect_misaligned_access_via_page_table,
        .only_detect_misalignment_via_page_table_on_page_boundary = conf.only_detect_misalignment_via_page_table_on_page_boundary,

        .fastmem_pointer = conf.fastmem_pointer,
        .recompile_on_fastmem_failure = conf.recompile_on_fastmem_failure,
        .fastmem_address_space_bits = conf.fastmem_address_space_bits,
        .silently_mirror_fastmem = conf.silently_mirror_fastmem,

        .global_monitor = conf.global_monitor,
        .processor_id = conf.processor_id,
        .fastmem_exclusive_access = conf.fastmem_exclusive_access,
        .elide_unchanged_exclusive_stores = conf.elide_unchanged_exclusive_stores,
        .recompile_on_exclusive_fastmem_failure = conf.recompile_on_exclusive_fastmem_failure,

        .wall_clock_cntpct = conf.wall_clock_cntpct,
        .enable_cycle_counting = conf.enable_cycle_counting,
        .enable_bounded_halt_polling = conf.enable_bounded_halt_polling && !conf.enable_cycle_counting,

        .svc_fast = conf.svc_fast,
        .svc_fp_transfer = conf.svc_fp_transfer,
        .svc_fp_guest = conf.svc_fp_guest,
        .svc_lookup = conf.svc_lookup,
        .diagnostic_entry = conf.diagnostic_entry.record && conf.diagnostic_entry.selected
                              ? std::function<std::optional<u64>(const IR::LocationDescriptor&)>{[selected = conf.diagnostic_entry.selected](const IR::LocationDescriptor& location) -> std::optional<u64> {
                                    const u64 pc = A64::LocationDescriptor{location}.PC();
                                    return selected(pc) ? std::optional<u64>{pc} : std::nullopt;
                                }}
                              : std::function<std::optional<u64>(const IR::LocationDescriptor&)>{},

        .always_little_endian = true,

        .descriptor_to_fpcr = [](const IR::LocationDescriptor& location) { return A64::LocationDescriptor{location}.FPCR(); },
        .emit_cond = EmitA64Cond,
        .emit_condition_failed_terminal = EmitA64ConditionFailedTerminal,
        .emit_terminal = EmitA64Terminal,
        .emit_check_memory_abort = EmitA64CheckMemoryAbort,

        .state_nzcv_offset = offsetof(A64JitState, cpsr_nzcv),
        .state_fpsr_offset = offsetof(A64JitState, fpsr),
        .state_exclusive_state_offset = offsetof(A64JitState, exclusive_state),

        .coprocessors{},

        .very_verbose_debugging_output = conf.very_verbose_debugging_output,
    };
}

void A64AddressSpace::RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) {
    const A64::LocationDescriptor descriptor{block.Location()};
    const A64::LocationDescriptor end_location{block.EndLocation()};
    const auto range = boost::icl::discrete_interval<u64>::closed(descriptor.PC(), end_location.PC() - 1);
    block_ranges.AddRange(range, descriptor);
    for (const auto& [begin, end] : inlined_code) {
        block_ranges.AddRange(boost::icl::discrete_interval<u64>::closed(begin, end - 1), descriptor);
    }
    if (const auto cb = code_registration.load(std::memory_order_relaxed)) {
        cb(block_info.entry_point, block_info.entry_point + block_info.size, fmt::format("a64_{:016X}_fpcr{:08X}", descriptor.PC(), descriptor.FPCR().Value()));
    }
}

std::pair<u64, bool> A64AddressSpace::DescribeLocation(IR::LocationDescriptor descriptor) const {
    return {A64::LocationDescriptor{descriptor}.PC(), false};
}

void A64AddressSpace::EmitLoadGuestFpcr(oaknut::WReg dest) {
    code.LDR(dest, Xstate, offsetof(A64JitState, fpcr));
}

std::size_t A64AddressSpace::GuestFpsrOffset() const {
    return offsetof(A64JitState, fpsr);
}

}  // namespace Dynarmic::Backend::Arm64
