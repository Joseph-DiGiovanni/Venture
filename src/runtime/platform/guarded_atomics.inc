// Fault-contained atomics shared by the Win32 and Linux platform layers. On
// x86-64, include after defining VENTURE_A0..VENTURE_A3 as the first four
// integer argument registers of the host ABI (AT&T names, e.g. "%rcx"); the
// AArch64 variant uses the AAPCS64 argument registers x0-x3.
//
// Every routine is a frameless leaf whose only access to the target object is
// one instruction. x86 faults and AArch64 synchronous data aborts are precise,
// so a fault leaves memory and the caller's `old` untouched; the platform
// fault handler moves the PC from anywhere in [venture_guarded_begin,
// venture_guarded_fault) to venture_guarded_fault, which returns false through
// the return address still on top of the stack (x86) or in x30 (AArch64).
//
//   bool venture_guarded_add<N>(void* p, uint64_t value, uint64_t* old);     lock xadd / ldaddal
//   bool venture_guarded_xchg<N>(void* p, uint64_t value, uint64_t* old);    xchg / swpal
//   bool venture_guarded_cas<N>(void* p, uint64_t expected, uint64_t desired, uint64_t* old);
//   bool venture_guarded_load32(const void* p, uint32_t* out);
//   bool venture_guarded_read(void* dst, const void* src, size_t n);       one load for n = 1, 2, 4, 8
#if defined(__aarch64__)
// LSE acquire-release forms are sequentially consistent like x86 LOCK. CASAL
// writes the value it read to the compare register whether or not it stores,
// so `old` is correct in both cases. B/H results are zero-extended. Callers
// reach the routines through function pointers, so they begin with BTI C when
// the build enables branch protection.
#if defined(__ARM_FEATURE_BTI_DEFAULT)
#define VENTURE_GUARDED_ENTRY(name) ".globl " name "\n.type " name ", %function\n" name ":\n  hint #34\n"
#else
#define VENTURE_GUARDED_ENTRY(name) ".globl " name "\n.type " name ", %function\n" name ":\n"
#endif
#define VENTURE_GUARDED_RMW(name, insn, reg) \
  VENTURE_GUARDED_ENTRY(name) "  " insn " " reg "1, " reg "8, [x0]\n  str x8, [x2]\n  mov w0, #1\n  ret\n"
#define VENTURE_GUARDED_CAS(name, insn, reg) \
  VENTURE_GUARDED_ENTRY(name) "  mov x8, x1\n  " insn " " reg "8, " reg "2, [x0]\n  str x8, [x3]\n  mov w0, #1\n  ret\n"

asm(".text\n.p2align 4\n.globl venture_guarded_begin\nventure_guarded_begin:\n"
    VENTURE_GUARDED_RMW("venture_guarded_add1", "ldaddalb", "w")
    VENTURE_GUARDED_RMW("venture_guarded_add2", "ldaddalh", "w")
    VENTURE_GUARDED_RMW("venture_guarded_add4", "ldaddal", "w")
    VENTURE_GUARDED_RMW("venture_guarded_add8", "ldaddal", "x")
    VENTURE_GUARDED_RMW("venture_guarded_xchg1", "swpalb", "w")
    VENTURE_GUARDED_RMW("venture_guarded_xchg2", "swpalh", "w")
    VENTURE_GUARDED_RMW("venture_guarded_xchg4", "swpal", "w")
    VENTURE_GUARDED_RMW("venture_guarded_xchg8", "swpal", "x")
    VENTURE_GUARDED_CAS("venture_guarded_cas1", "casalb", "w")
    VENTURE_GUARDED_CAS("venture_guarded_cas2", "casalh", "w")
    VENTURE_GUARDED_CAS("venture_guarded_cas4", "casal", "w")
    VENTURE_GUARDED_CAS("venture_guarded_cas8", "casal", "x")
    VENTURE_GUARDED_ENTRY("venture_guarded_load32") "  ldr w8, [x0]\n  str w8, [x1]\n  mov w0, #1\n  ret\n"
    // venture_guarded_read(dst, src, n): eight-byte words, then bytes; never reads
    // outside [src, src + n). A 1, 2, 4 or 8-byte read is one load of that
    // width, so a naturally aligned guest access through a JIT callback stays
    // single-copy atomic (ARMv8 B2.2.1).
    VENTURE_GUARDED_ENTRY("venture_guarded_read")
    "  cmp x2, #4\n  b.eq 5f\n  cmp x2, #2\n  b.eq 6f\n  cmp x2, #8\n  b.lo 2f\n"
    "1:\n  ldr x8, [x1], #8\n  str x8, [x0], #8\n  sub x2, x2, #8\n  cmp x2, #8\n  b.hs 1b\n"
    "2:\n  cbz x2, 4f\n"
    "3:\n  ldrb w8, [x1], #1\n  strb w8, [x0], #1\n  subs x2, x2, #1\n  b.ne 3b\n"
    "4:\n  mov w0, #1\n  ret\n"
    "5:\n  ldr w8, [x1]\n  str w8, [x0]\n  b 4b\n"
    "6:\n  ldrh w8, [x1]\n  strh w8, [x0]\n  b 4b\n"
    ".globl venture_guarded_fault\n.type venture_guarded_fault, %function\nventure_guarded_fault:\n  mov w0, #0\n  ret\n");
#undef VENTURE_GUARDED_CAS
#undef VENTURE_GUARDED_RMW
#undef VENTURE_GUARDED_ENTRY
#else
// LOCK CMPXCHG leaves memory unchanged when the comparison fails (Intel SDM
// Vol. 2A CMPXCHG); RAX then holds the current value, so `old` is correct in
// both cases. r11 is volatile in both the Microsoft x64 and System V ABIs.
#define VENTURE_GUARDED_OP(name, body, widen, out) \
  ".globl " name "\n" name ":\n" body "\n" widen "\n  movq %rax, (" out ")\n  movl $1, %eax\n  ret\n"
#define VENTURE_GUARDED_RMW(name, insn, reg, widen) \
  VENTURE_GUARDED_OP(name, "  movq " VENTURE_A1 ", %rax\n  " insn " " reg ", (" VENTURE_A0 ")", widen, VENTURE_A2)
#define VENTURE_GUARDED_CAS(name, insn, reg, widen) \
  VENTURE_GUARDED_OP(name, "  movq " VENTURE_A2 ", %r11\n  movq " VENTURE_A1 ", %rax\n  lock " insn " " reg ", (" VENTURE_A0 ")", widen, VENTURE_A3)

asm(".text\n.p2align 4\n.globl venture_guarded_begin\nventure_guarded_begin:\n"
    VENTURE_GUARDED_RMW("venture_guarded_add1", "lock xaddb", "%al", "  movzbl %al, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_add2", "lock xaddw", "%ax", "  movzwl %ax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_add4", "lock xaddl", "%eax", "  movl %eax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_add8", "lock xaddq", "%rax", "")
    VENTURE_GUARDED_RMW("venture_guarded_xchg1", "xchgb", "%al", "  movzbl %al, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_xchg2", "xchgw", "%ax", "  movzwl %ax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_xchg4", "xchgl", "%eax", "  movl %eax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_xchg8", "xchgq", "%rax", "")
    VENTURE_GUARDED_CAS("venture_guarded_cas1", "cmpxchgb", "%r11b", "  movzbl %al, %eax")
    VENTURE_GUARDED_CAS("venture_guarded_cas2", "cmpxchgw", "%r11w", "  movzwl %ax, %eax")
    VENTURE_GUARDED_CAS("venture_guarded_cas4", "cmpxchgl", "%r11d", "  movl %eax, %eax")
    VENTURE_GUARDED_CAS("venture_guarded_cas8", "cmpxchgq", "%r11", "")
    ".globl venture_guarded_load32\nventure_guarded_load32:\n  movl (" VENTURE_A0 "), %eax\n  movl %eax, (" VENTURE_A1 ")\n"
    "  movl $1, %eax\n  ret\n"
    // venture_guarded_read(dst, src, n): eight-byte words, then bytes; never reads
    // outside [src, src + n). Uses only argument registers and rax. A 1, 2, 4
    // or 8-byte read is one load of that width, so a naturally aligned guest
    // access through a JIT callback stays single-copy atomic (ARMv8 B2.2.1).
    ".globl venture_guarded_read\nventure_guarded_read:\n"
    "  cmpq $4, " VENTURE_A2 "\n  je 5f\n  cmpq $2, " VENTURE_A2 "\n  je 6f\n"
    "  cmpq $8, " VENTURE_A2 "\n  jb 2f\n"
    "1:\n  movq (" VENTURE_A1 "), %rax\n  movq %rax, (" VENTURE_A0 ")\n  addq $8, " VENTURE_A1 "\n  addq $8, " VENTURE_A0 "\n"
    "  subq $8, " VENTURE_A2 "\n  cmpq $8, " VENTURE_A2 "\n  jae 1b\n"
    "2:\n  testq " VENTURE_A2 ", " VENTURE_A2 "\n  jz 4f\n"
    "3:\n  movb (" VENTURE_A1 "), %al\n  movb %al, (" VENTURE_A0 ")\n  incq " VENTURE_A1 "\n  incq " VENTURE_A0 "\n"
    "  decq " VENTURE_A2 "\n  jnz 3b\n"
    "4:\n  movl $1, %eax\n  ret\n"
    "5:\n  movl (" VENTURE_A1 "), %eax\n  movl %eax, (" VENTURE_A0 ")\n  jmp 4b\n"
    "6:\n  movw (" VENTURE_A1 "), %ax\n  movw %ax, (" VENTURE_A0 ")\n  jmp 4b\n"
    ".globl venture_guarded_fault\nventure_guarded_fault:\n  xorl %eax, %eax\n  ret\n");
#undef VENTURE_GUARDED_CAS
#undef VENTURE_GUARDED_RMW
#undef VENTURE_GUARDED_OP
#endif
extern "C" {
extern const char venture_guarded_begin[], venture_guarded_fault[];
bool venture_guarded_add1(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_add2(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_add4(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_add8(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg1(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg2(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg4(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg8(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas1(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas2(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas4(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas8(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_load32(const void*, std::uint32_t*);
bool venture_guarded_read(void*, const void*, std::size_t);
}

namespace venture::platform {
namespace {
bool guarded_ip(std::uintptr_t ip) {
  return ip >= reinterpret_cast<std::uintptr_t>(venture_guarded_begin) && ip < reinterpret_cast<std::uintptr_t>(venture_guarded_fault);
}
bool guarded_access_ready();  // the platform fault handler is installed
}
bool atomic_rmw(GuestAddr address, unsigned size, AtomicRmw op, std::uint64_t value,
                std::uint64_t desired, std::uint64_t& old) {
  if ((size != 1 && size != 2 && size != 4 && size != 8) || (address & (size - 1)) || !guarded_access_ready())
    return false;
  using Rmw = bool (*)(void*, std::uint64_t, std::uint64_t*);
  using Cas = bool (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
  static constexpr Rmw add[]{venture_guarded_add1, venture_guarded_add2, venture_guarded_add4, venture_guarded_add8};
  static constexpr Rmw exchange[]{venture_guarded_xchg1, venture_guarded_xchg2, venture_guarded_xchg4, venture_guarded_xchg8};
  static constexpr Cas cas[]{venture_guarded_cas1, venture_guarded_cas2, venture_guarded_cas4, venture_guarded_cas8};
  const auto index = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
  const auto mask = size == 8 ? ~0ull : (1ull << (size * 8)) - 1;
  auto* target = g2h(address);
  switch (op) {
    case AtomicRmw::add: return add[index](target, value & mask, &old);
    case AtomicRmw::exchange: return exchange[index](target, value & mask, &old);
    case AtomicRmw::compare_exchange: return cas[index](target, value & mask, desired & mask, &old);
  }
  return false;
}
bool guarded_load32(GuestAddr address, std::uint32_t& value) {
  return !(address & 3) && guarded_access_ready() && venture_guarded_load32(g2h(address), &value);
}
bool guarded_store(GuestAddr address, const void* data, std::size_t size) {
  return address + size >= address && guarded_access_ready() && venture_guarded_read(g2h(address), data, size);
}
namespace {
// A copy that reports a fault on any source byte as false, like the process
// memory system calls, without entering the kernel when it succeeds.
[[maybe_unused]] bool guarded_read(GuestAddr address, void* out, std::size_t size) {
  return address + size >= address && guarded_access_ready() && venture_guarded_read(out, g2h(address), size);
}
}
}  // namespace venture::platform
