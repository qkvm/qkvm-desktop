#!/bin/bash
# Builds the project's own FFmpeg in WSL, from sources and a toolchain tools/build_ffmpeg.ps1 has fetched and checked:
# only the H.264, JPEG and PNG decoders, the H.264 parser, Direct3D 11 decoding and zlib, as DLLs for Windows x64.
#
#     build_ffmpeg.sh <work folder> <ffmpeg archive> <zlib archive> <toolchain .deb>...
#
# The build is made in a scratch folder of WSL's own (a Windows drive as WSL sees it is many times slower for the
# thousands of small files a build makes), and deleted afterwards; what it made is copied to <work folder>/out. The
# toolchain is unpacked there (dpkg-deb -x: nothing is installed), and FFmpeg and zlib are configured and built in
# their own trees with relative paths, so that what the DLLs carry of how they were made (their configuration line,
# their source names) says nothing of where.
set -euo pipefail

work=$1
ffmpeg_archive=$2
zlib_archive=$3
shift 3

scratch=$(mktemp -d /tmp/qkvm-ffmpeg.XXXXXX)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
mkdir -p toolchain out
for deb in "$@"; do
    dpkg-deb -x "$deb" toolchain
done
tools=$PWD/toolchain/usr
export PATH="$tools/bin:/usr/bin:/bin"
export LD_LIBRARY_PATH="$tools/lib/x86_64-linux-gnu"
unset CFLAGS CXXFLAGS LDFLAGS CPPFLAGS PKG_CONFIG_PATH
export SOURCE_DATE_EPOCH=0
host=x86_64-w64-mingw32
cc=$host-gcc-win32
# (The package's plain name for the compiler, which windres asks for as its preprocessor, is made by dpkg when it
# installs; unpacked, it is made here.)
ln -sf "$cc" "$tools/bin/$host-gcc"
"$cc" --version | head -n 1
nasm -v
make --version | head -n 1

mkdir zlib ffmpeg
tar -xzf "$zlib_archive" -C zlib --strip-components=1
tar -xJf "$ffmpeg_archive" -C ffmpeg --strip-components=1

# (configure asks the compiler of the machine it runs on whether it knows C11, and WSL brings none: the cross compiler
# answers it. With nothing but these decoders enabled, FFmpeg builds no program to run here, which the build would
# fail on.)

# zlib as a static library inside avcodec (the PNG decoder's).
(cd zlib && make -s -f win32/Makefile.gcc PREFIX=$host- CC="$cc" CFLAGS="-O2 -Wall" libz.a)

cd ffmpeg
./configure \
    --prefix=/ffmpeg \
    --target-os=mingw32 --arch=x86_64 --enable-cross-compile --cross-prefix=$host- --cc="$cc" \
    --host-cc="$cc" \
    --pkg-config=false \
    --enable-shared --disable-static \
    --enable-version3 \
    --disable-autodetect --disable-everything \
    --disable-programs --disable-doc --disable-network --disable-debug \
    --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --enable-w32threads --enable-d3d11va --enable-zlib \
    --enable-decoder=h264,mjpeg,png --enable-parser=h264 \
    --enable-hwaccel=h264_d3d11va,h264_d3d11va2 \
    --extra-cflags=-I../zlib --extra-ldflags="-L../zlib -static-libgcc"
# (Four at a time: WSL's processors are all of the PC's, and other builds may be at work on them.)
make -s -j4
make -s install DESTDIR="$scratch/out"
cp COPYING.LGPLv3 "$scratch/out/ffmpeg/LICENSE.txt"
cp ../zlib/LICENSE "$scratch/out/ffmpeg/zlib-LICENSE.txt"
# The export lists Microsoft's lib tool makes the import libraries from.
mkdir -p "$scratch/out/def"
cp libavcodec/avcodec-*.def libavutil/avutil-*.def "$scratch/out/def/"
# What each DLL needs of Windows (none of the toolchain's own DLLs may be among them).
for dll in "$scratch"/out/ffmpeg/bin/*.dll; do
    echo "$(basename "$dll"): $($host-objdump -p "$dll" | sed -n 's/^\s*DLL Name: //p' | tr '\n' ' ')"
done
rm -rf "$work/out"
cp -r "$scratch/out" "$work/out"
