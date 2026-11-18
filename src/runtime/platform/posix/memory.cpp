#include "platform/memory.h"
#include "platform/process_info.h"
#include "core/address_space.h"
#include "core/error.h"
#include <cerrno>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <optional>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <sstream>

namespace venture::platform {
std::string first_mapping(GuestAddr address, std::uint64_t size) {
  std::FILE* maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return "unqueryable";
  char line[512];
  std::string found;
  while (std::fgets(line, sizeof(line), maps)) {
    unsigned long long begin = 0, end = 0;
    if (std::sscanf(line, "%llx-%llx", &begin, &end) == 2 && begin < address + size && end > address) {
      found = line;
      if (!found.empty() && found.back() == '\n') found.pop_back();
      break;
    }
  }
  std::fclose(maps);
  return found;
}
std::optional<std::uint64_t> committed_bytes(GuestAddr, std::uint64_t) {
  // Linux anonymous mappings use overcommit; resident pages are not commit.
  return std::nullopt;
}
bool process_debugger_attached() {
  std::ifstream input("/proc/self/status");
  for (std::string line; std::getline(input, line);)
    if (line.starts_with("TracerPid:")) return std::stoul(line.substr(10)) != 0;
  throw std::runtime_error("Cannot query process debugger state");
}
bool user_shadow_stack_active() {
#if defined(__linux__) && defined(__x86_64__)
  // arch_prctl(ARCH_SHSTK_STATUS) from Linux asm/prctl.h: bit 0 is
  // ARCH_SHSTK_SHSTK. Kernels without user shadow stacks fail the call.
  constexpr int arch_shstk_status = 0x5005;
  unsigned long features = 0;
  return syscall(SYS_arch_prctl, arch_shstk_status, &features) == 0 && (features & 1);
#else
  return false;
#endif
}
std::string mutex_owner_thread(std::recursive_mutex& m) {
#if defined(__linux__) && defined(__GLIBC__)
  // glibc records the owner's kernel TID in the mutex; a racy read is fine here.
  const int tid = m.native_handle()->__data.__owner;
  if (!tid) return "none";
  std::ifstream comm("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string name;
  std::getline(comm, name);
  return std::to_string(tid) + " (" + name + ")";
#else
  (void)m;
  return "unknown";
#endif
}
bool process_memory(ProcessMemory& out) {
  // Linux Documentation/filesystems/proc.rst: smaps_rollup Pss includes the
  // proportional share of mapped pages; Private_Clean/Dirty are unshared.
  std::ifstream input("/proc/self/smaps_rollup");
  std::uint64_t pss = 0, private_kib = 0;
  bool found = false;
  for (std::string line; std::getline(input, line);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value;
    if (!(fields >> key >> value)) continue;
    if (key == "Pss:") { pss = value; found = true; }
    if (key == "Private_Clean:" || key == "Private_Dirty:" || key == "Private_Hugetlb:")
      private_kib += value;
  }
  if (!found) return false;
  out = {private_kib, pss > private_kib ? pss - private_kib : 0};
  return true;
}
namespace {
std::optional<std::uint64_t> read_number(const char* path) {
  std::ifstream input(path);
  std::string text;
  if (!(input >> text) || text == "max") return std::nullopt;
  try { return std::stoull(text); } catch (...) { return std::nullopt; }
}
}  // namespace
bool memory_headroom(MemoryHeadroom& out) {
  std::ifstream input("/proc/meminfo");
  std::uint64_t total = 0, available = 0, commit_limit = 0, committed = 0;
  unsigned found = 0;
  for (std::string line; std::getline(input, line);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t kib;
    if (!(fields >> key >> kib)) continue;
    if (key == "MemTotal:") { total = kib << 10; found |= 1; }
    else if (key == "MemAvailable:") { available = kib << 10; found |= 2; }
    else if (key == "CommitLimit:") { commit_limit = kib << 10; found |= 4; }
    else if (key == "Committed_AS:") { committed = kib << 10; found |= 8; }
  }
  if ((found & 3) != 3 || !total) return false;
  MemoryHeadroom result{available, total};
  // vm.overcommit_memory=2 refuses allocations past CommitLimit, as Windows does.
  if (read_number("/proc/sys/vm/overcommit_memory") == 2u && (found & 12) == 12)
    result = {commit_limit > committed ? commit_limit - committed : 0, commit_limit};
  // cgroup v2 limit of this process (systemd scopes, containers): the OOM killer acts there first.
  std::ifstream cgroup("/proc/self/cgroup");
  for (std::string line; std::getline(cgroup, line);) {
    if (line.rfind("0::", 0) != 0) continue;
    const auto base = "/sys/fs/cgroup" + line.substr(3);
    const auto max = read_number((base + "/memory.max").c_str());
    const auto current = read_number((base + "/memory.current").c_str());
    if (max && current && *max < result.limit) result = {*max > *current ? std::min(result.available, *max - *current) : 0, *max};
  }
  out = result;
  return true;
}
bool gpu_engine_times(std::vector<GpuEngineTime>& out) {
  // Linux Documentation/gpu/drm-usage-stats.rst: each open DRM file reports
  // "drm-engine-<name>: <ns> ns" per client; duplicated fds share a drm-client-id.
  out.clear();
  std::vector<std::pair<std::string, std::string>> seen;  // (pdev, client id)
  auto* directory = opendir("/proc/self/fdinfo");
  if (!directory) return false;
  while (auto* entry = readdir(directory)) {
    if (entry->d_name[0] == '.') continue;
    std::ifstream file(std::string("/proc/self/fdinfo/") + entry->d_name);
    std::string line, pdev, client;
    std::vector<std::pair<std::string, std::uint64_t>> engines;
    while (std::getline(file, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      const auto key = line.substr(0, colon);
      std::istringstream value(line.substr(colon + 1));
      if (key == "drm-pdev") value >> pdev;
      else if (key == "drm-client-id") value >> client;
      else if (key.rfind("drm-engine-", 0) == 0 && key.find("capacity") == std::string::npos) {
        std::uint64_t ns = 0;
        if (value >> ns) engines.emplace_back(key.substr(11), ns);
      }
    }
    if (engines.empty() || client.empty()) continue;
    if (std::find(seen.begin(), seen.end(), std::pair{pdev, client}) != seen.end()) continue;
    seen.emplace_back(pdev, client);
    for (auto& [name, ns] : engines) {
      const auto key = pdev + " " + name;
      auto it = std::find_if(out.begin(), out.end(), [&](const GpuEngineTime& e) { return e.engine == key; });
      if (it == out.end()) out.push_back({key, ns});
      else it->ns += ns;
    }
  }
  closedir(directory);
  return !out.empty();
}
bool gpu_clock(const std::string& engine, GpuClock& out) {
  // gpu_engine_times names an engine "<drm-pdev> <engine>"; Documentation/hwmon/sysfs-interface.rst:
  // freqN_input in Hz (amdgpu: freq1 is the shader clock), tempN_input in millidegrees Celsius.
  out = {};
  const auto pdev = engine.substr(0, engine.find(' '));
  if (pdev.empty() || pdev.find('/') != std::string::npos) return false;
  const auto base = "/sys/bus/pci/devices/" + pdev + "/hwmon";
  auto* directory = opendir(base.c_str());
  if (!directory) return false;
  while (auto* entry = readdir(directory)) {
    if (std::strncmp(entry->d_name, "hwmon", 5) != 0) continue;
    const auto hwmon = base + "/" + entry->d_name;
    std::uint64_t value = 0;
    if (std::ifstream f(hwmon + "/freq1_input"); f >> value) out.hz = value;
    if (std::ifstream f(hwmon + "/temp1_input"); f >> value) out.celsius = value / 1000.0;
    if (out.hz || out.celsius > 0) break;
  }
  closedir(directory);
  return out.hz || out.celsius > 0;
}
bool commit_charge(CommitCharge& out) {
  // Linux Documentation/filesystems/proc.rst (meminfo) and mm/overcommit-accounting.rst.
  std::ifstream meminfo("/proc/meminfo");
  CommitCharge result;
  unsigned found = 0;
  for (std::string line; std::getline(meminfo, line);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value;
    if (!(fields >> key >> value)) continue;
    if (key == "CommitLimit:") { result.system_limit = value * 1024; found |= 1; }
    if (key == "Committed_AS:") { result.system_used = value * 1024; found |= 2; }
  }
  if (found != 3) return false;
  int mode = 0;
  std::ifstream("/proc/sys/vm/overcommit_memory") >> mode;
  result.enforced = mode == 2;
  std::ifstream status("/proc/self/status");
  for (std::string line; std::getline(status, line);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value;
    if (!(fields >> key >> value)) continue;
    if (key == "RssAnon:" || key == "VmSwap:") result.process += value * 1024;
  }
  out = result;
  return true;
}
bool process_resident_memory(ProcessResidentMemory& out) {
  std::ifstream input("/proc/self/status");
  ProcessResidentMemory result;
  unsigned found = 0;
  for (std::string line; std::getline(input, line);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value;
    if (!(fields >> key >> value)) continue;
    if (key == "VmRSS:") { result.resident_kib = value; found |= 1; }
    if (key == "VmHWM:") { result.peak_resident_kib = value; found |= 2; }
  }
  if (found != 3) return false;
  out = result;
  return true;
}
namespace {
#if defined(__x86_64__)
__attribute__((target("cx16")))
#endif
bool cas128(GuestAddr p, const void* value, const void* expected) {
  unsigned __int128 v, e;
  std::memcpy(&v, value, 16); std::memcpy(&e, expected, 16);
  return __sync_bool_compare_and_swap(g2h<unsigned __int128>(p), e, v);
}
int protection(std::uint32_t p) {
  return ((p & (kProtRead | kProtExec)) ? PROT_READ : 0) | ((p & kProtWrite) ? PROT_WRITE : 0);
}
#if defined(__aarch64__)
// Guest-visible addresses stay below 2^47 (core/address_space.cpp). A 48-bit-VA
// arm64 kernel places mmap(nullptr) just below 2^48, so such a host mapping
// takes a fixed address counted down from 2^47 instead. Nothing else maps
// there: the PIE image, brk, the stack and default mmaps lie above it, and the
// runtime's fixed arenas lie below kLowPlacementFloor.
constexpr std::uintptr_t kGuestVaLimit = 1ull << 47, kLowPlacementFloor = 0x200000000000ull;
std::atomic<std::uintptr_t> low_placement_top{kGuestVaLimit};
std::atomic<bool> kernel_places_high{false};
void* map_fixed_new(std::uintptr_t address, std::size_t size, int prot, int flags, int fd, off_t offset) {
  auto* p = mmap(reinterpret_cast<void*>(address), size, prot, flags | MAP_FIXED_NOREPLACE, fd, offset);
  if (p != MAP_FAILED && p != reinterpret_cast<void*>(address)) {  // a kernel without NOREPLACE
    munmap(p, size); errno = EEXIST; return MAP_FAILED;
  }
  return p;
}
// The highest hole in [kLowPlacementFloor, ceiling) that holds size bytes.
std::uintptr_t low_placement_hole(std::size_t size, std::uintptr_t ceiling) {
  std::ifstream maps("/proc/self/maps");
  std::string line;
  std::uintptr_t cursor = kLowPlacementFloor, best = 0;
  while (std::getline(maps, line) && cursor < ceiling) {
    unsigned long long start{}, end{};
    if (std::sscanf(line.c_str(), "%llx-%llx", &start, &end) != 2 || end <= cursor) continue;
    const auto stop = std::min<std::uintptr_t>(start, ceiling);
    if (stop > cursor && stop - cursor >= size) best = stop - size;
    cursor = std::max<std::uintptr_t>(cursor, end);
  }
  if (cursor < ceiling && ceiling - cursor >= size) best = ceiling - size;
  return best;
}
// mmap for memory that becomes guest-visible: like mmap(hint, ...), but the
// mapping always ends at or below 2^47.
void* map_guest_visible(std::uintptr_t hint, std::size_t size, int prot, int flags, int fd, off_t offset) {
  if (hint || !kernel_places_high.load(std::memory_order_relaxed)) {
    auto* p = mmap(reinterpret_cast<void*>(hint), size, prot, flags, fd, offset);
    if (p == MAP_FAILED || reinterpret_cast<std::uintptr_t>(p) + size <= kGuestVaLimit) return p;
    munmap(p, size);
    if (!hint) kernel_places_high.store(true, std::memory_order_relaxed);
  }
  const std::uintptr_t length = align_up(size, kGuestPageSize);
  if (!length || length > kGuestVaLimit - kLowPlacementFloor) { errno = ENOMEM; return MAP_FAILED; }
  // Top-down: just below the previous placement while that has room, else the
  // highest hole /proc/self/maps shows (unmapped views leave holes). A taken
  // or unusable candidate lowers the search ceiling below it.
  const auto top = low_placement_top.load();
  auto ceiling = top - kLowPlacementFloor >= length ? top : kGuestVaLimit;
  std::uintptr_t at = ceiling == top ? top - length : 0;
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (!at && !(at = low_placement_hole(length, ceiling))) break;
    auto* p = map_fixed_new(at, size, prot, flags, fd, offset);
    if (p != MAP_FAILED) {
      for (auto t = low_placement_top.load(); at < t && !low_placement_top.compare_exchange_weak(t, at);) {}
      return p;
    }
    if (errno != EEXIST && errno != ENOMEM) return p;
    ceiling = at;
    at = 0;
  }
  errno = ENOMEM; return MAP_FAILED;
}
#else
void* map_guest_visible(std::uintptr_t hint, std::size_t size, int prot, int flags, int fd, off_t offset) {
  return mmap(reinterpret_cast<void*>(hint), size, prot, flags, fd, offset);
}
#endif
}
// The host page size equals kGuestPageSize: pre_reserve_low_memory refuses to
// start on any other.
std::uint64_t reservation_granularity() {
  static const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
  return page;
}
GuestAddr reserve(GuestAddr address, std::uint64_t size, bool exact) {
  auto p = exact ? mmap(g2h(address), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
                 : map_guest_visible(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? 0 : h2g(p);
}
void release(GuestAddr address, std::uint64_t size) { munmap(g2h(address), size); }
bool commit(GuestAddr address, std::uint64_t size, std::uint32_t prot) { return protect(address, size, prot); }
bool decommit(GuestAddr address, std::uint64_t size) {
  return mprotect(g2h(address), size, PROT_NONE) == 0 && madvise(g2h(address), size, MADV_DONTNEED) == 0;
}
bool protect(GuestAddr address, std::uint64_t size, std::uint32_t prot) { return mprotect(g2h(address), size, protection(prot)) == 0; }
int map_shared_file(int fd, std::uint64_t offset, std::uint64_t size, std::uint32_t prot, FileMapping& out) {
  auto* view = map_guest_visible(0, size, protection(prot), MAP_SHARED, fd, static_cast<off_t>(offset));
  if (view == MAP_FAILED) {
    switch (errno) { case EACCES: return -err::kEACCES; case EBADF: return -err::kEBADF;
      case EINVAL: return -err::kEINVAL; default: return -err::kENOMEM; }
  }
  out = {h2g(view), h2g(view), size}; return 0;
}
void unmap_shared_file(const FileMapping& mapping) { munmap(g2h(mapping.base), mapping.size); }
int sync_shared_file(GuestAddr address, std::uint64_t size, bool synchronous) {
  return msync(g2h(address), size, synchronous ? MS_SYNC : MS_ASYNC) == 0 ? 0 : -err::kEIO;
}
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
namespace {
bool guarded_access_ready();
bool guarded_read(GuestAddr address, void* out, std::size_t size);
}
#endif
bool read_memory(GuestAddr address, void* out, std::size_t size) {
  if (address + size < address) return false;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  // As on Windows, a naturally aligned 1, 2, 4 or 8-byte read is one load of
  // that width (ARMv8 single-copy atomicity); process_vm_readv promises none.
  if (size && size <= 8 && !(size & (size - 1)) && !(address & (size - 1)) && guarded_access_ready())
    return guarded_read(address, out, size);
  // Other sizes are a fault-contained memcpy, like the Windows SEH copy: HLE
  // string functions read guest blocks on every call, and process_vm_readv
  // (plus getpid) cost two syscalls each.
  if (guarded_access_ready()) {
    NativeMemoryFault fault;
    return native_copy(h2g(out), address, size, fault);
  }
#endif
  iovec local{out, size}, remote{g2h(address), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
bool write_memory(GuestAddr address, const void* data, std::size_t size) {
  if (address + size < address) return false;
#if defined(__linux__) && defined(__aarch64__)
  // A fault-contained copy, like read_memory: no system call per write, and
  // qemu-user (used to test arm64 builds) has no process_vm_writev.
  if (guarded_access_ready()) return guarded_store(address, data, size);
#endif
  iovec local{const_cast<void*>(data), size}, remote{g2h(address), size};
  return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
#if !defined(__linux__) || !defined(__aarch64__)
bool memory_accessible(GuestAddr address, std::size_t size, bool write) {
  if (address + size < address) return false;
  std::ifstream maps("/proc/self/maps");
  std::string line;
  auto cursor = address;
  while (std::getline(maps, line) && cursor < address + size) {
    unsigned long long start{}, end{};
    char permissions[5]{};
    if (std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, permissions) != 3) continue;
    if (start > cursor) return false;
    if (cursor >= start && cursor < end) {
      if (permissions[0] != 'r' || (write && permissions[1] != 'w')) return false;
      cursor = end;
    }
  }
  return cursor >= address + size;
}
#endif
bool compare_exchange(GuestAddr address, const void* value, void* expected, std::size_t size) {
  if (address & (size - 1)) return false;
  // Callback path is used only after fastmem faults. Never dereference a bad address.
  if (!memory_accessible(address, size, true)) return false;
  switch (size) {
    case 1: return __atomic_compare_exchange_n(g2h<std::uint8_t>(address), static_cast<std::uint8_t*>(expected), *static_cast<const std::uint8_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 2: return __atomic_compare_exchange_n(g2h<std::uint16_t>(address), static_cast<std::uint16_t*>(expected), *static_cast<const std::uint16_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 4: return __atomic_compare_exchange_n(g2h<std::uint32_t>(address), static_cast<std::uint32_t*>(expected), *static_cast<const std::uint32_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 8: return __atomic_compare_exchange_n(g2h<std::uint64_t>(address), static_cast<std::uint64_t*>(expected), *static_cast<const std::uint64_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 16: return cas128(address, value, expected);
    default: return false;
  }
}
}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <csignal>
#include <ucontext.h>
#include <csetjmp>
#if defined(__x86_64__)
// System V AMD64 calling convention: rdi, rsi, rdx, rcx.
#define VENTURE_A0 "%rdi"
#define VENTURE_A1 "%rsi"
#define VENTURE_A2 "%rdx"
#define VENTURE_A3 "%rcx"
#endif
#include "platform/guarded_atomics.inc"

namespace venture::platform {
#include "platform/native_memory.inc"
namespace {
struct NativeMemoryScope {
  sigjmp_buf recovery; NativeMemoryCall call; NativeMemoryScope* previous;
  volatile GuestAddr fault_address{}; volatile bool fault_write{}, fault_bus{};
};
thread_local NativeMemoryScope* native_memory_scope = nullptr;
struct sigaction previous_segv{}, previous_bus{};
struct FaultAccess { bool write, execute; };
#if defined(__x86_64__)
std::optional<FaultAccess> fault_access(const mcontext_t& machine) {
  // Page-fault error code: bit 1 write, bit 4 instruction fetch.
  return FaultAccess{(machine.gregs[REG_ERR] & 2) != 0, (machine.gregs[REG_ERR] & 16) != 0};
}
#else
// The kernel's esr_context record among the __reserved signal frame records
// (arch/arm64/include/uapi/asm/sigcontext.h). <asm/sigcontext.h> conflicts
// with glibc's struct sigcontext, so the layout is read here directly:
// {u32 magic, u32 size} headers, terminated by a zero header.
std::optional<FaultAccess> fault_access(const mcontext_t& machine) {
  constexpr std::uint32_t esr_magic = 0x45535201;
  const auto* record = machine.__reserved;
  const auto* end = record + sizeof(machine.__reserved);
  while (end - record >= 8) {
    std::uint32_t magic, size;
    std::memcpy(&magic, record, 4); std::memcpy(&size, record + 4, 4);
    if (!magic || size < 8 || size > static_cast<std::size_t>(end - record)) break;
    if (magic == esr_magic && size >= 16) {
      std::uint64_t esr;
      std::memcpy(&esr, record + 8, 8);
      // EC 0x24/0x25 data abort (WnR bit 6), 0x20/0x21 instruction abort.
      switch (esr >> 26) {
        case 0x24: case 0x25: return FaultAccess{((esr >> 6) & 1) != 0, false};
        case 0x20: case 0x21: return FaultAccess{false, true};
        default: return std::nullopt;
      }
    }
    record += size;
  }
  return std::nullopt;
}
#endif
// Installed on first use, so it runs before (and chains to) handlers installed
// earlier, such as Dynarmic's fastmem handler.
void guarded_fault(int signal, siginfo_t* info, void* raw_context) {
  auto* context = static_cast<ucontext_t*>(raw_context);
#if defined(__x86_64__)
  auto& pc = context->uc_mcontext.gregs[REG_RIP];
#else
  auto& pc = context->uc_mcontext.pc;
#endif
  if (guarded_ip(static_cast<std::uintptr_t>(pc))) {
    pc = static_cast<std::remove_reference_t<decltype(pc)>>(reinterpret_cast<std::uintptr_t>(venture_guarded_fault));
    return;
  }
  if (auto* scope = native_memory_scope; scope && info->si_code > 0) {
    const auto address = h2g(info->si_addr);
    // Without an ESR record the access kind follows the declared buffer the
    // address falls in, a source read first.
    const auto access = fault_access(context->uc_mcontext)
        .value_or(FaultAccess{!native_fault_matches(scope->call, address, false), false});
    const bool write = access.write;
    if (!access.execute && native_fault_matches(scope->call, address, write)) {
      scope->fault_address = address; scope->fault_write = write;
      scope->fault_bus = signal == SIGBUS;
      // sigsetjmp(..., 0) avoids a mask-query syscall on every successful CRT
      // call. Restore the interrupted mask only on this fault path.
      sigprocmask(SIG_SETMASK, &static_cast<ucontext_t*>(raw_context)->uc_sigmask, nullptr);
      siglongjmp(scope->recovery, 1);
    }
  }
  const auto& previous = signal == SIGSEGV ? previous_segv : previous_bus;
  if (previous.sa_flags & SA_SIGINFO) return previous.sa_sigaction(signal, info, raw_context);
  if (previous.sa_handler == SIG_IGN) return;
  if (previous.sa_handler == SIG_DFL) {
    std::signal(signal, SIG_DFL);  // the faulting instruction re-executes with the default action
    return;
  }
  previous.sa_handler(signal);
}
bool guarded_access_ready() {
  static const bool ready = [] {
    struct sigaction action{};
    action.sa_sigaction = guarded_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    // Snapshot both previous actions before the handler can run: a fault on
    // another thread must never chain through an empty snapshot (SIG_DFL).
    return sigaction(SIGSEGV, nullptr, &previous_segv) == 0 && sigaction(SIGBUS, nullptr, &previous_bus) == 0 &&
           sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0;
  }();
  return ready;
}
}
bool native_memory(NativeMemoryOp op, GuestAddr destination, GuestAddr source, std::size_t size,
                   int value, std::uint64_t& result, NativeMemoryFault& fault, bool source_guest) {
  if (!guarded_access_ready()) { fault = {source, false}; return false; }
  NativeMemoryScope scope{{}, {op, destination, source, size, value}, native_memory_scope};
  scope.call.source_guest = source_guest;
  if (!native_ranges_valid(scope.call)) { fault = scope.call.fault; return false; }
  if (sigsetjmp(scope.recovery, 0)) {
    native_memory_scope = scope.previous;
    fault = {scope.fault_address, scope.fault_write, scope.fault_bus}; return false;
  }
  native_memory_scope = &scope;
  invoke_native_memory(scope.call);
  native_memory_scope = scope.previous;
  result = scope.call.result; return true;
}
#if defined(__aarch64__)
// One guarded access per page: no system call, and a fault is recognised by
// its PC, so the answer holds even where the reported fault address is not
// usable (qemu-user on an x86-64 host reports zero above 2^47). A write probe
// adds zero atomically, so a concurrent writer loses nothing.
bool memory_accessible(GuestAddr address, std::size_t size, bool write) {
  if (address + size < address || !guarded_access_ready()) return false;
  if (!size) return true;
  const auto last = align_down(address + size - 1, kGuestPageSize);
  for (auto page = align_down(address, kGuestPageSize);; page += kGuestPageSize) {
    auto* at = g2h(std::max(page, address));
    std::uint64_t old;
    std::uint8_t byte;
    if (!(write ? venture_guarded_add1(at, 0, &old) : venture_guarded_read(&byte, at, 1))) return false;
    if (page == last) return true;
  }
}
#endif
bool guarded_access_ip(std::uintptr_t ip) { return guarded_ip(ip); }
}
#else
namespace venture::platform {
bool guarded_access_ip(std::uintptr_t) { return false; }
bool native_memory(NativeMemoryOp, GuestAddr, GuestAddr source, std::size_t, int,
                   std::uint64_t&, NativeMemoryFault& fault, bool) {
  fault = {source, false}; return false;
}
// Hosts without the guarded routines check the mapping first; a racing unmap
// remains fatal there.
bool atomic_rmw(GuestAddr address, unsigned size, AtomicRmw op, std::uint64_t value,
                std::uint64_t desired, std::uint64_t& old) {
  if ((size != 1 && size != 2 && size != 4 && size != 8) || (address & (size - 1)) ||
      !memory_accessible(address, size, true))
    return false;
  const auto apply = [&]<class T>(T* target) {
    T previous{};
    switch (op) {
      case AtomicRmw::add: previous = __atomic_fetch_add(target, static_cast<T>(value), __ATOMIC_SEQ_CST); break;
      case AtomicRmw::exchange: previous = __atomic_exchange_n(target, static_cast<T>(value), __ATOMIC_SEQ_CST); break;
      case AtomicRmw::compare_exchange:
        previous = static_cast<T>(value);
        __atomic_compare_exchange_n(target, &previous, static_cast<T>(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        break;
    }
    old = previous;
    return true;
  };
  switch (size) {
    case 1: return apply(g2h<std::uint8_t>(address));
    case 2: return apply(g2h<std::uint16_t>(address));
    case 4: return apply(g2h<std::uint32_t>(address));
    default: return apply(g2h<std::uint64_t>(address));
  }
}
bool guarded_load32(GuestAddr address, std::uint32_t& value) {
  return read_memory(address, &value, sizeof(value));
}
bool guarded_store(GuestAddr address, const void* data, std::size_t size) {
  return write_memory(address, data, size);
}
}
#endif

namespace venture::platform {
bool native_copy(GuestAddr d, GuestAddr s, std::size_t n, NativeMemoryFault& f) {
  std::uint64_t result;
  return native_memory(NativeMemoryOp::copy, d, s, n, 0, result, f);
}
bool native_move(GuestAddr d, GuestAddr s, std::size_t n, NativeMemoryFault& f) {
  std::uint64_t result;
  return native_memory(NativeMemoryOp::move, d, s, n, 0, result, f);
}
bool native_fill(GuestAddr d, int v, std::size_t n, NativeMemoryFault& f) {
  std::uint64_t result;
  return native_memory(NativeMemoryOp::fill, d, 0, n, v, result, f);
}
bool native_compare(GuestAddr a, GuestAddr b, std::size_t n, int& result, NativeMemoryFault& f) {
  std::uint64_t value;
  if (!native_memory(NativeMemoryOp::compare, a, b, n, 0, value, f)) return false;
  result = static_cast<int>(value); return true;
}
}
