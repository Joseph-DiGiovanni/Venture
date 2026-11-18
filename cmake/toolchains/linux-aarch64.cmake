# Linux aarch64 release (Steam Frame / SteamOS arm64, any glibc >= 2.35 distro),
# cross-compiled with the host's clang/lld against the arm64 baseline sysroot
# from tools/linux-baseline/aarch64_sysroot.py (Ubuntu 22.04 arm64 + libc++).
# Like linux-baseline.cmake, libc++/libc++abi, libgcc and libatomic are linked
# statically and their symbols stay local.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(venture_target aarch64-linux-gnu)

if(NOT VENTURE_AARCH64_SYSROOT)
  if(DEFINED ENV{VENTURE_AARCH64_SYSROOT})
    set(VENTURE_AARCH64_SYSROOT "$ENV{VENTURE_AARCH64_SYSROOT}")
  else()
    get_filename_component(VENTURE_AARCH64_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../.deps/sysroot-aarch64" ABSOLUTE)
  endif()
endif()
if(NOT EXISTS "${VENTURE_AARCH64_SYSROOT}/usr/include/aarch64-linux-gnu")
  message(FATAL_ERROR "aarch64 sysroot missing at ${VENTURE_AARCH64_SYSROOT}: run python3 tools/linux-baseline/aarch64_sysroot.py")
endif()
# Try-compiles (and the FetchContent sub-projects) see the same sysroot.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VENTURE_AARCH64_SYSROOT)

set(CMAKE_SYSROOT "${VENTURE_AARCH64_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${venture_target})
set(CMAKE_CXX_COMPILER_TARGET ${venture_target})
set(CMAKE_ASM_COMPILER_TARGET ${venture_target})

# libc++ from the sysroot's apt.llvm.org packages, headers and archives from the same LLVM release:
# -stdlib++-isystem replaces clang's default libc++ header directory, which is the host
# compiler's own (a newer libc++ whose headers would not match the sysroot's libc++.a).
file(GLOB venture_libcxx_dirs "${VENTURE_AARCH64_SYSROOT}/usr/lib/llvm-*")
list(SORT venture_libcxx_dirs COMPARE NATURAL ORDER DESCENDING)
list(GET venture_libcxx_dirs 0 venture_libcxx)
# Snapdragon 8 Gen 3 (Steam Frame) and every Quest-class SoC implement ARMv8.2-A with LSE atomics.
set(CMAKE_C_FLAGS_INIT "-march=armv8.2-a")
set(CMAKE_CXX_FLAGS_INIT "-stdlib++-isystem ${venture_libcxx}/include/c++/v1 -march=armv8.2-a")
set(venture_aarch64_link "-stdlib=libc++ -L${venture_libcxx}/lib -static-libstdc++ -static-libgcc -fuse-ld=lld -Wl,--exclude-libs,ALL -Wl,--as-needed")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${venture_aarch64_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${venture_aarch64_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${venture_aarch64_link}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${VENTURE_AARCH64_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${VENTURE_AARCH64_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${VENTURE_AARCH64_SYSROOT}/usr/lib/pkgconfig:${VENTURE_AARCH64_SYSROOT}/usr/share/pkgconfig")
set(OPENSSL_USE_STATIC_LIBS TRUE)
# Tests and tools built for the target run under user-mode QEMU on other hosts.
find_program(VENTURE_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(VENTURE_QEMU_AARCH64 AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(CMAKE_CROSSCOMPILING_EMULATOR "${VENTURE_QEMU_AARCH64};-L;${VENTURE_AARCH64_SYSROOT}")
endif()
