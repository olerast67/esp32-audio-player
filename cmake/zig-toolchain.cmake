# SPDX-License-Identifier: Apache-2.0
# Use `zig cc` as the host C compiler (no Visual Studio or MinGW needed on Windows).
#   python -m pip install ziglang cmake ninja
#   cmake -S test -B build/test -G Ninja -DCMAKE_TOOLCHAIN_FILE=<absolute path>/cmake/zig-toolchain.cmake
# On Linux/macOS the normal system compiler is fine and this file is not needed.
if(NOT ZIG_EXE)
    execute_process(
        COMMAND python -c "import ziglang, os; print(os.path.join(os.path.dirname(ziglang.__file__), 'zig'))"
        OUTPUT_VARIABLE ZIG_EXE OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(CMAKE_HOST_WIN32 AND NOT ZIG_EXE MATCHES "\\.exe$")
    set(ZIG_EXE "${ZIG_EXE}.exe")
endif()
file(TO_CMAKE_PATH "${ZIG_EXE}" ZIG_EXE)
set(ZIG_EXE "${ZIG_EXE}" CACHE FILEPATH "zig compiler" FORCE)

set(CMAKE_C_COMPILER "${ZIG_EXE}")
set(CMAKE_C_COMPILER_ARG1 cc)
set(CMAKE_CXX_COMPILER "${ZIG_EXE}")
set(CMAKE_CXX_COMPILER_ARG1 c++)
set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/zig-ar.cmd" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.cmd" CACHE FILEPATH "" FORCE)
