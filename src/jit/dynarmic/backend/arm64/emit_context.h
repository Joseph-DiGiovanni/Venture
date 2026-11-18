/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "common/container/unordered_map.h"

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Arm64 {

struct EmitConfig;
class FastmemManager;
class FpsrManager;

using SharedLabel = std::shared_ptr<oaknut::Label>;

inline SharedLabel GenSharedLabel() {
    return std::make_shared<oaknut::Label>();
}

// A register value a memory access's abort exit stores (A64ColdSet*). The location is
// the value's after the access, where the abort exit leaves.
struct ColdStateStore {
    IR::Opcode opcode;
    std::size_t index;
    std::optional<HostLoc> location;  // nullopt: immediate
    u64 immediate;
};

struct EmitContext {
    IR::Block& block;
    RegAlloc& reg_alloc;
    const EmitConfig& conf;
    EmittedBlockInfo& ebi;
    FpsrManager& fpsr;
    FastmemManager& fastmem;

    std::vector<std::function<void()>> deferred_emits;
    // Keyed by the access. Complete once the block's instructions are emitted, before
    // the deferred emits that hold the abort exits.
    ::Common::unordered_map<const IR::Inst*, std::vector<ColdStateStore>> cold_state{};

    FP::FPCR FPCR(bool fpcr_controlled = true) const {
        const FP::FPCR fpcr = conf.descriptor_to_fpcr(block.Location());
        return fpcr_controlled ? fpcr : fpcr.ASIMDStandardValue();
    }
};

}  // namespace Dynarmic::Backend::Arm64
