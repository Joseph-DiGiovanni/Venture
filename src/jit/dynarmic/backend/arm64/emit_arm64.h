// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "common/container/unordered_map.h"

#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/location_descriptor.h"

namespace oaknut {
struct CodeGenerator;
struct Label;
}  // namespace oaknut

namespace Dynarmic {
class ExclusiveMonitor;
}  // namespace Dynarmic

namespace Dynarmic::FP {
class FPCR;
}  // namespace Dynarmic::FP

namespace Dynarmic::IR {
class Block;
class Inst;
enum class Cond;
enum class Opcode;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Arm64 {

struct EmitContext;

using CodePtr = std::byte*;

enum class LinkTarget {
    ReturnToDispatcher,
    ReturnFromRunCode,
    ReadMemory8,
    ReadMemory16,
    ReadMemory32,
    ReadMemory64,
    ReadMemory128,
    WrappedReadMemory8,
    WrappedReadMemory16,
    WrappedReadMemory32,
    WrappedReadMemory64,
    WrappedReadMemory128,
    ExclusiveReadMemory8,
    ExclusiveReadMemory16,
    ExclusiveReadMemory32,
    ExclusiveReadMemory64,
    ExclusiveReadMemory128,
    WriteMemory8,
    WriteMemory16,
    WriteMemory32,
    WriteMemory64,
    WriteMemory128,
    WrappedWriteMemory8,
    WrappedWriteMemory16,
    WrappedWriteMemory32,
    WrappedWriteMemory64,
    WrappedWriteMemory128,
    ExclusiveWriteMemory8,
    ExclusiveWriteMemory16,
    ExclusiveWriteMemory32,
    ExclusiveWriteMemory64,
    ExclusiveWriteMemory128,
    // MemoryWriteExclusive* outside the global monitor (inline exclusives): vaddr in
    // Xscratch0, value in Xscratch1, expected in D0; Xscratch0 = 1 on success. The 128-bit
    // form is plain: vaddr in X1, value in Q0, expected in Q1; W0 = 1 on success.
    WrappedCompareWriteMemory8,
    WrappedCompareWriteMemory16,
    WrappedCompareWriteMemory32,
    WrappedCompareWriteMemory64,
    CompareWriteMemory128,
    // { this, vaddr }; vaddr in Xscratch0, value or expected in Xscratch1, AtomicOp or
    // desired in D0. Caller-saved registers other than Xscratch0 (the result) are kept.
    WrappedAtomicMemory8,
    WrappedAtomicMemory16,
    WrappedAtomicMemory32,
    WrappedAtomicMemory64,
    WrappedCompareAndSwapMemory8,
    WrappedCompareAndSwapMemory16,
    WrappedCompareAndSwapMemory32,
    WrappedCompareAndSwapMemory64,
    // { this, vaddr in X1, expected in Q0, desired in Q1 } -> Q0
    CompareAndSwapMemory128,
    // Plain CallSVC and the fast forms with the host FP environment.
    CallSVC,
    CallSVCFastHostFp,
    CallSVCFastKnownHostFp,
    // Fast forms that keep the guest FP environment live.
    CallSVCFast,
    CallSVCFastKnown,
    ExceptionRaised,
    InstructionSynchronizationBarrierRaised,
    InstructionCacheOperationRaised,
    DataCacheOperationRaised,
    GetCNTPCT,
    AddTicks,
    GetTicksRemaining,
    // A64 UserConfig::diagnostic_entry.record: { user, pc in X1, state in X2 }, host FP environment.
    DiagnosticEntry,
};

struct Relocation {
    std::ptrdiff_t code_offset;
    LinkTarget target;
};

enum class BlockRelocationType {
    Branch,
    MoveToScratch1,
};

struct BlockRelocation {
    std::ptrdiff_t code_offset;
    BlockRelocationType type;
};

struct EmittedBlockInfo {
    CodePtr entry_point;
    std::size_t size;
    std::vector<Relocation> relocations;
    ::Common::unordered_map<IR::LocationDescriptor, std::vector<BlockRelocation>> block_relocations;
    ::Common::unordered_map<std::ptrdiff_t, FastmemPatchInfo> fastmem_patch_info;
};

struct EmitConfig {
    OptimizationFlag optimizations;
    bool HasOptimization(OptimizationFlag f) const { return (f & optimizations) != no_optimizations; }

    bool hook_isb;

    // System registers
    u64 cntfreq_el0;
    u32 ctr_el0;
    u32 dczid_el0;
    const u64* tpidrro_el0;
    u64* tpidr_el0;

    // Memory
    bool check_halt_on_memory_access;

    // Page table
    u64 page_table_pointer;
    std::size_t page_table_address_space_bits;
    u64 page_table_pointer_mask;
    std::size_t page_table_log2_stride;
    std::optional<std::uint8_t> page_table_marked_bit;
    std::optional<std::uint8_t> page_table_sign_extension;
    bool silently_mirror_page_table;
    bool absolute_offset_page_table;
    u8 detect_misaligned_access_via_page_table;
    bool only_detect_misalignment_via_page_table_on_page_boundary;

    // Fastmem
    std::optional<u64> fastmem_pointer;
    bool recompile_on_fastmem_failure;
    std::size_t fastmem_address_space_bits;
    bool silently_mirror_fastmem;

    // Exclusives: UserConfig::fastmem_exclusive_access and related options
    ExclusiveMonitor* global_monitor;
    std::size_t processor_id;
    bool fastmem_exclusive_access;
    bool elide_unchanged_exclusive_stores;
    bool recompile_on_exclusive_fastmem_failure;

    // Timing
    bool wall_clock_cntpct;
    bool enable_cycle_counting;
    // A64: forward links within a halt-polling window may skip the halt poll.
    bool enable_bounded_halt_polling;

    // Supervisor calls (A64/A32::UserConfig::svc_*).
    std::optional<u32> svc_fast;
    std::optional<u32> svc_fp_transfer;
    std::optional<u32> svc_fp_guest;
    std::function<const void*(u64, u32)> svc_lookup;
    // A64: guest PC of a block whose entry calls LinkTarget::DiagnosticEntry, if selected.
    std::function<std::optional<u64>(const IR::LocationDescriptor&)> diagnostic_entry;

    // Endianness
    bool always_little_endian;

    // Frontend specific callbacks
    FP::FPCR (*descriptor_to_fpcr)(const IR::LocationDescriptor& descriptor);
    oaknut::Label (*emit_cond)(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Cond cond);
    void (*emit_condition_failed_terminal)(oaknut::CodeGenerator& code, EmitContext& ctx);
    void (*emit_terminal)(oaknut::CodeGenerator& code, EmitContext& ctx);
    void (*emit_check_memory_abort)(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end);

    // State offsets
    std::size_t state_nzcv_offset;
    std::size_t state_fpsr_offset;
    std::size_t state_exclusive_state_offset;

    // A32 specific
    std::array<std::shared_ptr<A32::Coprocessor>, 16> coprocessors{};

    // Debugging
    bool very_verbose_debugging_output;
};

EmittedBlockInfo EmitArm64(oaknut::CodeGenerator& code, IR::Block block, const EmitConfig& emit_conf, FastmemManager& fastmem_manager);

template<IR::Opcode op>
void EmitIR(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
void EmitRelocation(oaknut::CodeGenerator& code, EmitContext& ctx, LinkTarget link_target);
void EmitBlockLinkRelocation(oaknut::CodeGenerator& code, EmitContext& ctx, const IR::LocationDescriptor& descriptor, BlockRelocationType type);
oaknut::Label EmitA32Cond(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Cond cond);
oaknut::Label EmitA64Cond(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Cond cond);
void EmitA32Terminal(oaknut::CodeGenerator& code, EmitContext& ctx);
void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx);
void EmitA32ConditionFailedTerminal(oaknut::CodeGenerator& code, EmitContext& ctx);
void EmitA64ConditionFailedTerminal(oaknut::CodeGenerator& code, EmitContext& ctx);
void EmitA32CheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end);
void EmitA64CheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end);

}  // namespace Dynarmic::Backend::Arm64
