#!/bin/bash
# tools/win_deps.sh for a Windows machine: what bbhost.exe links against,
# built natively in MSYS2's UCRT64 environment with cmake/msys2-ucrt64.cmake -
# zlib, SDL3 and libcurl (schannel) static, a minimal static ffmpeg for the
# movies, and the Vulkan headers and loader import library from MSYS2. Same
# versions as tools/win_deps.sh. Everything lands in build/win-deps/prefix:
#   tools/win_deps_msys2.sh               (WIN_DEPS_ROOT: elsewhere than build/win-deps)
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/msys2-ucrt64.cmake \
#         -DCMAKE_PREFIX_PATH=$(cygpath -m $PWD/build/win-deps/prefix) -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build-win
set -eu
export MSYSTEM=UCRT64
case ":$PATH:" in *:/ucrt64/bin:*) ;; *) export PATH=/ucrt64/bin:$PATH ;; esac
cd "$(dirname "$0")/.."
root=${WIN_DEPS_ROOT:-$PWD/build/win-deps}
prefix=$root/prefix
mprefix=$(cygpath -m "$prefix")
tc=$(cygpath -m "$PWD/cmake/msys2-ucrt64.cmake")
SDL_TAG=${SDL_TAG:-release-3.4.16}
ZLIB_TAG=${ZLIB_TAG:-v1.3.1}
CURL_TAG=${CURL_TAG:-curl-8_22_0}
FFMPEG_VER=${FFMPEG_VER:-7.1.1}
jobs=$(nproc)
mkdir -p "$root/src" "$prefix/include" "$prefix/lib"

fetch() {  # fetch NAME URL DIR-GLOB
  local name=$1 url=$2 dir=$3
  if ! ls -d "$root/src"/$dir > /dev/null 2>&1; then
    echo "fetching $name" >&2
    curl -sSL -m 600 -o "$root/src/$name.tar.gz" "$url"
    tar xzf "$root/src/$name.tar.gz" -C "$root/src"
  fi
  ls -d "$root/src"/$dir | head -1
}
cmake_dep() {  # cmake_dep NAME SRC ARGS...
  local name=$1 src=$2; shift 2
  cmake -S "$(cygpath -m "$src")" -B "$(cygpath -m "$root/$name")" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$mprefix" -DCMAKE_PREFIX_PATH="$mprefix" "$@" \
    > "$root/$name-configure.log" 2>&1 || { tail -30 "$root/$name-configure.log"; exit 1; }
  cmake --build "$(cygpath -m "$root/$name")" -j"$jobs" > "$root/$name-build.log" 2>&1 || { tail -30 "$root/$name-build.log"; exit 1; }
  cmake --install "$(cygpath -m "$root/$name")" > /dev/null
}

zsrc=$(fetch zlib "https://github.com/madler/zlib/archive/refs/tags/$ZLIB_TAG.tar.gz" 'zlib-*')
if [ ! -f "$prefix/lib/libzlibstatic.a" ] && [ ! -f "$prefix/lib/libz.a" ]; then
  echo "building zlib" >&2
  cmake_dep zlib "$zsrc" -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_EXAMPLES=OFF
  rm -f "$prefix/lib/libzlib.dll.a" "$prefix/lib/libz.dll.a" "$prefix/bin"/*zlib*.dll  # static only
fi

ssrc=$(fetch sdl3 "https://github.com/libsdl-org/SDL/archive/refs/tags/$SDL_TAG.tar.gz" 'SDL-*')
if [ ! -f "$prefix/lib/libSDL3.a" ]; then
  echo "building SDL3" >&2
  cmake_dep sdl3 "$ssrc" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
fi

csrc=$(fetch curl "https://github.com/curl/curl/releases/download/$CURL_TAG/curl-$(echo ${CURL_TAG#curl-} | tr _ .).tar.gz" 'curl-*')
if [ ! -f "$prefix/lib/libcurl.a" ]; then
  echo "building curl" >&2
  cmake_dep curl "$csrc" -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_SCHANNEL=ON -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF -DCURL_DISABLE_LDAP=ON -DUSE_NGHTTP2=OFF -DCURL_ZLIB=ON -DZLIB_USE_STATIC_LIBS=ON
fi

if ! ls -d "$root/src"/ffmpeg-* > /dev/null 2>&1; then
  echo "fetching ffmpeg" >&2
  curl -sSL -m 900 -o "$root/src/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"
  tar xJf "$root/src/ffmpeg.tar.xz" -C "$root/src"
fi
fsrc=$(ls -d "$root/src"/ffmpeg-* | head -1)
if [ ! -f "$prefix/lib/libavcodec.a" ]; then
  echo "building ffmpeg" >&2
  mkdir -p "$root/ffmpeg"
  (cd "$root/ffmpeg" && "$fsrc/configure" --prefix="$prefix" --target-os=mingw64 --arch=x86_64 \
     --cc=clang --cxx=clang++ --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --windres=llvm-windres \
     --ld="clang -fuse-ld=lld" \
     --enable-static --disable-shared --disable-programs --disable-doc --disable-network \
     --disable-everything --enable-decoder=h264,hevc,aac,mp3 --enable-demuxer=mov,mp3,aac --enable-parser=h264,hevc,aac \
     --enable-protocol=file --enable-swscale --enable-swresample --disable-avdevice --disable-avfilter \
     --disable-postproc --disable-debug --extra-cflags="-O2" --pkg-config=false > "$root/ffmpeg-configure.log" 2>&1 \
   && make -j"$jobs" > "$root/ffmpeg-build.log" 2>&1 && make install > "$root/ffmpeg-install.log" 2>&1) \
   || { tail -30 "$root"/ffmpeg-*.log; exit 1; }
fi

# Vulkan: MSYS2's headers and its loader's import library for vulkan-1.dll
# (the driver installs the DLL itself).
cp -r /ucrt64/include/vulkan /ucrt64/include/vk_video "$prefix/include/" 2>/dev/null || cp -r /ucrt64/include/vulkan "$prefix/include/"
cp /ucrt64/lib/libvulkan-1.dll.a "$prefix/lib/libvulkan-1.dll.a"
mkdir -p "$prefix/share/vulkan"
echo "win-deps ready in $mprefix"
