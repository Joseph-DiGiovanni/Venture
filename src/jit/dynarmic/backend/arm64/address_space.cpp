// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstdio>
#include <new>

#include <bit>
#if !defined(_WIN32)
#    include <sys/mman.h>
#endif

#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/code_registration.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

CodeMemory::CodeMemory(std::size_t size, const std::function<void(std::size_t, std::size_t, std::size_t)>& allocation_failed)
        : oaknut::CodeBlock(size) {
#if !defined(_WIN32)
    if (m_memory == reinterpret_cast<std::uint32_t*>(MAP_FAILED)) {
        m_memory = nullptr;
        if (allocation_failed) {
            allocation_failed(size, 0, size);
        }
        throw std::bad_alloc{};
    }
#else
    (void)allocation_failed;
#endif
}

namespace {

// Compilation runs from the dispatcher with the guest FP environment installed. It uses
// the default environment and leaves the guest's mode and flags as they were.
class CompileFpEnvironment {
public:
    CompileFpEnvironment() {
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
        __asm__ volatile("msr fpcr, xzr");
    }
    ~CompileFpEnvironment() {
        __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
        __asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
    }
    CompileFpEnvironment(const CompileFpEnvironment&) = delete;
    CompileFpEnvironment& operator=(const CompileFpEnvironment&) = delete;

private:
    u64 fpcr;
    u64 fpsr;
};

}  // namespace

AddressSpace::AddressSpace(std::size_t code_cache_size, const std::function<void(std::size_t, std::size_t, std::size_t)>& code_allocation_failed)
    : ir_block{IR::LocationDescriptor{0}}
    , code_cache_size(code_cache_size)
    , mem(code_cache_size, code_allocation_failed)
    , code(mem.ptr(), mem.ptr())
    , fastmem_manager(exception_handler)
{
    ASSERT(code_cache_size <= 128 * 1024 * 1024 && "code_cache_size > 128 MiB not currently supported");

    exception_handler.Register(mem, code_cache_size);
    exception_handler.SetFastmemCallback([this](u64 host_pc) {
        return FastmemCallback(host_pc);
    });
}

AddressSpace::~AddressSpace() = default;

CodePtr AddressSpace::Get(IR::LocationDescriptor descriptor) {
    if (const auto iter = block_entries.find(descriptor); iter != block_entries.end()) {
        return iter->second;
    }
    return nullptr;
}

std::optional<IR::LocationDescriptor> AddressSpace::ReverseGetLocation(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->second;
    }
    return std::nullopt;
}

CodePtr AddressSpace::ReverseGetEntryPoint(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->first;
    }
    return nullptr;
}

CodePtr AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
        return block_entry;
    }
    CompileFpEnvironment fp_environment;
    const auto [guest_pc, a32] = DescribeLocation(descriptor);
    CompileTimingScope timing{guest_pc, a32, descriptor.Value()};
    SetCompilePhase(CompilePhase::Decode);
    GenerateIR(ir_block, descriptor);
    const EmittedBlockInfo block_info = Emit(std::move(ir_block));
    SetCompilePhase(CompilePhase::Finalize);
    timing.Complete();
    return block_info.entry_point;
}

void AddressSpace::InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& descriptors) {
    UnprotectCodeMemory();

    for (const auto& descriptor : descriptors) {
        const auto iter = block_entries.find(descriptor);
        if (iter == block_entries.end()) {
            continue;
        }

        // Unlink before removal because InvalidateBasicBlocks can be called within a fastmem callback,
        // and the currently executing block may have references to itself which need to be unlinked.
        RelinkForDescriptor(descriptor, nullptr);

        block_entries.erase(iter);
    }

    ProtectCodeMemory();
}

void AddressSpace::ClearCache() {
    block_entries.clear();
    reverse_block_entries.clear();
    block_infos.clear();
    block_references.clear();
    code.set_offset(prelude_info.end_of_prelude);
    if (const auto cb = code_registration.load(std::memory_order_relaxed)) {
        cb(nullptr, nullptr, "cache_clear");
    }
}

std::size_t AddressSpace::GetRemainingSize() {
    return code_cache_size - static_cast<std::size_t>(code.offset());
}

EmittedBlockInfo AddressSpace::Emit(IR::Block block) {
    if (GetRemainingSize() < 1024 * 1024) {
        ClearCache();
    }

    UnprotectCodeMemory();

    SetCompilePhase(CompilePhase::Emit);
    EmittedBlockInfo block_info = EmitArm64(code, std::move(block), GetEmitConfig(), fastmem_manager);

    SetCompilePhase(CompilePhase::CacheInsert);
    ASSERT(block_entries.insert({block.Location(), block_info.entry_point}).second);
    ASSERT(reverse_block_entries.insert({block_info.entry_point, block.Location()}).second);
    ASSERT(block_infos.insert({block_info.entry_point, block_info}).second);

    SetCompilePhase(CompilePhase::LinkPatch);
    Link(block_info);
    RelinkForDescriptor(block.Location(), block_info.entry_point);

    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
    ProtectCodeMemory();

    SetCompilePhase(CompilePhase::Registration);
    RegisterNewBasicBlock(block, block_info);

    return block_info;
}

void AddressSpace::Link(EmittedBlockInfo& block_info) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, target] : block_info.relocations) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(block_info.entry_point + ptr_offset));

        switch (target) {
        case LinkTarget::ReturnToDispatcher:
            c.B(prelude_info.return_to_dispatcher);
            break;
        case LinkTarget::ReturnFromRunCode:
            c.B(prelude_info.return_from_run_code);
            break;
        // { this, vaddr, size }
        case LinkTarget::ReadMemory8:
            c.BL(prelude_info.read_memory_8);
            break;
        case LinkTarget::ReadMemory16:
            c.BL(prelude_info.read_memory_16);
            break;
        case LinkTarget::ReadMemory32:
            c.BL(prelude_info.read_memory_32);
            break;
        case LinkTarget::ReadMemory64:
            c.BL(prelude_info.read_memory_64);
            break;
        case LinkTarget::ReadMemory128:
            c.BL(prelude_info.read_memory_128);
            break;
        // { this, vaddr, size }
        case LinkTarget::WrappedReadMemory8:
            c.BL(prelude_info.wrapped_read_memory_8);
            break;
        case LinkTarget::WrappedReadMemory16:
            c.BL(prelude_info.wrapped_read_memory_16);
            break;
        case LinkTarget::WrappedReadMemory32:
            c.BL(prelude_info.wrapped_read_memory_32);
            break;
        case LinkTarget::WrappedReadMemory64:
            c.BL(prelude_info.wrapped_read_memory_64);
            break;
        case LinkTarget::WrappedReadMemory128:
            c.BL(prelude_info.wrapped_read_memory_128);
            break;
        //
        case LinkTarget::ExclusiveReadMemory8:
            c.BL(prelude_info.exclusive_read_memory_8);
            break;
        case LinkTarget::ExclusiveReadMemory16:
            c.BL(prelude_info.exclusive_read_memory_16);
            break;
        case LinkTarget::ExclusiveReadMemory32:
            c.BL(prelude_info.exclusive_read_memory_32);
            break;
        case LinkTarget::ExclusiveReadMemory64:
            c.BL(prelude_info.exclusive_read_memory_64);
            break;
        case LinkTarget::ExclusiveReadMemory128:
            c.BL(prelude_info.exclusive_read_memory_128);
            break;
        // { this, vaddr, value, size }
        case LinkTarget::WriteMemory8:
            c.BL(prelude_info.write_memory_8);
            break;
        case LinkTarget::WriteMemory16:
            c.BL(prelude_info.write_memory_16);
            break;
        case LinkTarget::WriteMemory32:
            c.BL(prelude_info.write_memory_32);
            break;
        case LinkTarget::WriteMemory64:
            c.BL(prelude_info.write_memory_64);
            break;
        case LinkTarget::WriteMemory128:
            c.BL(prelude_info.write_memory_128);
            break;
        //
        case LinkTarget::WrappedWriteMemory8:
            c.BL(prelude_info.wrapped_write_memory_8);
            break;
        case LinkTarget::WrappedWriteMemory16:
            c.BL(prelude_info.wrapped_write_memory_16);
            break;
        case LinkTarget::WrappedWriteMemory32:
            c.BL(prelude_info.wrapped_write_memory_32);
            break;
        case LinkTarget::WrappedWriteMemory64:
            c.BL(prelude_info.wrapped_write_memory_64);
            break;
        case LinkTarget::WrappedWriteMemory128:
            c.BL(prelude_info.wrapped_write_memory_128);
            break;
        //
        case LinkTarget::ExclusiveWriteMemory8:
            c.BL(prelude_info.exclusive_write_memory_8);
            break;
        case LinkTarget::ExclusiveWriteMemory16:
            c.BL(prelude_info.exclusive_write_memory_16);
            break;
        case LinkTarget::ExclusiveWriteMemory32:
            c.BL(prelude_info.exclusive_write_memory_32);
            break;
        case LinkTarget::ExclusiveWriteMemory64:
            c.BL(prelude_info.exclusive_write_memory_64);
            break;
        case LinkTarget::ExclusiveWriteMemory128:
            c.BL(prelude_info.exclusive_write_memory_128);
            break;
        case LinkTarget::WrappedCompareWriteMemory8:
            c.BL(prelude_info.wrapped_compare_write_memory_8);
            break;
        case LinkTarget::WrappedCompareWriteMemory16:
            c.BL(prelude_info.wrapped_compare_write_memory_16);
            break;
        case LinkTarget::WrappedCompareWriteMemory32:
            c.BL(prelude_info.wrapped_compare_write_memory_32);
            break;
        case LinkTarget::WrappedCompareWriteMemory64:
            c.BL(prelude_info.wrapped_compare_write_memory_64);
            break;
        case LinkTarget::CompareWriteMemory128:
            c.BL(prelude_info.compare_write_memory_128);
            break;
        // { this, vaddr, value, AtomicOp or desired }
        case LinkTarget::WrappedAtomicMemory8:
            c.BL(prelude_info.wrapped_atomic_memory_8);
            break;
        case LinkTarget::WrappedAtomicMemory16:
            c.BL(prelude_info.wrapped_atomic_memory_16);
            break;
        case LinkTarget::WrappedAtomicMemory32:
            c.BL(prelude_info.wrapped_atomic_memory_32);
            break;
        case LinkTarget::WrappedAtomicMemory64:
            c.BL(prelude_info.wrapped_atomic_memory_64);
            break;
        case LinkTarget::WrappedCompareAndSwapMemory8:
            c.BL(prelude_info.wrapped_compare_and_swap_memory_8);
            break;
        case LinkTarget::WrappedCompareAndSwapMemory16:
            c.BL(prelude_info.wrapped_compare_and_swap_memory_16);
            break;
        case LinkTarget::WrappedCompareAndSwapMemory32:
            c.BL(prelude_info.wrapped_compare_and_swap_memory_32);
            break;
        case LinkTarget::WrappedCompareAndSwapMemory64:
            c.BL(prelude_info.wrapped_compare_and_swap_memory_64);
            break;
        case LinkTarget::CompareAndSwapMemory128:
            c.BL(prelude_info.compare_and_swap_memory_128);
            break;
        case LinkTarget::CallSVC:
            c.BL(prelude_info.call_svc);
            break;
        case LinkTarget::CallSVCFast:
            c.BL(prelude_info.call_svc_fast);
            break;
        case LinkTarget::CallSVCFastHostFp:
            c.BL(prelude_info.call_svc_fast_host_fp);
            break;
        case LinkTarget::CallSVCFastKnown:
            c.BL(prelude_info.call_svc_fast_known);
            break;
        case LinkTarget::CallSVCFastKnownHostFp:
            c.BL(prelude_info.call_svc_fast_known_host_fp);
            break;
        case LinkTarget::ExceptionRaised:
            c.BL(prelude_info.exception_raised);
            break;
        case LinkTarget::InstructionSynchronizationBarrierRaised:
            c.BL(prelude_info.isb_raised);
            break;
        case LinkTarget::InstructionCacheOperationRaised:
            c.BL(prelude_info.ic_raised);
            break;
        case LinkTarget::DataCacheOperationRaised:
            c.BL(prelude_info.dc_raised);
            break;
        case LinkTarget::GetCNTPCT:
            c.BL(prelude_info.get_cntpct);
            break;
        case LinkTarget::AddTicks:
            c.BL(prelude_info.add_ticks);
            break;
        case LinkTarget::GetTicksRemaining:
            c.BL(prelude_info.get_ticks_remaining);
            break;
        case LinkTarget::DiagnosticEntry:
            c.BL(prelude_info.diagnostic_entry);
            break;
        default:
            UNREACHABLE();
        }
    }

    for (auto [target_descriptor, list] : block_info.block_relocations) {
        block_references[target_descriptor].insert(block_info.entry_point);
        LinkBlockLinks(block_info.entry_point, Get(target_descriptor), list);
    }
}

void AddressSpace::LinkBlockLinks(const CodePtr entry_point, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, type] : block_relocations_list) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(entry_point + ptr_offset));

        switch (type) {
        case BlockRelocationType::Branch:
            if (target_ptr) {
                c.B((void*)target_ptr);
            } else {
                c.NOP();
            }
            break;
        case BlockRelocationType::MoveToScratch1:
            if (target_ptr) {
                c.ADRL(Xscratch1, (void*)target_ptr);
            } else {
                c.ADRL(Xscratch1, prelude_info.return_to_dispatcher);
            }
            break;
        default:
            UNREACHABLE();
        }
    }
}

void AddressSpace::RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    for (auto code_ptr : block_references[target_descriptor]) {
        if (auto block_iter = block_infos.find(code_ptr); block_iter != block_infos.end()) {
            const EmittedBlockInfo& block_info = block_iter->second;

            if (auto relocation_iter = block_info.block_relocations.find(target_descriptor); relocation_iter != block_info.block_relocations.end()) {
                LinkBlockLinks(block_info.entry_point, target_ptr, relocation_iter->second);
            }

            mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
        }
    }
}

FakeCall AddressSpace::FastmemCallback(u64 host_pc) {
    {
        const auto host_ptr = std::bit_cast<CodePtr>(host_pc);

        const auto entry_point = ReverseGetEntryPoint(host_ptr);
        if (!entry_point) {
            goto fail;
        }

        const auto block_info = block_infos.find(entry_point);
        if (block_info == block_infos.end()) {
            goto fail;
        }

        const auto patch_entry = block_info->second.fastmem_patch_info.find(host_ptr - entry_point);
        if (patch_entry == block_info->second.fastmem_patch_info.end()) {
            goto fail;
        }

        const auto fc = patch_entry->second.fc;

        if (patch_entry->second.recompile) {
            const auto marker = patch_entry->second.marker;
            fastmem_manager.MarkDoNotFastmem(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
        }

        return fc;
    }

fail:
    fmt::print("dynarmic: Segfault happened within JITted code at host_pc = {:016x}\n"
        "Segfault wasn't at a fastmem patch location!\n", host_pc);
    UNREACHABLE();
}

void* AddressSpace::EmitCallTrampoline(u64 this_ptr, u64 fn_ptr, RegisterList save, CallFp fp, const std::function<void()>& args, const std::function<void()>& result) {
    using namespace oaknut::util;

    oaknut::Label l_this, l_addr;
    const std::size_t fpsr_offset = GuestFpsrOffset();
    const auto merge_fpsr = [&] {
        code.MRS(Xscratch0, oaknut::SystemReg::FPSR);
        code.LDR(Wscratch1, Xstate, fpsr_offset);
        code.ORR(Wscratch1, Wscratch1, Wscratch0);
        code.STR(Wscratch1, Xstate, fpsr_offset);
    };

    // JIT code calls with SP at its StackLayout.
    save |= ToRegList(X29) | ToRegList(X30);
    const std::size_t stack_layout_offset = ABI_PushedSize(save);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save, 0);
    if (args) {
        args();
    }
    if (fp == CallFp::Host) {
        merge_fpsr();
        code.MSR(oaknut::SystemReg::FPSR, XZR);
        code.LDR(Wscratch0, SP, stack_layout_offset + offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    }
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    if (fp == CallFp::Host) {
        // The callback may have changed the guest FPCR (SetFpcr/SetFpscr).
        EmitLoadGuestFpcr(Wscratch0);
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    } else {
        merge_fpsr();
    }
    code.MSR(oaknut::SystemReg::FPSR, XZR);
    if (result) {
        result();
    }
    ABI_PopRegisters(code, save, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(this_ptr);
    code.l(l_addr);
    code.dx(fn_ptr);

    return target;
}

void AddressSpace::EmitEnterGuestFpFromRun() {
    using namespace oaknut::util;

    code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
    code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
    code.MRS(Xscratch1, oaknut::SystemReg::FPSR);
    code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpsr));
    EmitLoadGuestFpcr(Wscratch0);
    code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    code.MSR(oaknut::SystemReg::FPSR, XZR);
}

void AddressSpace::EmitLeaveGuestFpForRun() {
    using namespace oaknut::util;

    code.MRS(Xscratch0, oaknut::SystemReg::FPSR);
    code.LDR(Wscratch1, Xstate, GuestFpsrOffset());
    code.ORR(Wscratch1, Wscratch1, Wscratch0);
    code.STR(Wscratch1, Xstate, GuestFpsrOffset());
    code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpcr));
    code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpsr));
    code.MSR(oaknut::SystemReg::FPSR, Xscratch0);
}

}  // namespace Dynarmic::Backend::Arm64
