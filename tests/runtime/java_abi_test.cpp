#include <doctest/doctest.h>

#include <bit>

#include "fake_cpu.h"
#include "java/abi.h"

using namespace venture;
using namespace venture::java;
TEST_CASE("JNI descriptors reject malformed input and preserve JVM widths") {
  auto d = Descriptor::parse("(ZBCSIJFDLjava/lang/Object;[[I)V");
  CHECK(d.parameters.size() == 10);
  CHECK(d.parameters[8] == Type::Object);
  CHECK(d.result == Type::Void);
  for (auto bad : {"", "V", "(V)V", "([V)V", "(I)", "()VV", "(L;)V", "(Ljava.lang.String;)V", "(I)Vx"})
    CHECK_THROWS_AS(Descriptor::parse(bad), std::invalid_argument);
  CHECK(Descriptor::parse("(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z").parameters.size() == 17);
}
TEST_CASE("A @CriticalNative guest native gets its parameters without JNIEnv* and jclass") {
  // Host Win64 call of (II)I and (IJFD)V: JNIEnv*, jclass, then the parameters.
  HostArguments host;
  host.gp = {0x1000, 0x2000, 7, 9, 0, 0};
  const auto normal = native_arguments(Descriptor::parse("(II)I"), host, HostAbi::Win64, 0xe000);
  CHECK(normal.integers == std::vector<std::uint64_t>{0xe000, 0x2000, 7, 9});
  const auto critical = native_arguments(Descriptor::parse("(II)I"), host, HostAbi::Win64, 0xe000, true);
  CHECK(critical.integers == std::vector<std::uint64_t>{7, 9});
  std::uint64_t spill[]{std::bit_cast<std::uint64_t>(2.5)};
  host.gp = {0x1000, 0x2000, 3, 0, 0, 0};
  host.fp[3] = std::bit_cast<std::uint32_t>(1.5f);
  host.stack = spill;
  const auto mixed = native_arguments(Descriptor::parse("(IFD)V"), host, HostAbi::Win64, 0xe000, true);
  CHECK(mixed.integers == std::vector<std::uint64_t>{3});
  REQUIRE(mixed.doubles.size() == 2);
  CHECK(std::bit_cast<std::uint64_t>(mixed.doubles[0]) == std::bit_cast<std::uint32_t>(1.5f));
  CHECK(mixed.doubles[1] == 2.5);
}
TEST_CASE("AAPCS64 JNI decoding allocates 8 GP and 8 FP registers independently, then ordered 8-byte slots") {
  // (IJIJIJ J FDFDFDFD F I D)V: x2-x7 then one GP spill; d0-d7 then FP/GP spills interleaved.
  const auto d = Descriptor::parse("(IJIJIJJFDFDFDFDFID)V");
  std::uint64_t gp[8]{0x1000, 0x2000, 0xffffffff'80000001, 2, 0x1'00000003, 4, 5, 6};
  std::uint64_t fp[8];
  for (unsigned i = 0; i < 8; ++i)
    fp[i] = i % 2 ? std::bit_cast<std::uint64_t>(i + 0.5)
                  : 0xdead'0000'0000'0000 | std::bit_cast<std::uint32_t>(i + 0.25f);  // upper bits undefined
  std::uint64_t stack[4]{7, 0xbeef'0000'0000'0000 | std::bit_cast<std::uint32_t>(8.25f), 0x1234'fffffffe,
                         std::bit_cast<std::uint64_t>(9.5)};
  const HostRegisters host(gp, fp, stack);
  const auto args = native_arguments(d, host, HostAbi::Aapcs64, 0xe000);
  CHECK(args.integers == std::vector<std::uint64_t>{0xe000, 0x2000, 0xffffffff'80000001, 2, 3, 4, 5, 6});
  REQUIRE(args.doubles.size() == 8);
  for (unsigned i = 0; i < 8; ++i)
    CHECK(std::bit_cast<std::uint64_t>(args.doubles[i]) == (i % 2 ? fp[i] : (fp[i] & 0xffffffff)));
  CHECK(args.stack == std::vector<std::uint64_t>{7, std::bit_cast<std::uint32_t>(8.25f), 0xffffffff'fffffffe,
                                                 std::bit_cast<std::uint64_t>(9.5)});
  // SysV stops at 6 GP registers: the fifth parameter already spills.
  std::uint64_t sysv_stack[5]{0x1'00000005, 6, 7, std::bit_cast<std::uint32_t>(8.25f), 0xfffffffe};
  const auto sysv = native_arguments(Descriptor::parse("(IJIJIJJFDFDFDFDFI)V"),
                                     HostRegisters(std::span<const std::uint64_t>(gp, 6), fp, sysv_stack),
                                     HostAbi::SysV, 0xe000);
  CHECK(sysv.integers == std::vector<std::uint64_t>{0xe000, 0x2000, 0xffffffff'80000001, 2, 3, 4, 5, 6});
  CHECK(sysv.stack ==
        std::vector<std::uint64_t>{7, std::bit_cast<std::uint32_t>(8.25f), 0xffffffff'fffffffe});
}
TEST_CASE("AAPCS64 @CriticalNative decoding skips JNIEnv* and jclass but not their host registers") {
  std::uint64_t gp[8]{0x1000, 0x2000, 1, 2, 3, 4, 5, 6};
  std::uint64_t fp[8]{std::bit_cast<std::uint64_t>(0.5)};
  std::uint64_t stack[3]{7, 8, 9};
  const auto args = native_arguments(Descriptor::parse("(JJJJJJDJJJ)J"), HostRegisters(gp, fp, stack),
                                     HostAbi::Aapcs64, 0xe000, true);
  CHECK(args.integers == std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6, 7, 8});
  CHECK(args.stack == std::vector<std::uint64_t>{9});
  REQUIRE(args.doubles.size() == 1);
  CHECK(args.doubles[0] == 0.5);
}
TEST_CASE("JNI mangling round-trips escaped underscores, arrays, Unicode and empty overloads") {
  std::u16string owner = u"com/example/Cl_ass\u03bb";
  std::u16string name = u"foo_bar\U0001F600";
  std::u16string params = u"J[Ljava/lang/String;[[I";
  auto n = demangle(native_name(owner, name, params));
  CHECK(n.owner == owner);
  CHECK(n.method == name);
  CHECK(n.parameters == params);
  CHECK(demangle(native_name(owner, u"method")).parameters == std::nullopt);
  CHECK(demangle(native_name(owner, u"method", u"")).parameters == u"");
  auto leading = demangle(native_name(u"p/_Cls", u"_method", u"[I"));
  CHECK(leading.owner == u"p/_Cls");
  CHECK(leading.method == u"_method");
  CHECK(leading.parameters == u"[I");
  auto short_leading = demangle(native_name(u"p/_Cls", u"_method"));
  CHECK(short_leading.method == u"_method");
  CHECK(mangle(u"a_b/[;") == "a_1b__3_2");
  CHECK(from_modified_utf8(to_modified_utf8(name)) == name);
  CHECK_THROWS(demangle("Java_bad"));
  CHECK_THROWS(unescape("_0ab"));
}
TEST_CASE("JNI variadic decoder handles independent register exhaustion and ordered mixed spills") {
  test::FakeCpu cpu;
  // 3 fixed GPR args, then 10 pairs of long/float. Float is promoted to double.
  std::uint64_t stack[32]{};
  unsigned spilled = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (i < 5)
      cpu.xs[3 + i] = 100 + i;
    else
      stack[spilled++] = 100 + i;
    auto bits = std::bit_cast<std::uint64_t>(i + 0.25);
    if (i < 8)
      cpu.vs[i].lo = bits;
    else
      stack[spilled++] = bits;
  }
  cpu.use_stack(stack);
  hle::FunctionInfo info;
  hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
  hle::ArgCursor cursor(ctx);
  for (int i = 0; i < 3; ++i) cursor.next_gpr();
  auto values = read_arguments(Descriptor::parse("(JFJFJFJFJFJFJFJFJFJF)V"), cursor, ArgumentForm::Ellipsis);
  for (unsigned i = 0; i < 10; ++i) {
    CHECK(values[2 * i].j == 100 + i);
    CHECK(values[2 * i + 1].f == doctest::Approx(i + 0.25));
  }
}
TEST_CASE("JNI va_list uses signed offsets, 16-byte FP saves and shared spill cursor") {
  test::FakeCpu cpu;
  std::uint64_t gp[2] = {11, 12},
                vr[4] = {std::bit_cast<std::uint64_t>(1.25), 0, std::bit_cast<std::uint64_t>(2.25), 0};
  std::uint64_t stack[2] = {13, std::bit_cast<std::uint64_t>(3.25)};
  hle::GuestVaListStruct list{h2g(stack), h2g(gp + 2), h2g(vr + 4), -16, -32};
  cpu.xs[0] = h2g(&list);
  hle::FunctionInfo info;
  hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
  hle::ArgCursor cursor(ctx);
  auto values = read_arguments(Descriptor::parse("(JFJFJF)V"), cursor, ArgumentForm::VaList);
  for (int i = 0; i < 3; ++i) {
    CHECK(values[i * 2].j == 11 + i);
    CHECK(values[i * 2 + 1].f == doctest::Approx(i + 1.25));
  }
  CHECK(list.gr_offs == -16);
  CHECK(list.vr_offs == -32);
}
