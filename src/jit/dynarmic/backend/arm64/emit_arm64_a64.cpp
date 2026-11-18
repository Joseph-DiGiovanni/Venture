// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <bit>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

oaknut::Label EmitA64Cond(oaknut::CodeGenerator& code, EmitContext&, IR::Cond cond) {
    oaknut::Label pass;
    // TODO: Flags in host flags
    code.LDR(Wscratch0, Xstate, offsetof(A64JitState, cpsr_nzcv));
    code.MSR(oaknut::SystemReg::NZCV, Xscratch0);
    code.B(static_cast<oaknut::Cond>(cond), pass);
    return pass;
}

void EmitA64LeafTerminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step);
void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step);

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::ReturnToDispatch, IR::LocationDescriptor, bool) {
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step);

// Forward links within one halt-polling window from a conditional (If/CheckBit) block
// may skip the halt poll; every cycle of linked blocks still has a polling backward edge.
static bool ElideHaltPoll(EmitContext& ctx, u64 from, u64 to, bool is_single_step) {
    const auto terminal = ctx.block.GetTerminal();
    return ctx.conf.enable_bounded_halt_polling && !is_single_step && to > from &&
           to / A64::halt_polling_window_size == from / A64::halt_polling_window_size &&
           (std::holds_alternative<IR::Term::If>(terminal) || std::holds_alternative<IR::Term::CheckBit>(terminal));
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LinkBlock terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    const u64 from = A64::LocationDescriptor{initial_location}.PC();
    const u64 to = A64::LocationDescriptor{terminal.next}.PC();
    if (ElideHaltPoll(ctx, from, to, is_single_step)) {
        return EmitA64Terminal(code, ctx, IR::Term::LinkBlockFast{terminal.next}, initial_location, is_single_step);
    }

    oaknut::Label fail;

    if (ctx.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        if (ctx.conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, fail);
            EmitBlockLinkRelocation(code, ctx, terminal.next, BlockRelocationType::Branch);
        } else {
            code.LDAR(Wscratch0, Xhalt);
            code.CBNZ(Wscratch0, fail);
            EmitBlockLinkRelocation(code, ctx, terminal.next, BlockRelocationType::Branch);
        }
    }

    code.l(fail);
    code.MOV(Xscratch0, A64::LocationDescriptor{terminal.next}.PC());
    code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    // Backward and self links, and links leaving the halt-polling window, observe
    // halts and cache invalidation.
    const u64 from = A64::LocationDescriptor{initial_location}.PC();
    const u64 to = A64::LocationDescriptor{terminal.next}.PC();
    if (to <= from || (ctx.conf.enable_bounded_halt_polling && to / A64::halt_polling_window_size != from / A64::halt_polling_window_size)) {
        return EmitA64Terminal(code, ctx, IR::Term::LinkBlock{terminal.next}, initial_location, is_single_step);
    }

    if (ctx.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        EmitBlockLinkRelocation(code, ctx, terminal.next, BlockRelocationType::Branch);
    }

    code.MOV(Xscratch0, A64::LocationDescriptor{terminal.next}.PC());
    code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::PopRSBHint, IR::LocationDescriptor, bool is_single_step) {
    if (ctx.conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) && !is_single_step) {
        oaknut::Label fail;

        code.MOV(Wscratch0, A64::LocationDescriptor::fpcr_mask);
        code.LDR(W0, Xstate, offsetof(A64JitState, fpcr));
        code.LDR(X1, Xstate, offsetof(A64JitState, pc));
        code.AND(W0, W0, Wscratch0);
        code.AND(X1, X1, A64::LocationDescriptor::pc_mask);
        code.LSL(X0, X0, A64::LocationDescriptor::fpcr_shift);
        code.ORR(X0, X0, X1);

        code.LDR(Wscratch2, SP, offsetof(StackLayout, rsb_ptr));
        code.AND(Wscratch2, Wscratch2, RSBIndexMask);
        code.ADD(X2, SP, Xscratch2);
        code.SUB(Wscratch2, Wscratch2, sizeof(RSBEntry));
        code.STR(Wscratch2, SP, offsetof(StackLayout, rsb_ptr));

        code.LDP(Xscratch0, Xscratch1, X2, offsetof(StackLayout, rsb));

        code.CMP(X0, Xscratch0);
        code.B(NE, fail);
        code.BR(Xscratch1);

        code.l(fail);
    }

    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::FastDispatchHint, IR::LocationDescriptor, bool) {
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);

    // TODO: Implement FastDispatchHint optimization
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    oaknut::Label pass = EmitA64Cond(code, ctx, terminal.if_);
    EmitA64LeafTerminal(code, ctx, terminal.else_, initial_location, is_single_step);
    code.l(pass);
    EmitA64LeafTerminal(code, ctx, terminal.then_, initial_location, is_single_step);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    oaknut::Label fail;
    code.LDRB(Wscratch0, SP, offsetof(StackLayout, check_bit));
    code.CBZ(Wscratch0, fail);
    EmitA64LeafTerminal(code, ctx, terminal.then_, initial_location, is_single_step);
    code.l(fail);
    EmitA64LeafTerminal(code, ctx, terminal.else_, initial_location, is_single_step);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::CheckHalt terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    oaknut::Label fail;
    code.LDAR(Wscratch0, Xhalt);
    code.CBNZ(Wscratch0, fail);
    EmitA64LeafTerminal(code, ctx, terminal.else_, initial_location, is_single_step);
    code.l(fail);
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64LeafTerminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (auto const x = std::get_if<IR::Term::ReturnToDispatch>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::LinkBlock>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::LinkBlockFast>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::PopRSBHint>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::FastDispatchHint>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    UNREACHABLE();
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (auto const x = std::get_if<IR::Term::LeafTerminal>(&terminal))
        return EmitA64LeafTerminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::If>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::CheckBit>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::CheckHalt>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    UNREACHABLE();
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx) {
    const A64::LocationDescriptor location{ctx.block.Location()};
    EmitA64Terminal(code, ctx, ctx.block.GetTerminal(), location.SetSingleStepping(false), location.SingleStepping());
}

void EmitA64ConditionFailedTerminal(oaknut::CodeGenerator& code, EmitContext& ctx) {
    const A64::LocationDescriptor location{ctx.block.Location()};
    EmitA64Terminal(code, ctx, IR::Term::LinkBlock{ctx.block.ConditionFailedLocation()}, location.SetSingleStepping(false), location.SingleStepping());
}

static void EmitColdStateStores(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* access) {
    const auto found = ctx.cold_state.find(access);
    if (found == ctx.cold_state.end()) {
        return;
    }
    using Op = IR::Opcode;
    constexpr std::size_t spill_offset = offsetof(StackLayout, spill);
    constexpr std::size_t spill_slot_size = sizeof(decltype(StackLayout::spill)::value_type);
    // The exit leaves the block, so only Xscratch0/1 are needed and nothing is restored.
    for (const ColdStateStore& store : found->second) {
        const bool vector = store.opcode == Op::A64ColdSetS || store.opcode == Op::A64ColdSetD || store.opcode == Op::A64ColdSetQ;
        if (vector) {
            const std::size_t offset = offsetof(A64JitState, vec) + sizeof(u64) * 2 * store.index;
            ASSERT(store.location && store.location->kind != HostLoc::Kind::Gpr && store.location->kind != HostLoc::Kind::Flags);
            // SetS/SetD store the low 4/8 bytes zero-extended to 16, SetQ all 16.
            if (store.location->kind == HostLoc::Kind::Fpr) {
                const int reg = store.location->index;
                switch (store.opcode) {
                case Op::A64ColdSetS:
                    code.FMOV(Wscratch0, oaknut::SReg{reg});
                    break;
                case Op::A64ColdSetD:
                    code.FMOV(Xscratch0, oaknut::DReg{reg});
                    break;
                default:
                    code.STR(oaknut::QReg{reg}, Xstate, offset);
                    continue;
                }
            } else {
                const std::size_t slot = spill_offset + store.location->index * spill_slot_size;
                switch (store.opcode) {
                case Op::A64ColdSetS:
                    code.LDR(Wscratch0, SP, slot);
                    break;
                case Op::A64ColdSetD:
                    code.LDR(Xscratch0, SP, slot);
                    break;
                default:
                    code.LDR(Xscratch0, SP, slot);
                    code.LDR(Xscratch1, SP, slot + 8);
                    code.STR(Xscratch0, Xstate, offset);
                    code.STR(Xscratch1, Xstate, offset + 8);
                    continue;
                }
            }
            code.STR(Xscratch0, Xstate, offset);
            code.STR(XZR, Xstate, offset + 8);
            continue;
        }

        if (!store.location) {
            code.MOV(Xscratch0, store.immediate);
        } else {
            switch (store.location->kind) {
            case HostLoc::Kind::Gpr:
                code.MOV(Xscratch0, oaknut::XReg{store.location->index});
                break;
            case HostLoc::Kind::Fpr:
                code.FMOV(Xscratch0, oaknut::DReg{store.location->index});
                break;
            case HostLoc::Kind::Spill:
                code.LDR(Xscratch0, SP, spill_offset + store.location->index * spill_slot_size);
                break;
            default:
                // Memory accesses spill the host flags before the access.
                UNREACHABLE();
            }
        }
        switch (store.opcode) {
        case Op::A64ColdSetW:
            code.MOV(Wscratch0, Wscratch0);
            code.STR(Xscratch0, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * store.index);
            break;
        case Op::A64ColdSetX:
            code.STR(Xscratch0, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * store.index);
            break;
        case Op::A64ColdSetSP:
            code.STR(Xscratch0, Xstate, offsetof(A64JitState, sp));
            break;
        case Op::A64ColdSetNZCV:
            code.STR(Wscratch0, Xstate, offsetof(A64JitState, cpsr_nzcv));
            break;
        default:
            UNREACHABLE();
        }
    }
}

void EmitA64CheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end) {
    if (!ctx.conf.check_halt_on_memory_access) {
        return;
    }

    const A64::LocationDescriptor current_location{IR::LocationDescriptor{inst->GetArg(0).GetU64()}};

    code.LDAR(Wscratch0, Xhalt);
    code.TST(Wscratch0, static_cast<u32>(HaltReason::MemoryAbort));
    code.B(EQ, end);
    EmitColdStateStores(code, ctx, inst);
    code.MOV(Xscratch0, current_location.PC());
    code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
    EmitRelocation(code, ctx, LinkTarget::ReturnFromRunCode);
}

static void RecordColdState(EmitContext& ctx, IR::Inst* inst) {
    // Emits nothing: the argument keeps the value where it is until this instruction,
    // which directly follows the access.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto opcode = inst->GetOpcode();
    const bool has_register = opcode != IR::Opcode::A64ColdSetSP && opcode != IR::Opcode::A64ColdSetNZCV;
    ColdStateStore store{opcode, 0, std::nullopt, 0};
    if (opcode == IR::Opcode::A64ColdSetW || opcode == IR::Opcode::A64ColdSetX) {
        store.index = static_cast<std::size_t>(inst->GetArg(1).GetA64RegRef());
    } else if (has_register) {
        store.index = static_cast<std::size_t>(inst->GetArg(1).GetA64VecRef());
    }
    const auto& value = args[has_register ? 2 : 1];
    if (value.IsImmediate()) {
        store.immediate = value.GetImmediateU64();
    } else {
        store.location = ctx.reg_alloc.ArgumentLocation(value);
        ASSERT(store.location);
    }
    ctx.cold_state[inst->GetArg(0).GetInst()].push_back(store);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetW>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetX>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetS>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetD>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetQ>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetSP>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetNZCV>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64SetCheckBit>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (args[0].IsImmediate()) {
        if (args[0].GetImmediateU1()) {
            code.MOV(Wscratch0, 1);
            code.STRB(Wscratch0, SP, offsetof(StackLayout, check_bit));
        } else {
            code.STRB(WZR, SP, offsetof(StackLayout, check_bit));
        }
    } else {
        auto Wbit = ctx.reg_alloc.ReadW(args[0]);
        RegAlloc::Realize(Wbit);
        code.STRB(Wbit, SP, offsetof(StackLayout, check_bit));
    }
}

template<>
void EmitIR<IR::Opcode::A64GetCFlag>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wflag = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wflag);
    code.LDR(Wflag, Xstate, offsetof(A64JitState, cpsr_nzcv));
    code.AND(Wflag, Wflag, 1 << 29);
}

template<>
void EmitIR<IR::Opcode::A64GetNZCVRaw>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wnzcv = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wnzcv);

    code.LDR(Wnzcv, Xstate, offsetof(A64JitState, cpsr_nzcv));
}

template<>
void EmitIR<IR::Opcode::A64SetNZCVRaw>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wnzcv = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wnzcv);

    // MSR NZCV discards the reserved bits.
    code.AND(Wscratch0, Wnzcv, 0xf0000000);
    code.STR(Wscratch0, Xstate, offsetof(A64JitState, cpsr_nzcv));
}

template<>
void EmitIR<IR::Opcode::A64SetNZCV>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wnzcv = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wnzcv);

    code.STR(Wnzcv, Xstate, offsetof(A64JitState, cpsr_nzcv));
}

template<>
void EmitIR<IR::Opcode::A64GetW>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto Wresult = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wresult);

    // TODO: Detect if Gpr vs Fpr is more appropriate

    code.LDR(Wresult, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64GetX>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto Xresult = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xresult);

    // TODO: Detect if Gpr vs Fpr is more appropriate

    code.LDR(Xresult, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64GetS>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Sresult = ctx.reg_alloc.WriteS(inst);
    RegAlloc::Realize(Sresult);
    code.LDR(Sresult, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64GetD>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Dresult = ctx.reg_alloc.WriteD(inst);
    RegAlloc::Realize(Dresult);
    code.LDR(Dresult, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64GetQ>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Qresult = ctx.reg_alloc.WriteQ(inst);
    RegAlloc::Realize(Qresult);
    code.LDR(Qresult, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64GetSP>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xresult = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xresult);

    code.LDR(Xresult, Xstate, offsetof(A64JitState, sp));
}

template<>
void EmitIR<IR::Opcode::A64GetFPCR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wresult = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wresult);

    code.LDR(Wresult, Xstate, offsetof(A64JitState, fpcr));
}

template<>
void EmitIR<IR::Opcode::A64GetFPSR>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    auto Wresult = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wresult);

    ctx.fpsr.GetFpsr(Wresult);
}

template<>
void EmitIR<IR::Opcode::A64SetW>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    auto Wvalue = ctx.reg_alloc.ReadW(args[1]);
    RegAlloc::Realize(Wvalue);

    // TODO: Detect if Gpr vs Fpr is more appropriate
    code.MOV(*Wvalue, Wvalue);
    code.STR(Wvalue->toX(), Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64SetX>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    auto Xvalue = ctx.reg_alloc.ReadX(args[1]);
    RegAlloc::Realize(Xvalue);

    // TODO: Detect if Gpr vs Fpr is more appropriate

    code.STR(Xvalue, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64SetS>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Svalue = ctx.reg_alloc.ReadS(args[1]);
    RegAlloc::Realize(Svalue);

    code.FMOV(Svalue, Svalue);
    code.STR(Svalue->toQ(), Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64SetD>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Dvalue = ctx.reg_alloc.ReadD(args[1]);
    RegAlloc::Realize(Dvalue);

    code.FMOV(Dvalue, Dvalue);
    code.STR(Dvalue->toQ(), Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64SetQ>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Qvalue = ctx.reg_alloc.ReadQ(args[1]);
    RegAlloc::Realize(Qvalue);
    code.STR(Qvalue, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64SetSP>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
    RegAlloc::Realize(Xvalue);
    code.STR(Xvalue, Xstate, offsetof(A64JitState, sp));
}

template<>
void EmitIR<IR::Opcode::A64SetFPCR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wvalue = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wvalue);
    code.STR(Wvalue, Xstate, offsetof(A64JitState, fpcr));
    code.MSR(oaknut::SystemReg::FPCR, Wvalue->toX());
}

template<>
void EmitIR<IR::Opcode::A64SetFPSR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wvalue = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wvalue);
    code.STR(Wvalue, Xstate, offsetof(A64JitState, fpsr));
    code.MSR(oaknut::SystemReg::FPSR, Wvalue->toX());
}

template<>
void EmitIR<IR::Opcode::A64SetPC>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
    RegAlloc::Realize(Xvalue);
    code.STR(Xvalue, Xstate, offsetof(A64JitState, pc));
}

template<>
void EmitIR<IR::Opcode::A64CallSupervisor>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate());
    const u32 imm = args[0].GetImmediateU32();
    ctx.reg_alloc.PrepareForCall();

    if (ctx.conf.enable_cycle_counting) {
        code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
        code.SUB(X1, X1, Xticks);
        EmitRelocation(code, ctx, LinkTarget::AddTicks);
    }

    // Fast calls return to the next instruction without leaving Run; the block's
    // CheckHalt terminal exits if the callback halted. Every guest register is in the
    // JitState across the call. Only svc_fp_transfer switches to the host FP environment.
    const auto& conf = ctx.conf;
    const bool transfer_fp = conf.svc_fp_transfer && imm == *conf.svc_fp_transfer;
    const bool guest_fp = conf.svc_fp_guest && imm == *conf.svc_fp_guest;
    const bool fast_svc = transfer_fp || guest_fp || (conf.svc_fast && imm == *conf.svc_fast);
    const void* const known = fast_svc && conf.svc_lookup ? conf.svc_lookup(A64::LocationDescriptor{ctx.block.Location()}.PC(), imm) : nullptr;
    code.MOV(W1, imm);
    if (known) {
        code.MOV(X2, std::bit_cast<u64>(known));
        code.MOV(X3, Xstate);
        EmitRelocation(code, ctx, transfer_fp ? LinkTarget::CallSVCFastKnownHostFp : LinkTarget::CallSVCFastKnown);
    } else if (fast_svc) {
        EmitRelocation(code, ctx, transfer_fp ? LinkTarget::CallSVCFastHostFp : LinkTarget::CallSVCFast);
    } else {
        EmitRelocation(code, ctx, LinkTarget::CallSVC);
    }

    if (ctx.conf.enable_cycle_counting) {
        EmitRelocation(code, ctx, LinkTarget::GetTicksRemaining);
        code.STR(X0, SP, offsetof(StackLayout, cycles_to_run));
        code.MOV(Xticks, X0);
    }

    // The kernel would have to execute ERET to get here, which would clear exclusive state.
    code.STR(WZR, Xstate, offsetof(A64JitState, exclusive_state));
}

template<>
void EmitIR<IR::Opcode::A64ExceptionRaised>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall();

    if (ctx.conf.enable_cycle_counting) {
        code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
        code.SUB(X1, X1, Xticks);
        EmitRelocation(code, ctx, LinkTarget::AddTicks);
    }

    code.MOV(X1, args[0].GetImmediateU64());
    code.MOV(X2, args[1].GetImmediateU64());
    EmitRelocation(code, ctx, LinkTarget::ExceptionRaised);

    if (ctx.conf.enable_cycle_counting) {
        EmitRelocation(code, ctx, LinkTarget::GetTicksRemaining);
        code.STR(X0, SP, offsetof(StackLayout, cycles_to_run));
        code.MOV(Xticks, X0);
    }
}

template<>
void EmitIR<IR::Opcode::A64DataCacheOperationRaised>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    EmitRelocation(code, ctx, LinkTarget::DataCacheOperationRaised);
}

template<>
void EmitIR<IR::Opcode::A64InstructionCacheOperationRaised>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[0], args[1]);
    EmitRelocation(code, ctx, LinkTarget::InstructionCacheOperationRaised);
}

template<>
void EmitIR<IR::Opcode::A64DataSynchronizationBarrier>(oaknut::CodeGenerator& code, EmitContext&, IR::Inst*) {
    code.DSB(oaknut::BarrierOp::SY);
}

template<>
void EmitIR<IR::Opcode::A64DataMemoryBarrier>(oaknut::CodeGenerator& code, EmitContext&, IR::Inst* inst) {
    // Guest threads all run in the host's inner-shareable domain; keep the access types
    // (CRm<1:0>: 01 loads, 10 stores, 11 and the reserved 00 all).
    switch (inst->GetArg(0).GetU8() & 0b11) {
    case 0b01:
        code.DMB(oaknut::BarrierOp::ISHLD);
        break;
    case 0b10:
        code.DMB(oaknut::BarrierOp::ISHST);
        break;
    default:
        code.DMB(oaknut::BarrierOp::ISH);
        break;
    }
}

template<>
void EmitIR<IR::Opcode::A64InstructionSynchronizationBarrier>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst*) {
    if (!ctx.conf.hook_isb) {
        return;
    }

    ctx.reg_alloc.PrepareForCall();
    EmitRelocation(code, ctx, LinkTarget::InstructionSynchronizationBarrierRaised);
}

template<>
void EmitIR<IR::Opcode::A64GetCNTFRQ>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xvalue = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xvalue, ctx.conf.cntfreq_el0);
}

template<>
void EmitIR<IR::Opcode::A64GetCNTPCT>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.PrepareForCall();
    if (!ctx.conf.wall_clock_cntpct && ctx.conf.enable_cycle_counting) {
        code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
        code.SUB(X1, X1, Xticks);
        EmitRelocation(code, ctx, LinkTarget::AddTicks);
        EmitRelocation(code, ctx, LinkTarget::GetTicksRemaining);
        code.STR(X0, SP, offsetof(StackLayout, cycles_to_run));
        code.MOV(Xticks, X0);
    }
    EmitRelocation(code, ctx, LinkTarget::GetCNTPCT);
    ctx.reg_alloc.DefineAsRegister(inst, X0);
}

template<>
void EmitIR<IR::Opcode::A64GetCTR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wvalue = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wvalue);
    code.MOV(Wvalue, ctx.conf.ctr_el0);
}

template<>
void EmitIR<IR::Opcode::A64GetDCZID>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wvalue = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wvalue);
    code.MOV(Wvalue, ctx.conf.dczid_el0);
}

template<>
void EmitIR<IR::Opcode::A64GetTPIDR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xvalue = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xscratch0, std::bit_cast<u64>(ctx.conf.tpidr_el0));
    code.LDR(Xvalue, Xscratch0);
}

template<>
void EmitIR<IR::Opcode::A64GetTPIDRRO>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xvalue = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xscratch0, std::bit_cast<u64>(ctx.conf.tpidrro_el0));
    code.LDR(Xvalue, Xscratch0);
}

template<>
void EmitIR<IR::Opcode::A64SetTPIDR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xscratch0, std::bit_cast<u64>(ctx.conf.tpidr_el0));
    code.STR(Xvalue, Xscratch0);
}

}  // namespace Dynarmic::Backend::Arm64
