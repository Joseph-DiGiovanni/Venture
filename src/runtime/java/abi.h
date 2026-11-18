#pragma once
#include <jni.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <vector>

#include "core/hle.h"
#include "core/thread.h"

namespace venture::java {
enum class Type : char {
  Void = 'V',
  Boolean = 'Z',
  Byte = 'B',
  Char = 'C',
  Short = 'S',
  Int = 'I',
  Long = 'J',
  Float = 'F',
  Double = 'D',
  Object = 'L'
};
struct Descriptor {
  std::vector<Type> parameters;
  Type result;
  static Descriptor parse(std::string_view text);  // throws invalid_argument
};
enum class ArgumentForm { Ellipsis, VaList, Array };
std::vector<jvalue> read_arguments(const Descriptor&, hle::ArgCursor&, ArgumentForm);
std::vector<jvalue> read_arguments32(const Descriptor&, hle::a32::ArgCursor32&, ArgumentForm);

// JNI design.md: escaping is over UTF-16 code units, NOT UTF-8 bytes.
std::string mangle(std::u16string_view text);
std::u16string unescape(std::string_view text);
struct NativeName {
  std::u16string owner, method;
  std::optional<std::u16string> parameters;
};
NativeName demangle(std::string_view symbol);
std::string native_name(std::u16string_view owner, std::u16string_view method,
                        std::optional<std::u16string_view> parameters = {});
std::u16string from_modified_utf8(std::string_view);
std::string to_modified_utf8(std::u16string_view);

// Captured by platform/{win32,posix}/java_entry*.S. Offsets are asserted there
// by matching this layout; stack points at the first ABI stack argument.
#if defined(__aarch64__)
inline constexpr std::size_t kHostGpRegisters = 8;
#else
inline constexpr std::size_t kHostGpRegisters = 6;
#endif
struct HostArguments {
  std::array<std::uint64_t, kHostGpRegisters> gp{};
  std::array<std::uint64_t, 8> fp{};
  const std::uint64_t* stack = nullptr;
};
#if defined(__aarch64__)
static_assert(offsetof(HostArguments, fp) == 64);
static_assert(offsetof(HostArguments, stack) == 128);
#else
static_assert(offsetof(HostArguments, fp) == 48);
static_assert(offsetof(HostArguments, stack) == 112);
#endif
enum class HostAbi { Win64, SysV, Aapcs64 };
#if defined(_WIN32)
inline constexpr HostAbi kHostAbi = HostAbi::Win64;
#elif defined(__aarch64__)
inline constexpr HostAbi kHostAbi = HostAbi::Aapcs64;
#else
inline constexpr HostAbi kHostAbi = HostAbi::SysV;
#endif
// Decoder input independent of the build host's HostArguments, so every
// HostAbi decodes on any host. gp must hold the ABI's argument registers.
struct HostRegisters {
  std::span<const std::uint64_t> gp, fp;
  const std::uint64_t* stack = nullptr;
  HostRegisters(std::span<const std::uint64_t> g, std::span<const std::uint64_t> f, const std::uint64_t* s)
      : gp(g), fp(f), stack(s) {}
  HostRegisters(const HostArguments& a) : gp(a.gp), fp(a.fp), stack(a.stack) {}
};
// Where a JNI parameter (after JNIEnv* and jclass) arrives. Win64: four shared
// positional slots. SysV (6 GP) and AAPCS64 (8 GP): GP and FP/SIMD registers
// allocated independently, overflow in source order with one 8-byte slot each;
// a float occupies the low 32 bits of its register or slot.
struct HostSlot {
  bool spilled, floating;
  unsigned index;  // register index, or stack slot when spilled
};
class HostSlots {
 public:
  explicit HostSlots(HostAbi abi) : abi_(abi) {}
  HostSlot next(Type t) {
    const bool floating = t == Type::Float || t == Type::Double;
    if (abi_ == HostAbi::Win64) {
      const unsigned p = position_++;
      return p < 4 ? HostSlot{false, floating, p} : HostSlot{true, floating, p - 4};
    }
    const unsigned gp_limit = abi_ == HostAbi::Aapcs64 ? 8 : 6;
    if (floating) return fp_ < 8 ? HostSlot{false, true, fp_++} : HostSlot{true, true, stack_++};
    return gp_ < gp_limit ? HostSlot{false, false, gp_++} : HostSlot{true, false, stack_++};
  }
  unsigned stack_slots() const { return abi_ == HostAbi::Win64 ? (position_ > 4 ? position_ - 4 : 0) : stack_; }

 private:
  HostAbi abi_;
  unsigned gp_ = 2, fp_ = 0, position_ = 2, stack_ = 0;
};
inline std::uint64_t host_value(const HostRegisters& r, HostSlot s) {
  return s.spilled ? r.stack[s.index] : s.floating ? r.fp[s.index] : r.gp[s.index];
}
struct NativeArguments {
  std::vector<std::uint64_t> integers, stack;
  std::vector<double> doubles;
  GuestCallArgs view() const { return {integers, doubles, stack}; }
};
// A @CriticalNative method gets only its parameters: no JNIEnv* or jclass (ART, API 26+).
NativeArguments native_arguments(const Descriptor&, const HostRegisters&, HostAbi, GuestAddr guest_env,
                                 bool critical = false);
std::uint64_t native_result(Type, const GuestCallResult&);
}  // namespace venture::java
