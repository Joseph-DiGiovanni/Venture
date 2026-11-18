// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once
#include <cstddef>

namespace oaknut {
struct CodeGenerator;
struct Label;
}  // namespace oaknut

namespace Dynarmic::IR {
enum class AccType;
class Inst;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Arm64 {

struct EmitContext;
enum class LinkTarget;

template<std::size_t bitsize>
void EmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template<std::size_t bitsize>
void EmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template<std::size_t bitsize>
void EmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template<std::size_t bitsize>
void EmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
/// A64 LSE atomics: native acquire-release LSE on the fastmem path, the
/// UserCallbacks::MemoryAtomic* callbacks otherwise or on a fault.
template<std::size_t bitsize>
void EmitAtomicMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template<std::size_t bitsize>
void EmitCompareAndSwapMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);

}  // namespace Dynarmic::Backend::Arm64
