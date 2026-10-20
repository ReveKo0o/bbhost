# Native Windows build in MSYS2's UCRT64 environment: the same configuration
# as the Linux cross build (cmake/mingw-toolchain.cmake) - Clang with lld,
# GCC's libstdc++ on winpthreads, the UCRT, everything static - built on the
# Windows machine itself. Run from a UCRT64 shell (or with /ucrt64/bin first
# on PATH), after tools/win_deps_msys2.sh:
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/msys2-ucrt64.cmake \
#         -DCMAKE_PREFIX_PATH=$PWD/build/win-deps/prefix -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build-win
# Packages: mingw-w64-ucrt-x86_64-{clang,lld,llvm,cmake,ninja,pkgconf,nasm,
# vulkan-headers,vulkan-loader}, make.
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_RC_COMPILER llvm-windres)

# -Wno-invalid-constexpr: a libstdc++ header artifact under Clang.
set(CMAKE_CXX_FLAGS_INIT "-Wno-invalid-constexpr")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static -static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libstdc++ -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS "-fuse-ld=lld -static-libstdc++ -static-libgcc" CACHE STRING "Windows module link flags")

set(MINGW_STD_LIBS "-lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 -lwinpthread")
set(CMAKE_CXX_STANDARD_LIBRARIES "${MINGW_STD_LIBS}" CACHE STRING "Windows link libraries")
set(CMAKE_C_STANDARD_LIBRARIES "${MINGW_STD_LIBS}" CACHE STRING "Windows link libraries")

# Only the dependency prefix: MSYS2's own /ucrt64 carries import libraries
# (SPIRV-Tools, SDL3, curl, zlib as DLLs) that would make the exe need DLLs
# the package does not ship.
set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
# Programs (ninja, pkg-config, windres) still come from MSYS2's bin, where
# this cmake lives.
cmake_path(GET CMAKE_COMMAND PARENT_PATH MSYS2_BIN)
list(APPEND CMAKE_PROGRAM_PATH "${MSYS2_BIN}")
find_program(CMAKE_MAKE_PROGRAM ninja PATHS "${MSYS2_BIN}" NO_DEFAULT_PATH)
set(MINGW_PKG_DIRS "")
foreach(p ${CMAKE_PREFIX_PATH})
  if(MINGW_PKG_DIRS)
    set(MINGW_PKG_DIRS "${MINGW_PKG_DIRS};${p}/lib/pkgconfig")  # a native pkgconf: Windows' separator
  else()
    set(MINGW_PKG_DIRS "${p}/lib/pkgconfig")
  endif()
endforeach()
set(ENV{PKG_CONFIG_LIBDIR} "${MINGW_PKG_DIRS}")
set(ENV{PKG_CONFIG_PATH} "")
