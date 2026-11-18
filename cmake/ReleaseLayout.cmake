if(TARGET venture AND TARGET venture-runtime)
  if(NOT TARGET Python3::Interpreter)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
  endif()
  set(venture_strip "${CMAKE_STRIP}")
  if(WIN32)
    set(venture_package_name Venture-win64)
    get_filename_component(venture_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(venture_import_tools --readobj "${venture_compiler_bin}/llvm-readobj.exe")
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
      set(venture_package_name Venture-linux-aarch64)
    else()
      set(venture_package_name Venture-linux64)
    endif()
    # A cross build's binaries need a strip that reads the target's ELF; LLVM's reads every architecture.
    if(CMAKE_CROSSCOMPILING)
      get_filename_component(venture_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
      find_program(VENTURE_LLVM_STRIP llvm-strip HINTS "${venture_compiler_bin}" REQUIRED)
      set(venture_strip "${VENTURE_LLVM_STRIP}")
    endif()
    # Statically linked system libraries' notices come from the sysroot they were linked from.
    if(CMAKE_SYSROOT)
      set(venture_sysroot_args --sysroot "${CMAKE_SYSROOT}")
    endif()
  endif()
  # VENTURE_ANGLE_ROOT (src/runtime) and the target/host JDKs (src/runtime/java) of this build.
  set(venture_release_args --build "${CMAKE_BINARY_DIR}" --angle "${VENTURE_ANGLE_ROOT}" --strip "${venture_strip}"
    --jdk "${VENTURE_JAVA_HOME}" --host-jdk "${VENTURE_HOST_JAVA_HOME}" ${venture_import_tools} ${venture_sysroot_args})
  set(venture_package_command "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/package_release.py"
    --output "${PROJECT_SOURCE_DIR}/dist/${venture_package_name}" ${venture_release_args})
  add_custom_target(package-release COMMAND ${venture_package_command}
    DEPENDS venture venture-runtime VERBATIM)
  if(WIN32)
    # Venture-windows64.zip and -setup.exe from the layout (docs/WINDOWS_RELEASE.md).
    add_custom_target(package-windows COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tools/packaging/windows/build_installer.py"
      --layout "${PROJECT_SOURCE_DIR}/dist/${venture_package_name}" --output-dir "${PROJECT_SOURCE_DIR}/dist"
      ${venture_import_tools} VERBATIM)
    add_dependencies(package-windows package-release)
  endif()
  if(VENTURE_BUILD_TESTS)
    add_test(NAME release_layout COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/package_release_test.py" ${venture_release_args})
    set_tests_properties(release_layout PROPERTIES TIMEOUT 600)
  endif()
endif()
