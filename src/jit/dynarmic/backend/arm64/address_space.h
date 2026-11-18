// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <utility>

#include "common/common_types.h"
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

/// Code cache memory. oaknut::CodeBlock reports an mmap failure (MAP_FAILED) as a
/// valid pointer; this checks it and reports it through `allocation_failed`.
class CodeMemory final : public oaknut::CodeBlock {
public:
    CodeMemory(std::size_t size, const std::function<void(std::size_t, std::size_t, std::size_t)>& allocation_failed);
};

class AddressSpace {
public:
    AddressSpace(std::size_t code_cache_size, const std::function<void(std::size_t, std::size_t, std::size_t)>& code_allocation_failed);
    virtual ~AddressSpace();

    virtual void GenerateIR(IR::Block& ir_block, IR::LocationDescriptor) const = 0;

    CodePtr Get(IR::LocationDescriptor descriptor);

    // Returns "most likely" LocationDescriptor assocated with the emitted code at that location
    std::optional<IR::LocationDescriptor> ReverseGetLocation(CodePtr host_pc);

    // Returns "most likely" entry_point associated with the emitted code at that location
    CodePtr ReverseGetEntryPoint(CodePtr host_pc);

    CodePtr GetOrEmit(IR::LocationDescriptor descriptor);

    void InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& descriptors);

    void ClearCache();
protected:
    virtual EmitConfig GetEmitConfig() = 0;
    virtual void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) = 0;

    void ProtectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.protect();
#endif
    }

    void UnprotectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.unprotect();
#endif
    }

    std::size_t GetRemainingSize();
    EmittedBlockInfo Emit(IR::Block ir_block);
    void Link(EmittedBlockInfo& block);
    void LinkBlockLinks(const CodePtr entry_point, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list);
    void RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr);

    FakeCall FastmemCallback(u64 host_pc);

    /// Guest PC of a descriptor and whether it is an A32 location (compile timing).
    virtual std::pair<u64, bool> DescribeLocation(IR::LocationDescriptor descriptor) const = 0;
    /// Loads the guest FPCR value from the JitState (Xstate) into `dest`.
    virtual void EmitLoadGuestFpcr(oaknut::WReg dest) = 0;
    virtual std::size_t GuestFpsrOffset() const = 0;

    enum class CallFp {
        Host,   // host FPCR and FPSR=0 during the call; guest flags merged first
        Guest,  // guest FPCR and FPSR stay live; flags the callee raises are merged after
    };
    /// A trampoline calling fn(this_ptr, ...) from JIT code. `save` is pushed (X29/X30 always),
    /// `args` runs after the push with the inputs intact, `result` after the call with
    /// the guest FP environment reinstalled. Only Xscratch0/1 are used besides those.
    void* EmitCallTrampoline(u64 this_ptr, u64 fn_ptr, RegisterList save, CallFp fp,
                             const std::function<void()>& args = {}, const std::function<void()>& result = {});
    /// Run entry: saves the host FPCR/FPSR in the StackLayout at SP, installs the guest
    /// FPCR and FPSR=0. Run exit: merges FPSR into the guest FPSR and restores the host's.
    void EmitEnterGuestFpFromRun();
    void EmitLeaveGuestFpForRun();

    IR::Block ir_block;
    const std::size_t code_cache_size;
    CodeMemory mem;
    oaknut::CodeGenerator code;

    // A IR::LocationDescriptor will have one current CodePtr.
    // However, there can be multiple other CodePtrs which are older, previously invalidated blocks.
    std::map<CodePtr, IR::LocationDescriptor> reverse_block_entries;
    ::Common::unordered_map<IR::LocationDescriptor, CodePtr> block_entries;
    ::Common::unordered_map<CodePtr, EmittedBlockInfo> block_infos;
    ::Common::unordered_map<IR::LocationDescriptor, ::Common::unordered_set<CodePtr>> block_references;

    ExceptionHandler exception_handler;
    FastmemManager fastmem_manager;

    struct PreludeInfo {
        std::ptrdiff_t end_of_prelude;

        using RunCodeFuncType = HaltReason (*)(CodePtr entry_point, void* jit_state, volatile u32* halt_reason);
        RunCodeFuncType run_code;
        RunCodeFuncType step_code;
        void* return_to_dispatcher;
        void* return_from_run_code;

        void* read_memory_8;
        void* read_memory_16;
        void* read_memory_32;
        void* read_memory_64;
        void* read_memory_128;
        void* wrapped_read_memory_8;
        void* wrapped_read_memory_16;
        void* wrapped_read_memory_32;
        void* wrapped_read_memory_64;
        void* wrapped_read_memory_128;
        void* exclusive_read_memory_8;
        void* exclusive_read_memory_16;
        void* exclusive_read_memory_32;
        void* exclusive_read_memory_64;
        void* exclusive_read_memory_128;
        void* write_memory_8;
        void* write_memory_16;
        void* write_memory_32;
        void* write_memory_64;
        void* write_memory_128;
        void* wrapped_write_memory_8;
        void* wrapped_write_memory_16;
        void* wrapped_write_memory_32;
        void* wrapped_write_memory_64;
        void* wrapped_write_memory_128;
        void* exclusive_write_memory_8;
        void* exclusive_write_memory_16;
        void* exclusive_write_memory_32;
        void* exclusive_write_memory_64;
        void* exclusive_write_memory_128;
        void* wrapped_compare_write_memory_8;
        void* wrapped_compare_write_memory_16;
        void* wrapped_compare_write_memory_32;
        void* wrapped_compare_write_memory_64;
        void* compare_write_memory_128;

        void* wrapped_atomic_memory_8;
        void* wrapped_atomic_memory_16;
        void* wrapped_atomic_memory_32;
        void* wrapped_atomic_memory_64;
        void* wrapped_compare_and_swap_memory_8;
        void* wrapped_compare_and_swap_memory_16;
        void* wrapped_compare_and_swap_memory_32;
        void* wrapped_compare_and_swap_memory_64;
        void* compare_and_swap_memory_128;

        void* call_svc;
        void* call_svc_fast;
        void* call_svc_fast_host_fp;
        void* call_svc_fast_known;
        void* call_svc_fast_known_host_fp;
        void* exception_raised;
        void* dc_raised;
        void* ic_raised;
        void* isb_raised;

        void* get_cntpct;
        void* add_ticks;
        void* get_ticks_remaining;
        void* diagnostic_entry;
    } prelude_info{};
};

}  // namespace Dynarmic::Backend::Arm64
