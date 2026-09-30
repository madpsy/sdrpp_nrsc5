#!/usr/bin/env bash
# Runs inside the sdrpp-nrsc5-win image (see windows.Dockerfile).
#
#   /src     the repo
#   /hdr     VOLK and FFTW headers (volk/, fftw3.h)
#   /fetch   FetchContent's download and build area, kept between runs
#   /sdrpp   SDR++ Windows releases, unpacked: /sdrpp/<name>/sdrpp_core.dll ...
#            The one named by SDRPP_LINK provides the import libraries; every
#            one is checked for the symbols the module imports.
#   /out     where hdradio_decoder.dll goes (and, with TESTS=1, host_test.exe)
set -euo pipefail

LINK_FROM="${SDRPP_LINK:?set SDRPP_LINK to the release dir to link against}"
IMPLIB=/tmp/implib
mkdir -p "$IMPLIB"

# volk.dll exports its functions C++-mangled while VOLK's header declares them
# with C linkage: its import library gets C-named aliases (tools/pe_imports.py).
python3 /src/tools/pe_imports.py def "/sdrpp/$LINK_FROM/sdrpp_core.dll" "$IMPLIB/sdrpp_core.def"
python3 /src/tools/pe_imports.py def "/sdrpp/$LINK_FROM/volk.dll" "$IMPLIB/volk.def" --c-aliases
python3 /src/tools/pe_imports.py def "/sdrpp/$LINK_FROM/fftw3f.dll" "$IMPLIB/fftw3f.def"
for dll in sdrpp_core volk fftw3f; do
    llvm-dlltool -m i386:x86-64 -d "$IMPLIB/$dll.def" -l "$IMPLIB/$dll.lib" -D "$dll.dll"
done

test_args=(-DHDRADIO_TEST_CORE_LIB=OFF)
targets=(hdradio_decoder)
if [ "${TESTS:-0}" = 1 ]; then
    test_args=(-DHDRADIO_TEST_CORE_LIB="$IMPLIB/sdrpp_core.lib" -DHDRADIO_TEST_VOLK="$IMPLIB/volk.lib")
    targets+=(host_test)
fi
cmake -S /src -B /tmp/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/src/docker/windows-toolchain.cmake \
    -DFETCHCONTENT_BASE_DIR=/fetch -DVOLK_INCLUDE_DIR=/hdr -DFFTW3F_INCLUDE_DIR=/hdr \
    -DSDRPP_WINDOWS_IMPORT_DIR="$IMPLIB" "${test_args[@]}" >/tmp/configure.log 2>&1 || { cat /tmp/configure.log; exit 1; }
cmake --build /tmp/build --target "${targets[@]}"

status=0
for dll in sdrpp_core volk fftw3f; do
    rels=()
    for rel in /sdrpp/*/; do rels+=("$rel$dll.dll"); done
    python3 /src/tools/pe_imports.py check /tmp/build/hdradio_decoder.dll "$dll.dll" "${rels[@]}" || status=1
done
[ "$status" -eq 0 ] || { echo "import check failed" >&2; exit 1; }

cp /tmp/build/hdradio_decoder.dll /out/
[ "${TESTS:-0}" = 1 ] && cp /tmp/build/host_test.exe /out/
echo "built /out/hdradio_decoder.dll"
