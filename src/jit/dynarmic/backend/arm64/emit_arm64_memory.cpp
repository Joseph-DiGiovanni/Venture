// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/emit_arm64_memory.h"

#include <optional>
#include <utility>
#include <cstddef>

#include <bit>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/common/spin_lock_arm64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

namespace {

bool IsOrdered(IR::AccType acctype) {
    return acctype == IR::AccType::ORDERED || acctype == IR::AccType::ORDEREDRW || acctype == IR::AccType::LIMITEDORDERED;
}

LinkTarget ReadMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ReadMemory8;
    case 16:
        return LinkTarget::ReadMemory16;
    case 32:
        return LinkTarget::ReadMemory32;
    case 64:
        return LinkTarget::ReadMemory64;
    case 128:
        return LinkTarget::ReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WriteMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WriteMemory8;
    case 16:
        return LinkTarget::WriteMemory16;
    case 32:
        return LinkTarget::WriteMemory32;
    case 64:
        return LinkTarget::WriteMemory64;
    case 128:
        return LinkTarget::WriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedReadMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedReadMemory8;
    case 16:
        return LinkTarget::WrappedReadMemory16;
    case 32:
        return LinkTarget::WrappedReadMemory32;
    case 64:
        return LinkTarget::WrappedReadMemory64;
    case 128:
        return LinkTarget::WrappedReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedWriteMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedWriteMemory8;
    case 16:
        return LinkTarget::WrappedWriteMemory16;
    case 32:
        return LinkTarget::WrappedWriteMemory32;
    case 64:
        return LinkTarget::WrappedWriteMemory64;
    case 128:
        return LinkTarget::WrappedWriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveReadMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveReadMemory8;
    case 16:
        return LinkTarget::ExclusiveReadMemory16;
    case 32:
        return LinkTarget::ExclusiveReadMemory32;
    case 64:
        return LinkTarget::ExclusiveReadMemory64;
    case 128:
        return LinkTarget::ExclusiveReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveWriteMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveWriteMemory8;
    case 16:
        return LinkTarget::ExclusiveWriteMemory16;
    case 32:
        return LinkTarget::ExclusiveWriteMemory32;
    case 64:
        return LinkTarget::ExclusiveWriteMemory64;
    case 128:
        return LinkTarget::ExclusiveWriteMemory128;
    }
    UNREACHABLE();
}

// For an access emitted inline: its A64ColdSet* records follow it, so the abort exit is
// deferred until they are known.
void EmitInlineCheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.check_halt_on_memory_access) {
        return;
    }
    SharedLabel abort = GenSharedLabel(), resume = GenSharedLabel();
    code.LDAR(Wscratch0, Xhalt);
    code.TST(Wscratch0, static_cast<u32>(HaltReason::MemoryAbort));
    code.B(NE, *abort);
    code.l(*resume);
    ctx.deferred_emits.emplace_back([&code, &ctx, inst, abort, resume] {
        code.l(*abort);
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *resume);
    });
}

template<std::size_t bitsize>
void CallbackOnlyEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    EmitRelocation(code, ctx, ReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
    EmitInlineCheckMemoryAbort(code, ctx, inst);
}

template<std::size_t bitsize>
void CallbackOnlyEmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    code.MOV(Wscratch0, 1);
    code.STRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
    EmitInlineCheckMemoryAbort(code, ctx, inst);
}

template<std::size_t bitsize>
void CallbackOnlyEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    EmitRelocation(code, ctx, WriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    EmitInlineCheckMemoryAbort(code, ctx, inst);
}

template<std::size_t bitsize>
void CallbackOnlyEmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    oaknut::Label end;

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    code.MOV(W0, 1);
    code.LDRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CBZ(Wscratch0, end);
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveWriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    code.l(end);
    ctx.reg_alloc.DefineAsRegister(inst, X0);
    EmitInlineCheckMemoryAbort(code, ctx, inst);
}

constexpr std::size_t page_table_const_bits = 12;
constexpr std::size_t page_table_const_size = 1 << page_table_const_bits;
constexpr std::size_t page_table_const_mask = (1 << page_table_const_bits) - 1;

// This function may use Xscratch0 as a scratch register
// Trashes NZCV
template<std::size_t bitsize>
void EmitDetectMisalignedVAddr(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    static_assert(bitsize == 8 || bitsize == 16 || bitsize == 32 || bitsize == 64 || bitsize == 128);

    if (bitsize == 8 || (ctx.conf.detect_misaligned_access_via_page_table & bitsize) == 0) {
        return;
    }

    if (!ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        const u64 align_mask = []() -> u64 {
            switch (bitsize) {
            case 16:
                return 0b1;
            case 32:
                return 0b11;
            case 64:
                return 0b111;
            case 128:
                return 0b1111;
            default:
                UNREACHABLE();
            }
        }();

        code.TST(Xaddr, align_mask);
        code.B(NE, *fallback);
    } else {
        // If (addr & page_table_const_mask) > page_table_const_size - byte_size, use fallback.
        code.AND(Xscratch0, Xaddr, page_table_const_mask);
        code.CMP(Xscratch0, page_table_const_size - bitsize / 8);
        code.B(HI, *fallback);
    }
}

// Outputs Xscratch0 = page_table[addr >> page_table_const_bits]
// May use Xscratch1 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xscratch0 and ret1 is either Xaddr or Xscratch1
// Trashes NZCV
template<std::size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> InlinePageTableEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    const std::size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_table_const_bits;
    const std::size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;

    EmitDetectMisalignedVAddr<bitsize>(code, ctx, Xaddr, fallback);

    if (ctx.conf.silently_mirror_page_table || unused_top_bits == 0) {
        code.UBFX(Xscratch0, Xaddr, page_table_const_bits, valid_page_index_bits);
    } else {
        code.LSR(Xscratch0, Xaddr, page_table_const_bits);
        code.TST(Xscratch0, u64(~u64(0)) << valid_page_index_bits);
        code.B(NE, *fallback);
    }

    // index = index << log2
    code.LSL(Xscratch0, Xscratch0, ctx.conf.page_table_log2_stride);
    // load x0 = *<(u8*)pagetable + index>
    code.LDR(Xscratch0, Xpagetable, Xscratch0);

    if (ctx.conf.page_table_marked_bit) {
        code.TST(Xscratch0, 1ULL << *ctx.conf.page_table_marked_bit);
        code.B(NE, *fallback);
    }

    if (ctx.conf.page_table_pointer_mask != 0) {
        code.AND(Xscratch0, Xscratch0, ctx.conf.page_table_pointer_mask);
    }

    // TODO: combine this with page_table_pointer_mask
    if (ctx.conf.page_table_sign_extension) {
        code.SBFM(Xscratch0, Xscratch0, 0, *ctx.conf.page_table_sign_extension);
    }

    code.CBZ(Xscratch0, *fallback);

    if (ctx.conf.absolute_offset_page_table) {
        return std::make_pair(Xscratch0, Xaddr);
    }
    code.AND(Xscratch1, Xaddr, page_table_const_mask);
    return std::make_pair(Xscratch0, Xscratch1);
}

template<std::size_t bitsize>
CodePtr EmitMemoryLdr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location = code.xptr<CodePtr>();

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDARB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.LDARH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.LDAR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.LDAR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            UNREACHABLE();
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.LDRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.LDR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.LDR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            UNREACHABLE();
        }
    }

    return fastmem_location;
}

template<std::size_t bitsize>
CodePtr EmitMemoryStr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location;

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STLRB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.STLRH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.STLR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.STLR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            // The patch location is the access that can fault.
            code.DMB(oaknut::BarrierOp::ISH);
            fastmem_location = code.xptr<CodePtr>();
            code.STR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            UNREACHABLE();
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.STRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.STR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.STR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.STR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            UNREACHABLE();
        }
    }

    return fastmem_location;
}

template<std::size_t bitsize>
void InlinePageTableEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end] {
        code.l(*fallback);
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<std::size_t bitsize>
void InlinePageTableEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end] {
        code.l(*fallback);
        if constexpr (bitsize == 128) {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

std::optional<DoNotFastmemMarker> ShouldFastmem(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.fastmem_pointer || !ctx.fastmem.SupportsFastmem()) {
        return std::nullopt;
    }

    const auto marker = std::make_tuple(ctx.block.Location(), inst->GetName());
    if (ctx.fastmem.ShouldFastmem(marker)) {
        return marker;
    }
    return std::nullopt;
}

inline bool ShouldExt32(EmitContext& ctx) {
    return ctx.conf.fastmem_address_space_bits == 32 && ctx.conf.silently_mirror_fastmem;
}

// May use Xscratch0 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xfastmem and ret1 is either Xaddr or Xscratch0
// Trashes NZCV
template<std::size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> FastmemEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    if (ctx.conf.fastmem_address_space_bits == 64 || ShouldExt32(ctx)) {
        return std::make_pair(Xfastmem, Xaddr);
    }

    if (ctx.conf.silently_mirror_fastmem) {
        code.UBFX(Xscratch0, Xaddr, 0, ctx.conf.fastmem_address_space_bits);
        return std::make_pair(Xfastmem, Xscratch0);
    }

    code.LSR(Xscratch0, Xaddr, ctx.conf.fastmem_address_space_bits);
    code.CBNZ(Xscratch0, *fallback);
    return std::make_pair(Xfastmem, Xaddr);
}

template<std::size_t bitsize>
void FastmemEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    const auto fastmem_location = EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end, fastmem_location] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<std::size_t bitsize>
void FastmemEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    const auto fastmem_location = EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end, fastmem_location] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        if constexpr (bitsize == 128) {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

LinkTarget WrappedAtomicMemoryLinkTarget(std::size_t bitsize, bool compare_and_swap) {
    switch (bitsize) {
    case 8:
        return compare_and_swap ? LinkTarget::WrappedCompareAndSwapMemory8 : LinkTarget::WrappedAtomicMemory8;
    case 16:
        return compare_and_swap ? LinkTarget::WrappedCompareAndSwapMemory16 : LinkTarget::WrappedAtomicMemory16;
    case 32:
        return compare_and_swap ? LinkTarget::WrappedCompareAndSwapMemory32 : LinkTarget::WrappedAtomicMemory32;
    case 64:
        return compare_and_swap ? LinkTarget::WrappedCompareAndSwapMemory64 : LinkTarget::WrappedAtomicMemory64;
    }
    UNREACHABLE();
}

// Native LSE needs fastmem and, as on x64, no global monitor to keep reservations for.
std::optional<DoNotFastmemMarker> ShouldFastmemAtomic(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return std::nullopt;
    }
    return ShouldFastmem(ctx, inst);
}

// Address to access with an LSE instruction (base register only) into Xscratch0.
template<std::size_t bitsize>
void FastmemEmitAtomicAddress(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    if constexpr (bitsize > 8) {
        // LSE requires natural alignment; the callback reports the fault.
        code.TST(Xaddr, bitsize / 8 - 1);
        code.B(NE, *fallback);
    }
    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    if (ShouldExt32(ctx)) {
        code.ADD(Xscratch0, Xbase, Xoffset.toW(), oaknut::AddSubExt::UXTW);
    } else {
        code.ADD(Xscratch0, Xbase, Xoffset);
    }
}

template<std::size_t bitsize, typename Reg>
void EmitLse(oaknut::CodeGenerator& code, A64::AtomicOp op, Reg Rs, Reg Rt) {
    // Always acquire-release: the IR does not keep LSE's A/R bits, and x64's locked
    // instructions are full barriers too.
#define VENTURE_LSE(NAME)                      \
    if constexpr (bitsize == 8) {              \
        code.NAME##B(Rs, Rt, Xscratch0);       \
    } else if constexpr (bitsize == 16) {      \
        code.NAME##H(Rs, Rt, Xscratch0);       \
    } else {                                   \
        code.NAME(Rs, Rt, Xscratch0);          \
    }                                          \
    break;
    using Op = A64::AtomicOp;
    switch (op) {
    case Op::Add:
        VENTURE_LSE(LDADDAL)
    case Op::Clear:
        VENTURE_LSE(LDCLRAL)
    case Op::Eor:
        VENTURE_LSE(LDEORAL)
    case Op::Set:
        VENTURE_LSE(LDSETAL)
    case Op::SMax:
        VENTURE_LSE(LDSMAXAL)
    case Op::SMin:
        VENTURE_LSE(LDSMINAL)
    case Op::UMax:
        VENTURE_LSE(LDUMAXAL)
    case Op::UMin:
        VENTURE_LSE(LDUMINAL)
    case Op::Swap:
        VENTURE_LSE(SWPAL)
    case Op::CompareAndSwap:
        VENTURE_LSE(CASAL)
    default:
        UNREACHABLE();
    }
#undef VENTURE_LSE
}

// Calls the MemoryAtomic callback through the wrapped trampoline; other registers are kept.
template<std::size_t bitsize, bool compare_and_swap>
void EmitAtomicCallback(oaknut::CodeGenerator& code, EmitContext& ctx, A64::AtomicOp op, oaknut::XReg Xaddr, oaknut::XReg Xvalue, oaknut::XReg Xdesired, oaknut::XReg Xresult) {
    if constexpr (compare_and_swap) {
        code.FMOV(D0, Xdesired);
    } else {
        code.MOV(Xscratch0, static_cast<u64>(op));
        code.FMOV(D0, Xscratch0);
    }
    code.MOV(Xscratch1, Xvalue);
    code.MOV(Xscratch0, Xaddr);
    EmitRelocation(code, ctx, WrappedAtomicMemoryLinkTarget(bitsize, compare_and_swap));
    if constexpr (bitsize == 64) {
        code.MOV(Xresult, Xscratch0);
    } else {
        code.MOV(Xresult.toW(), Wscratch0);
    }
}

template<std::size_t bitsize, bool compare_and_swap>
void EmitAtomicMemoryImpl(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    static_assert(bitsize == 8 || bitsize == 16 || bitsize == 32 || bitsize == 64);
    constexpr std::size_t regsize = bitsize == 64 ? 64 : 32;
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto op = compare_and_swap ? A64::AtomicOp::CompareAndSwap : static_cast<A64::AtomicOp>(args[3].GetImmediateU8());
    const auto marker = ShouldFastmemAtomic(ctx, inst);

    // CAS: the result register starts as the expected value (CASAL's Rs).
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rresult = [&] {
        if constexpr (compare_and_swap) {
            return ctx.reg_alloc.ReadWriteReg<regsize>(args[2], inst);
        } else {
            return ctx.reg_alloc.WriteReg<regsize>(inst);
        }
    }();
    auto Roperand = ctx.reg_alloc.ReadReg<regsize>(args[compare_and_swap ? 3 : 2]);
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rresult, Roperand);

    const oaknut::XReg Xresult{Rresult->index()};
    const oaknut::XReg Xoperand{Roperand->index()};
    // Callback inputs: the value (CAS: expected) and the AtomicOp or desired value.
    const oaknut::XReg Xvalue = compare_and_swap ? Xresult : Xoperand;
    const oaknut::XReg Xdesired = Xoperand;

    // A local exclusive monitor does not survive an atomic access.
    code.STR(WZR, Xstate, ctx.conf.state_exclusive_state_offset);

    if (!marker) {
        EmitAtomicCallback<bitsize, compare_and_swap>(code, ctx, op, *Xaddr, Xvalue, Xdesired, Xresult);
        EmitInlineCheckMemoryAbort(code, ctx, inst);
        return;
    }

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
    FastmemEmitAtomicAddress<bitsize>(code, ctx, *Xaddr, fallback);
    const CodePtr fastmem_location = code.xptr<CodePtr>();
    if constexpr (compare_and_swap) {
        EmitLse<bitsize>(code, op, *Rresult, *Roperand);
    } else {
        EmitLse<bitsize>(code, op, *Roperand, *Rresult);
    }
    code.l(*end);

    // Entered by the branches above, or by the fault handler at the LSE instruction
    // with the registers as they were before it.
    ctx.deferred_emits.emplace_back([&code, &ctx, inst, op, marker = *marker, Xaddr = *Xaddr, Xvalue, Xdesired, Xresult, fallback, end, fastmem_location] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        EmitAtomicCallback<bitsize, compare_and_swap>(code, ctx, op, Xaddr, Xvalue, Xdesired, Xresult);
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });
}

void EmitCompareAndSwapMemory128Impl(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto marker = ShouldFastmemAtomic(ctx, inst);
    // X1 = vaddr, Q0 = expected, Q1 = desired; every caller-saved register is free.
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2], args[3]);

    code.STR(WZR, Xstate, ctx.conf.state_exclusive_state_offset);

    if (marker) {
        SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
        FastmemEmitAtomicAddress<128>(code, ctx, X1, fallback);
        code.FMOV(X2, D0);
        code.FMOV(X3, V0.D()[1]);
        code.FMOV(X4, D1);
        code.FMOV(X5, V1.D()[1]);
        const CodePtr fastmem_location = code.xptr<CodePtr>();
        code.CASPAL(X2, X3, X4, X5, Xscratch0);
        code.FMOV(D0, X2);
        code.FMOV(V0.D()[1], X3);
        code.l(*end);

        ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker = *marker, fallback, end, fastmem_location] {
            ctx.ebi.fastmem_patch_info.emplace(
                fastmem_location - ctx.ebi.entry_point,
                FastmemPatchInfo{
                    .marker = marker,
                    .fc = FakeCall{
                        .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                    },
                    .recompile = ctx.conf.recompile_on_fastmem_failure,
                });

            code.l(*fallback);
            EmitRelocation(code, ctx, LinkTarget::CompareAndSwapMemory128);
            ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
            code.B(*end);
        });
    } else {
        EmitRelocation(code, ctx, LinkTarget::CompareAndSwapMemory128);
    }

    code.MOV(Q8.B16(), Q0.B16());
    ctx.reg_alloc.DefineAsRegister(inst, Q8);
    if (!marker) {
        EmitInlineCheckMemoryAbort(code, ctx, inst);
    }
}

// Inline (fastmem) exclusives keep the global monitor's reservation state as the x64
// backend does (UserConfig::fastmem_exclusive_access), so they interoperate with the
// callback paths of other blocks and Jits.
bool UseInlineExclusives(EmitContext& ctx) {
    return ctx.conf.fastmem_exclusive_access && ctx.conf.fastmem_pointer && ctx.conf.global_monitor && ctx.fastmem.SupportsFastmem();
}

bool IgnoreGlobalMonitor(EmitContext& ctx) {
    return ctx.conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor);
}

// The store-exclusive's exclusive load is earlier in this block and nothing between
// them reads memory: a value compare is a linearizable compare-and-swap, so the
// generation claim is unnecessary (x64 ExclusivePairIsLocal).
bool ExclusivePairIsLocal(const IR::Block& block, const IR::Inst* store) {
    bool local = false;
    for (const IR::Inst& inst : block.Instructions()) {
        if (&inst == store) {
            return local;
        }
        const auto op = inst.GetOpcode();
        if (IR::IsExclusiveMemoryRead(op)) {
            local = true;
        } else if (IR::IsMemoryRead(op) || IR::IsExclusiveMemoryWrite(op)) {
            local = false;
        }
    }
    return false;
}

// The store-exclusive follows an exclusive read in this block with no other memory
// access, clear-exclusive, supervisor call or exception between them.
bool ExclusivePairIsAdjacent(const IR::Block& block, const IR::Inst* store) {
    bool adjacent = false;
    for (const IR::Inst& inst : block.Instructions()) {
        if (&inst == store) {
            return adjacent;
        }
        const auto op = inst.GetOpcode();
        if (IR::IsExclusiveMemoryRead(op)) {
            adjacent = true;
        } else if (IR::IsMemoryReadOrWrite(op)) {
            adjacent = false;
        } else {
            switch (op) {
            case IR::Opcode::A32ClearExclusive:
            case IR::Opcode::A64ClearExclusive:
            case IR::Opcode::A32CallSupervisor:
            case IR::Opcode::A64CallSupervisor:
            case IR::Opcode::A32ExceptionRaised:
            case IR::Opcode::A64ExceptionRaised:
                adjacent = false;
                break;
            default:
                break;
            }
        }
    }
    return false;
}

template<std::size_t bitsize>
void EmitLoadFromPointer(oaknut::CodeGenerator& code, oaknut::XReg value, oaknut::XReg pointer) {
    if constexpr (bitsize == 8) {
        code.LDRB(value.toW(), pointer);
    } else if constexpr (bitsize == 16) {
        code.LDRH(value.toW(), pointer);
    } else if constexpr (bitsize == 32) {
        code.LDR(value.toW(), pointer);
    } else {
        code.LDR(value, pointer);
    }
}

template<std::size_t bitsize>
void EmitStoreToPointer(oaknut::CodeGenerator& code, oaknut::XReg value, oaknut::XReg pointer) {
    if constexpr (bitsize == 8) {
        code.STRB(value.toW(), pointer);
    } else if constexpr (bitsize == 16) {
        code.STRH(value.toW(), pointer);
    } else if constexpr (bitsize == 32) {
        code.STR(value.toW(), pointer);
    } else {
        code.STR(value, pointer);
    }
}

// Uses Xscratch0-2.
void EmitExclusiveLock(oaknut::CodeGenerator& code, EmitContext& ctx) {
    if (IgnoreGlobalMonitor(ctx)) {
        return;
    }
    code.MOV(Xscratch2, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(ctx.conf.global_monitor)));
    EmitSpinLockLock(code, Xscratch2);
}

void EmitExclusiveUnlock(oaknut::CodeGenerator& code, EmitContext& ctx) {
    if (IgnoreGlobalMonitor(ctx)) {
        return;
    }
    code.MOV(Xscratch0, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(ctx.conf.global_monitor)));
    EmitSpinLockUnlock(code, Xscratch0);
}

// out = &generation bucket of the 64-byte granule at Xaddr; bits 6-47 only, so a tagged
// pointer reserves the same granule as an untagged one (x64 EmitExclusiveGenerationBucket).
void EmitGenerationBucket(oaknut::CodeGenerator& code, u64* base, oaknut::XReg Xaddr, oaknut::XReg out, oaknut::XReg scratch) {
    static_assert((ExclusiveMonitor::GENERATION_BUCKETS & (ExclusiveMonitor::GENERATION_BUCKETS - 1)) == 0);
    code.UBFX(out, Xaddr, 6, 42);
    code.EOR(out, out, out, oaknut::LogShift::LSR, 12);
    code.AND(out, out, ExclusiveMonitor::GENERATION_BUCKETS - 1);
    code.MOV(scratch, std::bit_cast<u64>(base));
    code.ADD(out, scratch, out, oaknut::AddSubShift::LSL, 6);
}

// Local monitor set and the reservation recorded: its address, and its granule's
// generation before the value is loaded. Uses Xscratch0/1.
void EmitExclusiveMark(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr) {
    ExclusiveMonitor* const monitor = ctx.conf.global_monitor;
    code.MOV(Wscratch0, 1);
    code.STRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.MOV(Xscratch0, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(monitor, ctx.conf.processor_id)));
    code.STR(Xaddr, Xscratch0);
    if (u64* const generations = GetExclusiveMonitorGenerationBase(monitor)) {
        EmitGenerationBucket(code, generations, Xaddr, Xscratch1, Xscratch0);
        code.LDAR(Xscratch1, Xscratch1);
        code.MOV(Xscratch0, std::bit_cast<u64>(GetExclusiveMonitorGenerationSlot(monitor, ctx.conf.processor_id)));
        code.STR(Xscratch1, Xscratch0);
    }
}

// Branches to `fail` unless this processor holds the reservation for Xaddr. Uses Xscratch0.
void EmitExclusiveCheck(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, oaknut::Label& fail) {
    code.LDRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CBZ(Wscratch0, fail);
    code.MOV(Xscratch0, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(ctx.conf.global_monitor, ctx.conf.processor_id)));
    code.LDR(Xscratch0, Xscratch0);
    code.CMP(Xscratch0, Xaddr);
    code.B(NE, fail);
}

// Claims the granule: advances its generation from the value the load recorded, or
// branches to `fail` because another exclusive store succeeded since. Uses Xscratch0-2.
void EmitGenerationClaim(oaknut::CodeGenerator& code, EmitContext& ctx, u64* generations, oaknut::XReg Xaddr, oaknut::Label& fail) {
    EmitGenerationBucket(code, generations, Xaddr, Xscratch1, Xscratch0);
    code.MOV(Xscratch0, std::bit_cast<u64>(GetExclusiveMonitorGenerationSlot(ctx.conf.global_monitor, ctx.conf.processor_id)));
    code.LDR(Xscratch0, Xscratch0);
    code.ADD(Xscratch2, Xscratch0, 2);
    code.CASAL(Xscratch0, Xscratch2, Xscratch1);
    code.ADD(Xscratch0, Xscratch0, 2);
    code.CMP(Xscratch0, Xscratch2);
    code.B(NE, fail);
}

// Clears every processor's reservation of Xaddr (the lock is held). Uses Xscratch0-2.
void EmitExclusiveTestAndClear(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr) {
    if (IgnoreGlobalMonitor(ctx)) {
        return;
    }
    ExclusiveMonitor* const monitor = ctx.conf.global_monitor;
    ASSERT(GetExclusiveMonitorProcessorStride() == 64);
    oaknut::Label loop, next, done;
    // Slots above the active bound at execution time hold no reservation.
    code.MOV(Xscratch0, std::bit_cast<u64>(GetExclusiveMonitorActiveCountPointer(monitor)));
    code.LDR(Xscratch1, Xscratch0);
    code.MOV(Xscratch0, std::bit_cast<u64>(GetExclusiveMonitorAddressBase(monitor)));
    code.ADD(Xscratch1, Xscratch0, Xscratch1, oaknut::AddSubShift::LSL, 6);
    code.l(loop);
    code.CMP(Xscratch0, Xscratch1);
    code.B(EQ, done);
    code.LDR(Xscratch2, Xscratch0);
    code.CMP(Xscratch2, Xaddr);
    code.B(NE, next);
    code.MOV(Xscratch2, 0xDEAD'DEAD'DEAD'DEADull);
    code.STR(Xscratch2, Xscratch0);
    code.l(next);
    code.ADD(Xscratch0, Xscratch0, 64);
    code.B(loop);
    code.l(done);
}

template<std::size_t bitsize>
void InlineEmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    const auto marker = ShouldFastmem(ctx, inst);
    const u64 value_pointer = std::bit_cast<u64>(GetExclusiveMonitorValuePointer(ctx.conf.global_monitor, ctx.conf.processor_id));

    if constexpr (bitsize == 128) {
        // X1 = vaddr; every caller-saved register is free.
        ctx.reg_alloc.PrepareForCall({}, args[1]);
        EmitExclusiveLock(code, ctx);
        EmitExclusiveMark(code, ctx, X1);
        if (marker) {
            SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
            // A 16-byte aligned LDP is single-copy atomic (FEAT_LSE2); a misaligned pair
            // takes the callback.
            FastmemEmitAtomicAddress<128>(code, ctx, X1, fallback);
            const CodePtr fastmem_location = code.xptr<CodePtr>();
            code.LDP(X2, X3, Xscratch0);
            if (ordered) {
                code.DMB(oaknut::BarrierOp::ISHLD);
            }
            code.FMOV(D0, X2);
            code.FMOV(V0.D()[1], X3);
            code.l(*end);

            ctx.deferred_emits.emplace_back([&code, &ctx, marker = *marker, ordered, fallback, end, fastmem_location] {
                ctx.ebi.fastmem_patch_info.emplace(
                    fastmem_location - ctx.ebi.entry_point,
                    FastmemPatchInfo{
                        .marker = marker,
                        .fc = FakeCall{
                            .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                        },
                        .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                    });

                code.l(*fallback);
                EmitRelocation(code, ctx, LinkTarget::ReadMemory128);
                if (ordered) {
                    code.DMB(oaknut::BarrierOp::ISH);
                }
                code.B(*end);
            });
        } else {
            EmitRelocation(code, ctx, LinkTarget::ReadMemory128);
            if (ordered) {
                code.DMB(oaknut::BarrierOp::ISH);
            }
        }
        code.MOV(Xscratch0, value_pointer);
        code.STR(Q0, Xscratch0);
        EmitExclusiveUnlock(code, ctx);
        code.MOV(Q8.B16(), Q0.B16());
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
        auto Rvalue = ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        ctx.fpsr.Spill();
        ctx.reg_alloc.SpillFlags();
        RegAlloc::Realize(Xaddr, Rvalue);
        const oaknut::XReg Xvalue{Rvalue->index()};

        EmitExclusiveLock(code, ctx);
        EmitExclusiveMark(code, ctx, *Xaddr);
        if (marker) {
            SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
            const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, *Xaddr, fallback);
            const CodePtr fastmem_location = EmitMemoryLdr<bitsize>(code, Xvalue.index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));
            code.l(*end);

            ctx.deferred_emits.emplace_back([&code, &ctx, marker = *marker, Xaddr = *Xaddr, Xvalue, ordered, fallback, end, fastmem_location] {
                ctx.ebi.fastmem_patch_info.emplace(
                    fastmem_location - ctx.ebi.entry_point,
                    FastmemPatchInfo{
                        .marker = marker,
                        .fc = FakeCall{
                            .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                        },
                        .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                    });

                code.l(*fallback);
                code.MOV(Xscratch0, Xaddr);
                EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
                if (ordered) {
                    code.DMB(oaknut::BarrierOp::ISH);
                }
                code.MOV(Xvalue, Xscratch0);
                code.B(*end);
            });
        } else {
            code.MOV(Xscratch0, *Xaddr);
            EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
            if (ordered) {
                code.DMB(oaknut::BarrierOp::ISH);
            }
            code.MOV(Xvalue, Xscratch0);
        }
        code.MOV(Xscratch0, value_pointer);
        EmitStoreToPointer<bitsize>(code, Xvalue, Xscratch0);
        EmitExclusiveUnlock(code, ctx);
    }
    EmitInlineCheckMemoryAbort(code, ctx, inst);
}

LinkTarget WrappedCompareWriteMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedCompareWriteMemory8;
    case 16:
        return LinkTarget::WrappedCompareWriteMemory16;
    case 32:
        return LinkTarget::WrappedCompareWriteMemory32;
    case 64:
        return LinkTarget::WrappedCompareWriteMemory64;
    }
    UNREACHABLE();
}

template<std::size_t bitsize>
void InlineEmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    const auto marker = ShouldFastmem(ctx, inst);
    const u64 value_pointer = std::bit_cast<u64>(GetExclusiveMonitorValuePointer(ctx.conf.global_monitor, ctx.conf.processor_id));
    u64* const generations = GetExclusiveMonitorGenerationBase(ctx.conf.global_monitor);
    // A plain store-exclusive of the value its adjacent load returned succeeds without
    // writing: the pair linearizes at the single-copy atomic load.
    const bool elide = ctx.conf.elide_unchanged_exclusive_stores && !ordered && ExclusivePairIsAdjacent(ctx.block, inst);
    const bool claim = generations && !ExclusivePairIsLocal(ctx.block, inst);

    if constexpr (bitsize == 128) {
        // X1 = vaddr, Q0 = value; every caller-saved register is free. W0 = status.
        ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
        SharedLabel end = GenSharedLabel();

        EmitExclusiveLock(code, ctx);
        code.MOV(W0, 1);
        EmitExclusiveCheck(code, ctx, X1, *end);
        if (elide) {
            oaknut::Label store;
            code.MOV(Xscratch0, value_pointer);
            code.LDP(X2, X3, Xscratch0);
            code.FMOV(X4, D0);
            code.FMOV(X5, V0.D()[1]);
            code.CMP(X2, X4);
            code.CCMP(X3, X5, 0, EQ);
            code.B(NE, store);
            code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
            code.MOV(W0, WZR);
            code.B(*end);
            code.l(store);
        }
        if (claim) {
            EmitGenerationClaim(code, ctx, generations, X1, *end);
        }
        EmitExclusiveTestAndClear(code, ctx, X1);
        code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
        // X2:X3 = expected (CASP's comparand and old value), X6:X7 its copy.
        code.MOV(Xscratch0, value_pointer);
        code.LDP(X2, X3, Xscratch0);
        code.MOV(X6, X2);
        code.MOV(X7, X3);
        const auto emit_callback = [&code, &ctx] {
            code.FMOV(D1, X6);
            code.FMOV(V1.D()[1], X7);
            EmitRelocation(code, ctx, LinkTarget::CompareWriteMemory128);
            code.EOR(W0, W0, 1);
        };
        if (marker) {
            SharedLabel fallback = GenSharedLabel();
            FastmemEmitAtomicAddress<128>(code, ctx, X1, fallback);
            code.FMOV(X4, D0);
            code.FMOV(X5, V0.D()[1]);
            const CodePtr fastmem_location = code.xptr<CodePtr>();
            if (ordered) {
                code.CASPAL(X2, X3, X4, X5, Xscratch0);
            } else {
                code.CASP(X2, X3, X4, X5, Xscratch0);
            }
            code.CMP(X2, X6);
            code.CCMP(X3, X7, 0, EQ);
            code.CSET(W0, NE);

            ctx.deferred_emits.emplace_back([&code, &ctx, marker = *marker, fallback, end, fastmem_location, emit_callback] {
                ctx.ebi.fastmem_patch_info.emplace(
                    fastmem_location - ctx.ebi.entry_point,
                    FastmemPatchInfo{
                        .marker = marker,
                        .fc = FakeCall{
                            .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                        },
                        .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                    });

                code.l(*fallback);
                emit_callback();
                code.B(*end);
            });
        } else {
            emit_callback();
        }
        code.l(*end);
        EmitExclusiveUnlock(code, ctx);
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    } else {
        auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
        auto Rvalue = ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        auto Wstatus = ctx.reg_alloc.WriteW(inst);
        ctx.fpsr.Spill();
        ctx.reg_alloc.SpillFlags();
        RegAlloc::Realize(Xaddr, Rvalue, Wstatus);
        const oaknut::XReg Xvalue{Rvalue->index()};
        const oaknut::WReg Wresult = *Wstatus;
        SharedLabel end = GenSharedLabel();

        EmitExclusiveLock(code, ctx);
        code.MOV(Wresult, 1);
        EmitExclusiveCheck(code, ctx, *Xaddr, *end);
        if (elide) {
            oaknut::Label store;
            code.MOV(Xscratch0, value_pointer);
            EmitLoadFromPointer<bitsize>(code, Xscratch1, Xscratch0);
            if constexpr (bitsize == 8) {
                code.CMP(Wscratch1, Xvalue.toW(), oaknut::AddSubExt::UXTB);
            } else if constexpr (bitsize == 16) {
                code.CMP(Wscratch1, Xvalue.toW(), oaknut::AddSubExt::UXTH);
            } else if constexpr (bitsize == 32) {
                code.CMP(Wscratch1, Xvalue.toW());
            } else {
                code.CMP(Xscratch1, Xvalue);
            }
            code.B(NE, store);
            code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
            code.MOV(Wresult, WZR);
            code.B(*end);
            code.l(store);
        }
        if (claim) {
            EmitGenerationClaim(code, ctx, generations, *Xaddr, *end);
        }
        EmitExclusiveTestAndClear(code, ctx, *Xaddr);
        code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
        // Xscratch1 = expected (CAS's comparand and old value), Xscratch2 its copy.
        code.MOV(Xscratch0, value_pointer);
        EmitLoadFromPointer<bitsize>(code, Xscratch1, Xscratch0);
        code.MOV(Xscratch2, Xscratch1);
        const auto emit_callback = [&code, &ctx, Xaddr = *Xaddr, Xvalue, Wresult] {
            code.FMOV(D0, Xscratch2);
            code.MOV(Xscratch1, Xvalue);
            code.MOV(Xscratch0, Xaddr);
            EmitRelocation(code, ctx, WrappedCompareWriteMemoryLinkTarget(bitsize));
            code.EOR(Wresult, Wscratch0, 1);
        };
        if (marker) {
            SharedLabel fallback = GenSharedLabel();
            const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, *Xaddr, fallback);
            if (ShouldExt32(ctx)) {
                code.ADD(Xscratch0, Xbase, Xoffset.toW(), oaknut::AddSubExt::UXTW);
            } else {
                code.ADD(Xscratch0, Xbase, Xoffset);
            }
            const CodePtr fastmem_location = code.xptr<CodePtr>();
            const oaknut::WReg Wvalue = Xvalue.toW();
            if constexpr (bitsize == 8) {
                ordered ? code.CASALB(Wscratch1, Wvalue, Xscratch0) : code.CASB(Wscratch1, Wvalue, Xscratch0);
            } else if constexpr (bitsize == 16) {
                ordered ? code.CASALH(Wscratch1, Wvalue, Xscratch0) : code.CASH(Wscratch1, Wvalue, Xscratch0);
            } else if constexpr (bitsize == 32) {
                ordered ? code.CASAL(Wscratch1, Wvalue, Xscratch0) : code.CAS(Wscratch1, Wvalue, Xscratch0);
            } else {
                ordered ? code.CASAL(Xscratch1, Xvalue, Xscratch0) : code.CAS(Xscratch1, Xvalue, Xscratch0);
            }
            code.CMP(Xscratch1, Xscratch2);
            code.CSET(Wresult, NE);

            ctx.deferred_emits.emplace_back([&code, &ctx, marker = *marker, fallback, end, fastmem_location, emit_callback] {
                ctx.ebi.fastmem_patch_info.emplace(
                    fastmem_location - ctx.ebi.entry_point,
                    FastmemPatchInfo{
                        .marker = marker,
                        .fc = FakeCall{
                            .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                        },
                        .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                    });

                code.l(*fallback);
                emit_callback();
                code.B(*end);
            });
        } else {
            emit_callback();
        }
        code.l(*end);
        EmitExclusiveUnlock(code, ctx);
    }
    EmitInlineCheckMemoryAbort(code, ctx, inst);
}

}  // namespace

template<std::size_t bitsize>
void EmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitReadMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitReadMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitReadMemory<bitsize>(code, ctx, inst);
    }
}

template<std::size_t bitsize>
void EmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (UseInlineExclusives(ctx)) {
        InlineEmitExclusiveReadMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitExclusiveReadMemory<bitsize>(code, ctx, inst);
    }
}

template<std::size_t bitsize>
void EmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitWriteMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitWriteMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitWriteMemory<bitsize>(code, ctx, inst);
    }
}

template<std::size_t bitsize>
void EmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (UseInlineExclusives(ctx)) {
        InlineEmitExclusiveWriteMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitExclusiveWriteMemory<bitsize>(code, ctx, inst);
    }
}

template<std::size_t bitsize>
void EmitAtomicMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemoryImpl<bitsize, false>(code, ctx, inst);
}

template<std::size_t bitsize>
void EmitCompareAndSwapMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if constexpr (bitsize == 128) {
        EmitCompareAndSwapMemory128Impl(code, ctx, inst);
    } else {
        EmitAtomicMemoryImpl<bitsize, true>(code, ctx, inst);
    }
}

template void EmitReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitAtomicMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitAtomicMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitAtomicMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitAtomicMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitCompareAndSwapMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitCompareAndSwapMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitCompareAndSwapMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitCompareAndSwapMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitCompareAndSwapMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);

}  // namespace Dynarmic::Backend::Arm64
