#!/bin/bash
# Cross-compiles the host-side unit tests for the Windows target (clang +
# lld against the mingw sysroot, static) and runs them under wine: the thunk
# trampolines, the TlsAlloc slot read through GS, setjmp/longjmp, and the
# guest va_list walker with its asm shims, all on Windows ABI semantics.
#   tools/win_tests.sh [OUTDIR]     (default build/win)
set -u
cd "$(dirname "$0")/.."
out=${1:-build/win}
mkdir -p "$out"
export WINEPREFIX=${WINEPREFIX:-$PWD/$out/wine} WINEDEBUG=${WINEDEBUG:--all}
cxx() {
  local exe=$1; shift
  clang++ --target=x86_64-w64-mingw32 --sysroot=/usr/x86_64-w64-mingw32 \
    -I/usr/x86_64-w64-mingw32/include/c++/16.1.0 -I/usr/x86_64-w64-mingw32/include/c++/16.1.0/x86_64-w64-mingw32 \
    -std=c++20 -Wno-invalid-constexpr -w -O1 -g -Isrc -fuse-ld=lld \
    -L/usr/x86_64-w64-mingw32/lib -L/usr/lib/gcc/x86_64-w64-mingw32/16.1.0 -static "$@" -lwinpthread -lwinmm -o "$exe"
}
fail=0
run() {
  local name=$1; shift
  if ! cxx "$out/$name.exe" "$@"; then echo "$name: build failed"; fail=1; return; fi
  if timeout 120 wine "$out/$name.exe" 2>&1 | grep -v "pci id\|libEGL\|^$"; then
    echo "$name: ok under wine"
  else
    echo "$name: FAILED under wine"; fail=1
  fi
}
run libc_abi_test tests/libc_abi_test.cpp
run libc_stdio_test tests/libc_stdio_test.cpp src/hle/sysv_va.cpp
run libc_errno_test tests/libc_errno_test.cpp
run libc_wide_test tests/libc_wide_test.cpp src/hle/sysv_va.cpp
run sysv_va_test tests/sysv_va_test.cpp src/hle/sysv_va.cpp
run thunk_args tests/thunk_args.cpp src/core/thunk.cpp src/core/tls_rewrite.cpp src/core/portable.cpp src/hle/sysv_va.cpp src/host/main_wait.cpp
wineserver -k 2>/dev/null
exit $fail
