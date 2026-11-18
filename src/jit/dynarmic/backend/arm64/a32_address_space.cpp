// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/a32_address_space.h"

#include <fmt/format.h>

#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/code_registration.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::Backend::Arm64 {

namespace {

template<auto callback, typename T>
T ExclusiveRead(const A32::UserConfig& conf, A32::VAddr vaddr) {
    return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
        return T((conf.callbacks->*callback)(vaddr, sizeof(T)));
    });
}

template<auto callback, typename T>
u32 ExclusiveWrite(const A32::UserConfig& conf, A32::VAddr vaddr, T value) {
    return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr, [&](T expected) -> bool {
        return (conf.callbacks->*callback)(vaddr, value, expected);
    }) ? 0 : 1;
}

template<typename F>
u64 FunctionAddress(F* fn) {
    return std::bit_cast<u64>(fn);
}

}  // namespace

A32AddressSpace::A32AddressSpace(const A32::UserConfig& conf)
        : AddressSpace(conf.code_cache_size, conf.code_allocation_failed)
        , conf(conf) {
    EmitPrelude();
}

void A32AddressSpace::GenerateIR(IR::Block& ir_block, IR::LocationDescriptor descriptor) const {
    ir_block.Reset(descriptor);
    A32::Translate(ir_block, A32::LocationDescriptor{descriptor}, conf.callbacks, {conf.arch_version, conf.define_unpredictable_behaviour, conf.hook_hint_instructions});
    Optimization::Optimize(ir_block, conf, {});
}

void A32AddressSpace::InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges) {
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
}

void A32AddressSpace::EmitPrelude() {
    using namespace oaknut::util;

    UnprotectCodeMemory();

    u32* const prelude_start = code.xptr<u32*>();
    const u64 conf_ptr = std::bit_cast<u64>(&conf);

    // Plain trampolines are called after RegAlloc::PrepareForCall with X1.. already set.
    const auto call = [&](DevirtualizedCall info, CallFp fp = CallFp::Host, const std::function<void()>& args = {}) {
        return EmitCallTrampoline(info.this_ptr, info.fn_ptr, 0, fp, args);
    };
    const auto conf_call = [&](u64 fn) {
        return EmitCallTrampoline(conf_ptr, fn, 0, CallFp::Host);
    };
    const auto read = [&](std::size_t bitsize) {
        return call(Devirtualize<&A32::UserCallbacks::MemoryRead>(conf.callbacks), CallFp::Host, [&] { code.MOV(X2, bitsize); });
    };
    const auto write = [&](std::size_t bitsize) {
        return call(Devirtualize<&A32::UserCallbacks::MemoryWrite>(conf.callbacks), CallFp::Host, [&] { code.MOV(X3, bitsize); });
    };
    // Wrapped trampolines keep the caller-saved registers: vaddr in Xscratch0, value in Xscratch1.
    const auto wrapped_read = [&](std::size_t bitsize) {
        const auto info = Devirtualize<&A32::UserCallbacks::MemoryRead>(conf.callbacks);
        return EmitCallTrampoline(info.this_ptr, info.fn_ptr, ABI_CALLER_SAVE & ~ToRegList(Xscratch0), CallFp::Host, [&] {
                code.MOV(X1, Xscratch0);
                code.MOV(X2, bitsize); }, [&] { code.MOV(Xscratch0, X0); });
    };
    const auto wrapped_write = [&](std::size_t bitsize) {
        const auto info = Devirtualize<&A32::UserCallbacks::MemoryWrite>(conf.callbacks);
        return EmitCallTrampoline(info.this_ptr, info.fn_ptr, ABI_CALLER_SAVE, CallFp::Host, [&] {
                code.MOV(X1, Xscratch0);
                code.MOV(X2, Xscratch1);
                code.MOV(X3, bitsize); });
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
    prelude_info.wrapped_read_memory_8 = wrapped_read(sizeof(u8));
    prelude_info.wrapped_read_memory_16 = wrapped_read(sizeof(u16));
    prelude_info.wrapped_read_memory_32 = wrapped_read(sizeof(u32));
    prelude_info.wrapped_read_memory_64 = wrapped_read(sizeof(u64));
    prelude_info.exclusive_read_memory_8 = conf_call(FunctionAddress(&ExclusiveRead<&A32::UserCallbacks::MemoryRead, u8>));
    prelude_info.exclusive_read_memory_16 = conf_call(FunctionAddress(&ExclusiveRead<&A32::UserCallbacks::MemoryRead, u16>));
    prelude_info.exclusive_read_memory_32 = conf_call(FunctionAddress(&ExclusiveRead<&A32::UserCallbacks::MemoryRead, u32>));
    prelude_info.exclusive_read_memory_64 = conf_call(FunctionAddress(&ExclusiveRead<&A32::UserCallbacks::MemoryRead, u64>));
    prelude_info.write_memory_8 = write(sizeof(u8));
    prelude_info.write_memory_16 = write(sizeof(u16));
    prelude_info.write_memory_32 = write(sizeof(u32));
    prelude_info.write_memory_64 = write(sizeof(u64));
    prelude_info.wrapped_write_memory_8 = wrapped_write(sizeof(u8));
    prelude_info.wrapped_write_memory_16 = wrapped_write(sizeof(u16));
    prelude_info.wrapped_write_memory_32 = wrapped_write(sizeof(u32));
    prelude_info.wrapped_write_memory_64 = wrapped_write(sizeof(u64));
    prelude_info.exclusive_write_memory_8 = conf_call(FunctionAddress(&ExclusiveWrite<&A32::UserCallbacks::MemoryWriteExclusive8, u8>));
    prelude_info.exclusive_write_memory_16 = conf_call(FunctionAddress(&ExclusiveWrite<&A32::UserCallbacks::MemoryWriteExclusive16, u16>));
    prelude_info.exclusive_write_memory_32 = conf_call(FunctionAddress(&ExclusiveWrite<&A32::UserCallbacks::MemoryWriteExclusive32, u32>));
    prelude_info.exclusive_write_memory_64 = conf_call(FunctionAddress(&ExclusiveWrite<&A32::UserCallbacks::MemoryWriteExclusive64, u64>));
    prelude_info.wrapped_compare_write_memory_8 = wrapped_compare_write(Devirtualize<&A32::UserCallbacks::MemoryWriteExclusive8>(conf.callbacks));
    prelude_info.wrapped_compare_write_memory_16 = wrapped_compare_write(Devirtualize<&A32::UserCallbacks::MemoryWriteExclusive16>(conf.callbacks));
    prelude_info.wrapped_compare_write_memory_32 = wrapped_compare_write(Devirtualize<&A32::UserCallbacks::MemoryWriteExclusive32>(conf.callbacks));
    prelude_info.wrapped_compare_write_memory_64 = wrapped_compare_write(Devirtualize<&A32::UserCallbacks::MemoryWriteExclusive64>(conf.callbacks));
    prelude_info.call_svc = call(Devirtualize<&A32::UserCallbacks::CallSVC>(conf.callbacks));
    prelude_info.call_svc_fast = call(Devirtualize<&A32::UserCallbacks::CallSVCFast>(conf.callbacks), CallFp::Guest);
    prelude_info.call_svc_fast_host_fp = call(Devirtualize<&A32::UserCallbacks::CallSVCFast>(conf.callbacks));
    prelude_info.call_svc_fast_known = call(Devirtualize<&A32::UserCallbacks::CallSVCFastKnown>(conf.callbacks), CallFp::Guest);
    prelude_info.call_svc_fast_known_host_fp = call(Devirtualize<&A32::UserCallbacks::CallSVCFastKnown>(conf.callbacks));
    prelude_info.exception_raised = call(Devirtualize<&A32::UserCallbacks::ExceptionRaised>(conf.callbacks));
    prelude_info.isb_raised = call(Devirtualize<&A32::UserCallbacks::InstructionSynchronizationBarrierRaised>(conf.callbacks));
    prelude_info.add_ticks = call(Devirtualize<&A32::UserCallbacks::AddTicks>(conf.callbacks));
    prelude_info.get_ticks_remaining = call(Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(conf.callbacks));

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

        const auto fn = [](A32AddressSpace& self, A32JitState& context) -> CodePtr {
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

EmitConfig A32AddressSpace::GetEmitConfig() {
    std::function<const void*(u64, u32)> svc_lookup;
    if (conf.svc_lookup) {
        svc_lookup = [this](u64 pc, u32 imm) { return conf.svc_lookup(static_cast<A32::VAddr>(pc), imm); };
    }
    return EmitConfig{
        .optimizations = conf.unsafe_optimizations ? conf.optimizations : conf.optimizations & all_safe_optimizations,

        .hook_isb = conf.hook_isb,

        .cntfreq_el0{},
        .ctr_el0{},
        .dczid_el0{},
        .tpidrro_el0{},
        .tpidr_el0{},

        .check_halt_on_memory_access = conf.check_halt_on_memory_access,

        .page_table_pointer = std::bit_cast<u64>(conf.page_table),
        .page_table_address_space_bits = 32,
        .page_table_pointer_mask = conf.page_table_pointer_mask,
        .page_table_log2_stride = conf.page_table_log2_stride,
        .page_table_marked_bit = conf.page_table_marked_bit,
        .page_table_sign_extension = conf.page_table_sign_extension,
        .silently_mirror_page_table = true,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .detect_misaligned_access_via_page_table = conf.detect_misaligned_access_via_page_table,
        .only_detect_misalignment_via_page_table_on_page_boundary = conf.only_detect_misalignment_via_page_table_on_page_boundary,

        .fastmem_pointer = conf.fastmem_pointer,
        .recompile_on_fastmem_failure = conf.recompile_on_fastmem_failure,
        .fastmem_address_space_bits = 32,
        .silently_mirror_fastmem = true,

        .global_monitor = conf.global_monitor,
        .processor_id = conf.processor_id,
        .fastmem_exclusive_access = conf.fastmem_exclusive_access,
        .elide_unchanged_exclusive_stores = conf.elide_unchanged_exclusive_stores,
        .recompile_on_exclusive_fastmem_failure = conf.recompile_on_exclusive_fastmem_failure,

        .wall_clock_cntpct = conf.wall_clock_cntpct,
        .enable_cycle_counting = conf.enable_cycle_counting,
        .enable_bounded_halt_polling = false,

        .svc_fast = conf.svc_fast,
        .svc_fp_transfer = conf.svc_fp_transfer,
        .svc_fp_guest = std::nullopt,
        .svc_lookup = std::move(svc_lookup),

        .always_little_endian = conf.always_little_endian,

        .descriptor_to_fpcr = [](const IR::LocationDescriptor& location) { return FP::FPCR{A32::LocationDescriptor{location}.FPSCR().Value()}; },
        .emit_cond = EmitA32Cond,
        .emit_condition_failed_terminal = EmitA32ConditionFailedTerminal,
        .emit_terminal = EmitA32Terminal,
        .emit_check_memory_abort = EmitA32CheckMemoryAbort,

        .state_nzcv_offset = offsetof(A32JitState, cpsr_nzcv),
        .state_fpsr_offset = offsetof(A32JitState, fpsr),
        .state_exclusive_state_offset = offsetof(A32JitState, exclusive_state),

        .coprocessors = conf.coprocessors,

        .very_verbose_debugging_output = conf.very_verbose_debugging_output,
    };
}

void A32AddressSpace::RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) {
    const A32::LocationDescriptor descriptor{block.Location()};
    const A32::LocationDescriptor end_location{block.EndLocation()};
    const auto range = boost::icl::discrete_interval<u32>::closed(descriptor.PC(), end_location.PC() - 1);
    block_ranges.AddRange(range, descriptor);
    if (const auto cb = code_registration.load(std::memory_order_relaxed)) {
        cb(block_info.entry_point, block_info.entry_point + block_info.size,
           fmt::format("a32_{}{:08X}_{}_fpcr{:08X}", descriptor.TFlag() ? "t" : "a", descriptor.PC(), descriptor.EFlag() ? "be" : "le", descriptor.FPSCR().Value()));
    }
}

std::pair<u64, bool> A32AddressSpace::DescribeLocation(IR::LocationDescriptor descriptor) const {
    return {A32::LocationDescriptor{descriptor}.PC(), true};
}

void A32AddressSpace::EmitLoadGuestFpcr(oaknut::WReg dest) {
    code.LDR(dest, Xstate, offsetof(A32JitState, upper_location_descriptor));
    code.AND(dest, dest, 0xffff0000);
}

std::size_t A32AddressSpace::GuestFpsrOffset() const {
    return offsetof(A32JitState, fpsr);
}

}  // namespace Dynarmic::Backend::Arm64
