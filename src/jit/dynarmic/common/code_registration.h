// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "dynarmic/interface/profiling.h"

namespace Dynarmic {

// Set by SetCodeRegistrationCallback; backends report each emitted block and
// a full cache clear (start == end == nullptr, name "cache_clear") through it.
extern std::atomic<CodeRegistrationCallback> code_registration;

}  // namespace Dynarmic
