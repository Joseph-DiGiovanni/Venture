#include <doctest/doctest.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <string_view>

#include "core/address_space.h"
#include "core/guest_heap.h"
#include "core/error.h"
#include "hle/libc/libc_a.h"
#include "libc_a_test_support.h"
#include "platform/memory.h"
using namespace venture;

// North Star (Unity 2022, Linux): Unity's allocator fell back to libc free with
// 0x20000000000, the first page of the guest arena, and mimalloc faulted in the
// host reading a segment header there. free, realloc and malloc_usable_size of
// memory no guest heap allocation owns leave that memory alone and return.
TEST_CASE("libc A free, realloc and malloc_usable_size skip memory no heap allocation owns") {
  test::LibcFixture f;
  // A segment-aligned page of bytes that are no mimalloc metadata, as the
  // guest arena's first page is not; mimalloc would chase them as pointers.
  constexpr std::uint64_t segment = 64ull << 20, page = 4096;
  const auto base = AddressSpace::get().allocate(segment, kProtRead | kProtWrite, "[anon:not a heap]", segment);
  REQUIRE(base);
  struct Release {
    GuestAddr p;
    ~Release() { AddressSpace::get().free(p); }
  } release{base};
  std::memset(g2h(base), 0xa5, page);
  for (const auto foreign : {base, base + 0x100}) {
    CHECK_FALSE(guest_heap::owns(g2h(foreign)));
    f.invoke("free", {foreign});
    CHECK(f.invoke("malloc_usable_size", {foreign}) == 0);
    *f.thread.errno_location() = 0;
    CHECK(f.invoke("realloc", {foreign, 64}) == 0);
    CHECK(*f.thread.errno_location() == err::kENOMEM);
  }
  const auto* bytes = g2h<unsigned char>(base);
  CHECK(std::all_of(bytes, bytes + page, [](unsigned char b) { return b == 0xa5; }));
  // Heap blocks are unaffected.
  const auto block = f.invoke("malloc", {48});
  REQUIRE(block);
  CHECK(guest_heap::owns(g2h(block)));
  CHECK(f.invoke("malloc_usable_size", {block}) >= 48);
  const auto grown = f.invoke("realloc", {block, 4096});
  REQUIRE(grown);
  CHECK(f.invoke("malloc_usable_size", {grown}) >= 4096);
  f.invoke("free", {grown});
}
// Aim XR (Unity 2019, OPS.Obfuscator): the IL2CPP metadata loader frees the
// std::string holding the metadata file's name, allocates and frees again,
// then opens the freed name. Bionic's allocators leave a freed small block's
// bytes alone until it is reused; mimalloc wrote its free-list link over the
// name's first word, so the loader opened a wrong path and no assembly loaded.
TEST_CASE("libc A free leaves a small block's bytes intact through later allocations and frees") {
  test::LibcFixture f;
  constexpr std::string_view name = "mono/2.0/DefaultWsdlHelpGenerator.aspx";
  const auto block = f.invoke("malloc", {name.size() + 1});
  REQUIRE(block);
  std::memcpy(g2h<char>(block), name.data(), name.size() + 1);
  f.invoke("free", {block});
  const auto big = f.invoke("malloc", {20000});
  REQUIRE(big);
  f.invoke("free", {big});
  const auto next = f.invoke("malloc", {name.size() + 1});
  REQUIRE(next);
  CHECK(next != block);
  CHECK(std::string_view(g2h<char>(block)) == name);
  f.invoke("free", {next});
  // Later frees release it; the heap keeps serving and freeing blocks.
  for (int i = 0; i < 64; ++i) {
    const auto p = f.invoke("malloc", {48});
    REQUIRE(p);
    f.invoke("free", {p});
  }
}
// The fault-contained copies behind HLE memcpy/memset/memcmp: a guest fault
// returns false with the faulting address and access kind instead of killing
// the process.
TEST_CASE("native memory routines contain guest faults and report the access") {
  auto& space = AddressSpace::get();
  const auto p = space.allocate(3 * kGuestPageSize, kProtRead | kProtWrite, "[native memory faults]");
  REQUIRE(p);
  const auto read_only = p + kGuestPageSize, none = p + 2 * kGuestPageSize;
  std::memset(g2h(p), 'a', 2 * kGuestPageSize);
  REQUIRE(space.protect(read_only, kGuestPageSize, kProtRead) == 0);
  REQUIRE(space.protect(none, kGuestPageSize, kProtNone) == 0);
  platform::NativeMemoryFault fault;
  CHECK(platform::native_copy(p + 64, read_only, 64, fault));
  CHECK_FALSE(platform::native_copy(read_only, p, 64, fault));
  CHECK(fault.address - read_only < 64);
  CHECK(fault.write);
  CHECK_FALSE(fault.bus);
  fault = {};
  CHECK_FALSE(platform::native_copy(p, none, 64, fault));
  CHECK(fault.address - none < 64);
  CHECK_FALSE(fault.write);
  fault = {};
  CHECK_FALSE(platform::native_fill(read_only - 32, 'b', 64, fault));
  CHECK(fault.address - (read_only - 32) < 64);
  CHECK(fault.write);
  CHECK(*g2h<char>(read_only) == 'a');
  int order = 0;
  CHECK(platform::native_compare(p, read_only, 16, order, fault));
  CHECK(order == 0);
  fault = {};
  CHECK_FALSE(platform::native_compare(p, none, 16, order, fault));
  CHECK(fault.address - none < 16);
  CHECK_FALSE(fault.write);
  // Reads of every width, including the single-load sizes, stop at the page.
  std::array<char, 24> out{};
  for (const std::size_t size : {1u, 2u, 4u, 8u, 12u, 24u}) {
    INFO(size);
    CHECK(platform::read_memory(none - size, out.data(), size));
    CHECK_FALSE(platform::read_memory(none - size + 1, out.data(), size));
  }
  space.free(p);
}
