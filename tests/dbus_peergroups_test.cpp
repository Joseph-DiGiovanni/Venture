// D-Bus clients where getsockopt(SO_PEERGROUPS) fails ERANGE without reporting
// a size, as qemu-user's does: sd-bus would retry that call forever on connect
// (a guest sched_setattr spun in RealtimeKit's lookup), so no bus is used.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "common/platform/posix/dbus.h"

namespace fs = std::filesystem;
namespace {
std::atomic<int> unsized_failures{0};
}

// The executable exports this, so the libsystemd the clients load binds it too.
// As qemu-user's do_getsockopt: an int buffer and ERANGE without a size. After
// 64 calls it fails ENOPROTOOPT, which sd-bus accepts, so a client that retries
// fails the checks below instead of hanging.
extern "C" int getsockopt(int fd, int level, int name, void* value, socklen_t* size) {
  static const auto real = reinterpret_cast<int (*)(int, int, int, void*, socklen_t*)>(dlsym(RTLD_NEXT, "getsockopt"));
  if (level == SOL_SOCKET && name == SO_PEERGROUPS) {
    errno = unsized_failures.fetch_add(1) < 64 ? ERANGE : ENOPROTOOPT;
    return -1;
  }
  return real(fd, level, name, value, size);
}

TEST_CASE("D-Bus clients do not connect where SO_PEERGROUPS reports no size") {
  if (void* library = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL)) {
    dlclose(library);
  } else {
    MESSAGE("No libsystemd; D-Bus clients are not exercised");
    return;
  }
  // A session bus socket that accepts and drops connections: reachable, so
  // only the probe keeps sd-bus from connecting.
  const auto directory = fs::temp_directory_path() / ("venture-peergroups-" + std::to_string(getpid()));
  fs::create_directories(directory);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = (directory / "bus").string();
  REQUIRE(path.size() < sizeof address.sun_path);
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  REQUIRE(listener >= 0);
  REQUIRE(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
  REQUIRE(listen(listener, 4) == 0);
  std::atomic<bool> stop{false};
  std::atomic<int> connections{0};
  std::thread server([&] {
    while (!stop) {
      pollfd ready{listener, POLLIN, 0};
      if (poll(&ready, 1, 20) <= 0) continue;
      if (const int client = accept(listener, nullptr, nullptr); client >= 0) {
        ++connections;
        close(client);
      }
    }
  });
  setenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + path).c_str(), 1);

  CHECK_FALSE(venture::dbus::portal_min_nice());
  CHECK_FALSE(venture::dbus::portal_thread_high_priority(std::uint64_t(getpid()), 1234, -11));
  stop = true;
  server.join();
  close(listener);
  fs::remove_all(directory);
  CHECK(unsized_failures.load() == 1);  // the probe's call; sd-bus never retried
  CHECK(connections.load() == 0);
}
