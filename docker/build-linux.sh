#!/usr/bin/env bash
# Runs inside the sdrpp-nrsc5-linux image (see linux.Dockerfile).
#
#   /src     the repo
#   /hdr     VOLK and FFTW headers (volk/, fftw3.h)
#   /fetch   FetchContent's download and build area, kept between runs
#   /sdrpp   SDR++ .deb packages for this architecture, unpacked, one per dir
#   /out     where hdradio_decoder.so goes
set -euo pipefail

cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_BASE_DIR=/fetch -DVOLK_INCLUDE_DIR=/hdr -DFFTW3F_INCLUDE_DIR=/hdr \
    -DHDRADIO_TEST_CORE_LIB=OFF >/tmp/configure.log 2>&1 \
    || { cat /tmp/configure.log; exit 1; }
cmake --build /tmp/build --target hdradio_decoder
strip --strip-unneeded /tmp/build/hdradio_decoder.so

cores=()
for d in /sdrpp/*/; do
    core="$(find "$d" -name libsdrpp_core.so | head -1)"
    [ -n "$core" ] && cores+=("$core")
done
[ "${#cores[@]}" -gt 0 ] || { echo "no libsdrpp_core.so under /sdrpp" >&2; exit 1; }
python3 /src/tools/elf_imports.py /tmp/build/hdradio_decoder.so "${cores[@]}"

cp /tmp/build/hdradio_decoder.so /out/
echo "built /out/hdradio_decoder.so ($(uname -m))"
