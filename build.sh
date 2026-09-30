#!/usr/bin/env bash
# build.sh — build the HD Radio (NRSC-5) decoder module for SDR++ on every
# platform SDR++ ships for, check each against the real SDR++ releases, and
# package them.
#
# What comes out, in dist/:
#
#   sdrpp_nrsc5-linux-x86_64.zip    hdradio_decoder.so    (Ubuntu Focal / Debian Bullseye and newer)
#   sdrpp_nrsc5-linux-aarch64.zip   hdradio_decoder.so    (the same, 64-bit ARM: Raspberry Pi OS 64-bit ...)
#   sdrpp_nrsc5-macos.zip           hdradio_decoder.dylib (universal: Apple silicon and Intel)
#   sdrpp_nrsc5-windows-x64.zip     hdradio_decoder.dll
#
# One binary per platform serves both upstream SDR++ and SDR++ Community
# Edition. That is a claim about somebody else's binaries, so it is checked
# rather than assumed: every build is compared against the libraries in the
# current upstream nightly and in the pinned CE release, and refused if it
# needs a symbol either of them lacks. --test goes further and plays every
# recording in samples/ through the module inside each of those cores, checking
# that it decodes what test/samples.txt says it should.
#
# How each platform is built:
#   Linux    Docker, Ubuntu Focal (glibc 2.31), amd64 and arm64 (arm64 under emulation)
#   Windows  Docker, clang-cl against Microsoft's CRT and SDK fetched by xwin,
#            which accepts Microsoft's license terms for them
#   macOS    over ssh on $MAC_HOST, then signed with the Developer ID and
#            notarised: a downloaded, unnotarised dylib is refused by Gatekeeper
#
# Usage:
#   ./build.sh                       build everything
#   ./build.sh --only=linux,windows  build some (linux, linux-arm, mac, windows)
#   ./build.sh --test                ...and run the sample tests on each
#   ./build.sh --publish             ...and upload the zips to the $TAG release
#                                    (created if missing), replacing what is
#                                    there. Asks first.
#   ./build.sh --yes                 answer that prompt in advance
#
# Environment:
#   MAC_HOST            Mac to build and sign on (default: macbook)
#   SDRPP_CE_TAG        Community Edition release to check against (default: v1.2.5-CE)
#   RELEASE_TAG         release to publish to (default: latest)

set -euo pipefail
cd "$(dirname "$0")"
HERE="$PWD"

MAC_HOST="${MAC_HOST:-macbook}"
MAC_DIR="sdrpp-nrsc5-build"
REPO="${RELEASE_REPO:-madpsy/sdrpp_nrsc5}"
TAG="${RELEASE_TAG:-latest}"
CE_TAG="${SDRPP_CE_TAG:-v1.2.5-CE}"
KEYCHAIN_PASSWORD_FILE="${MAC_KEYCHAIN_PASSWORD_FILE:-$HOME/keys/mac-keychain.password}"
APPLE_PASSWORD_FILE="${APPLE_PASSWORD_FILE:-$HOME/keys/app.password}"
APPLE_ID_VALUE="${APPLE_ID:-nathan@nsamail.uk}"
TEAM_ID="${APPLE_TEAM_ID:-B7CM4Z8JW8}"

UPSTREAM_URL="https://github.com/AlexandreRouma/SDRPlusPlus/releases/download/nightly"
CE_URL="https://github.com/LunaeMons/SDRPlusPlus_CommunityEdition/releases/download/$CE_TAG"

# The VOLK and FFTW headers every platform compiles against. SDR++'s headers
# call VOLK kernels newer than some distributions ship headers for; the kernels
# themselves come from the VOLK each SDR++ install already has.
VOLK_DEB_URL="http://archive.ubuntu.com/ubuntu/pool/universe/v/volk/libvolk-dev_3.1.2-1.1build1_amd64.deb"
VOLK_DEB_SHA256="c69fda5639cb3b7a369a8e6016b25e778fdeb7fb6f510b4267eb6e38e3eb5ae9"
FFTW_URL="https://www.fftw.org/fftw-3.3.10.tar.gz"
FFTW_SHA256="56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467"

CACHE="$HERE/.cache"
DIST="$HERE/dist"
OUT="$HERE/build-release"

ONLY="linux,linux-arm,mac,windows"
TEST=0
PUBLISH=0
ASSUME_YES=0
for arg in "$@"; do
  case "$arg" in
    --only=*) ONLY="${arg#--only=}" ;;
    --test) TEST=1 ;;
    --publish) PUBLISH=1 ;;
    --yes) ASSUME_YES=1 ;;
    -h|--help) sed -n '2,42p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done
want() { [[ ",$ONLY," == *",$1,"* ]]; }

FAILED=()
fail() { echo "FAILED: $*" >&2; FAILED+=("$*"); }

# --- downloads -----------------------------------------------------------------------

fetch() { # url dest [sha256]
  if [ ! -s "$2" ]; then
    mkdir -p "$(dirname "$2")"
    echo "  fetching $(basename "$1")"
    curl -fsSL -o "$2.part" "$1" && mv "$2.part" "$2"
  fi
  if [ -n "${3:-}" ] && ! echo "$3  $2" | sha256sum -c - >/dev/null 2>&1; then
    rm -f "$2"
    echo "checksum mismatch: $1" >&2
    return 1
  fi
}

headers() {
  local hdr="$CACHE/hdr"
  [ -f "$hdr/fftw3.h" ] && [ -f "$hdr/volk/volk.h" ] && return 0
  fetch "$VOLK_DEB_URL" "$CACHE/dl/libvolk-dev.deb" "$VOLK_DEB_SHA256"
  fetch "$FFTW_URL" "$CACHE/dl/fftw.tar.gz" "$FFTW_SHA256"
  local t
  t="$(mktemp -d)"
  dpkg-deb -x "$CACHE/dl/libvolk-dev.deb" "$t"
  rm -rf "$hdr"; mkdir -p "$hdr"
  cp -r "$t/usr/include/volk" "$hdr/"
  tar xzf "$CACHE/dl/fftw.tar.gz" -C "$t" --wildcards '*/api/fftw3.h'
  cp "$t"/fftw-*/api/fftw3.h "$hdr/"
  rm -rf "$t"
}

# Unpacks one SDR++ release into $CACHE/<flavour>/<platform>/.
unpack_release() { # flavour url-base asset platform
  local dir="$CACHE/$1/$4"
  [ -d "$dir" ] && return 0
  fetch "$2/$3" "$CACHE/dl/$1-$3"
  mkdir -p "$dir"
  case "$3" in
    *.deb) dpkg-deb -x "$CACHE/dl/$1-$3" "$dir" ;;
    *.zip) unzip -q "$CACHE/dl/$1-$3" -d "$dir" ;;
  esac
}

# The nightly moves, so its unpacked copy is refreshed once a day.
if [ -d "$CACHE/upstream" ] && [ -n "$(find "$CACHE/upstream" -maxdepth 0 -mmin +1440)" ]; then
  rm -rf "$CACHE/upstream" "$CACHE"/dl/upstream-*
fi

# The SDR++ package built for this machine's distribution, for the Linux test:
# the core has to load here, next to this system's VOLK, FFTW and GLFW.
host_deb() {
  . /etc/os-release
  echo "sdrpp_${ID}_${VERSION_CODENAME}_amd64.deb"
}

releases() {
  local plat asset
  for plat in "$@"; do
    case "$plat" in
      linux-x86_64)  asset=sdrpp_debian_bullseye_amd64.deb ;;
      linux-aarch64) asset=sdrpp_debian_bullseye_aarch64.deb ;;
      windows)       asset=sdrpp_windows_x64.zip ;;
      mac)           asset=sdrpp_macos_arm.zip ;;
      linux-host)    asset="$(host_deb)" ;;
    esac
    unpack_release upstream "$UPSTREAM_URL" "$asset" "$plat"
    unpack_release ce "$CE_URL" "$asset" "$plat"
  done
}

# The recordings as the tests on every platform read them: xz is unpacked
# here, as wine and macOS have no xz to hand.
samples() {
  local s="$CACHE/samples" f
  mkdir -p "$s"
  for f in "$HERE"/samples/*; do
    case "$f" in
      *.xz) [ -s "$s/$(basename "${f%.xz}")" ] || xz -dc "$f" > "$s/$(basename "${f%.xz}")" ;;
      *) cp -u "$f" "$s/" ;;
    esac
  done
}

# Prints one test per line of test/samples.txt: name, recording (as unpacked
# by samples()), mode, program, expected texts; separated by \x1f.
sample_tests() {
  grep -v '^#' "$HERE/test/samples.txt" | grep -v '^$' | while IFS= read -r line; do
    IFS='|' read -r -a f <<< "$line"
    f[1]="${f[1]%.xz}"
    local IFS=$'\x1f'
    printf '%s\n' "${f[*]}"
  done
}

# --- Linux ---------------------------------------------------------------------------

build_linux() { # docker-platform arch-name
  local plat="linux-$2"
  echo
  echo "== $plat"
  releases "$plat"
  headers
  docker build -q --platform "$1" -t "sdrpp-nrsc5-linux:$2" -f docker/linux.Dockerfile docker >/dev/null
  mkdir -p "$OUT/$plat" "$CACHE/fetch-$plat"
  rm -f "$OUT/$plat/hdradio_decoder.so"
  if docker run --rm --platform "$1" \
      -v "$HERE:/src:ro" -v "$CACHE/hdr:/hdr:ro" -v "$CACHE/fetch-$plat:/fetch" \
      -v "$CACHE/upstream/$plat:/sdrpp/upstream:ro" -v "$CACHE/ce/$plat:/sdrpp/ce:ro" \
      -v "$OUT/$plat:/out" \
      "sdrpp-nrsc5-linux:$2" /src/docker/build-linux.sh 2>&1 \
      | grep -vE '^\[[0-9]+/[0-9]+\] Building C object'; then
    [ -s "$OUT/$plat/hdradio_decoder.so" ] || fail "$plat: no module produced"
  else
    fail "$plat build"
  fi
}

# --- Windows -------------------------------------------------------------------------

build_windows() {
  echo
  echo "== windows-x64"
  releases windows
  headers
  docker build -q -t sdrpp-nrsc5-win -f docker/windows.Dockerfile docker >/dev/null
  mkdir -p "$OUT/windows" "$CACHE/fetch-windows"
  rm -f "$OUT/windows/hdradio_decoder.dll" "$OUT/windows/host_test.exe"
  # Real directories, not symlinks: they are mounted into the container.
  local w="$CACHE/win-stage"
  rm -rf "$w"; mkdir -p "$w"
  cp -r "$(dirname "$(find "$CACHE/upstream/windows" -name sdrpp_core.dll | head -1)")" "$w/upstream"
  cp -r "$(dirname "$(find "$CACHE/ce/windows" -name sdrpp_core.dll | head -1)")" "$w/ce"
  if docker run --rm -e SDRPP_LINK=upstream -e TESTS="$TEST" \
      -v "$HERE:/src:ro" -v "$CACHE/hdr:/hdr:ro" -v "$CACHE/fetch-windows:/fetch" \
      -v "$w:/sdrpp:ro" -v "$OUT/windows:/out" \
      sdrpp-nrsc5-win /src/docker/build-windows.sh 2>&1 \
      | grep -vE '^\[[0-9]+/[0-9]+\] Building C object'; then
    [ -s "$OUT/windows/hdradio_decoder.dll" ] || fail "windows: no module produced"
  else
    fail "windows build"
  fi
}

# --- macOS ---------------------------------------------------------------------------

mac() {
  ssh -o BatchMode=yes -o ConnectTimeout=15 "$MAC_HOST" \
      "export PATH=/opt/homebrew/bin:\$PATH; export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer; $*" \
      2> >(grep -v "X11 forwarding request failed" >&2)
}

# The signing key sits in the login keychain, which an ssh session may only use
# once unlocked.
mac_signed() {
  printf '%s\n' "$(cat "$KEYCHAIN_PASSWORD_FILE" 2>/dev/null)" "$(cat "$APPLE_PASSWORD_FILE" 2>/dev/null)" \
  | ssh -o BatchMode=yes "$MAC_HOST" "
      export PATH=/opt/homebrew/bin:\$PATH
      export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
      IFS= read -r pw || true
      IFS= read -r apppw || true
      kc=\$HOME/Library/Keychains/login.keychain-db
      if [ -n \"\$pw\" ]; then
        security unlock-keychain -p \"\$pw\" \$kc 2>/dev/null || true
        security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k \"\$pw\" \$kc >/dev/null 2>&1 || true
      fi
      unset pw
      $*
    " 2> >(grep -v "X11 forwarding request failed" >&2)
}

# The source tree as it stands (committed or not), without build output or
# samples, plus the pinned headers.
mac_send_source() {
  headers
  mac "mkdir -p ~/$MAC_DIR && cd ~/$MAC_DIR && rm -rf src hdr && mkdir -p src \
       && ( [ -x cmake/CMake.app/Contents/bin/cmake ] || ( curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-macos-universal.tar.gz | tar xz && mv cmake-3.30.5-macos-universal cmake ) )"
  tar czf - --exclude='./build*' --exclude=./.cache --exclude=./dist --exclude=./samples --exclude=./.git -C "$HERE" . \
    | mac "cd ~/$MAC_DIR/src && tar xzf -"
  tar czf - -C "$CACHE" hdr | mac "cd ~/$MAC_DIR && tar xzf -"
}

build_mac() {
  echo
  echo "== macos (universal)"
  mkdir -p "$OUT/mac"
  rm -f "$OUT/mac/hdradio_decoder.dylib" "$OUT/mac/hdradio_decoder.dylib.gatekeeper-ok"
  if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$MAC_HOST" true 2>/dev/null; then
    fail "macos: cannot reach $MAC_HOST over ssh"
    return
  fi
  mac_send_source

  if ! mac "cd ~/$MAC_DIR && C=cmake/CMake.app/Contents/bin/cmake \
        && \$C -S src -B build -DCMAKE_BUILD_TYPE=Release '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
             -DFETCHCONTENT_BASE_DIR=\$PWD/fetch -DVOLK_INCLUDE_DIR=\$PWD/hdr -DFFTW3F_INCLUDE_DIR=\$PWD/hdr -DHDRADIO_TEST_CORE_LIB=OFF >configure.log 2>&1 \
        || { cat configure.log; exit 1; }; \
        \$C --build build --target hdradio_decoder -j8 2>&1 | grep -E 'error:|Error ' ; \
        strip -x build/hdradio_decoder.dylib && lipo -info build/hdradio_decoder.dylib"; then
    fail "macos build"
    return
  fi

  # Sign and notarise. A dylib cannot have a ticket stapled to it, so Gatekeeper
  # checks with Apple when it is first loaded; "Accepted" is what matters.
  local cert
  cert="$(mac "security find-identity -v -p codesigning 2>/dev/null | grep -m1 'Developer ID Application' | sed 's/.*\"\(.*\)\".*/\1/'" || true)"
  local verdict=""
  if [ -z "$cert" ] || [ ! -f "$APPLE_PASSWORD_FILE" ]; then
    echo "  not signed: no Developer ID certificate on $MAC_HOST, or no $APPLE_PASSWORD_FILE"
  else
    verdict="$(mac_signed "cd ~/$MAC_DIR/build \
        && codesign --force --timestamp --options runtime --sign '$cert' hdradio_decoder.dylib \
        && codesign --verify --strict hdradio_decoder.dylib \
        && rm -f notarise.zip && ditto -c -k hdradio_decoder.dylib notarise.zip \
        && xcrun notarytool submit notarise.zip --apple-id '$APPLE_ID_VALUE' --team-id '$TEAM_ID' --password \"\$apppw\" --wait 2>&1 \
           | grep -E '^\s*status:' | tail -1" || true)"
    echo "  signed with $cert; notarisation:${verdict#*status:}"
  fi
  scp -q -o BatchMode=yes "$MAC_HOST:$MAC_DIR/build/hdradio_decoder.dylib" "$OUT/mac/" 2>/dev/null \
    || { fail "macos: could not copy the dylib back"; return; }
  if [[ "$verdict" == *Accepted* ]]; then
    echo "Accepted by Apple notary service $(date -u +%FT%TZ)" > "$OUT/mac/hdradio_decoder.dylib.gatekeeper-ok"
  fi
}

# --- tests ---------------------------------------------------------------------------

# Runs every sample test as: <runner> <root dir> <recording> <mode> <program>
# <expected...>, where the runner (a command or function) supplies host_test
# and the module, and its own time limit. Reports each; fails the label if any
# test failed.
run_sample_tests() { # label samples-dir root-parent runner...
  local label="$1" sdir="$2" rootp="$3"
  shift 3
  local bad=0 name rec mode prog log line
  while IFS= read -r line; do
    IFS=$'\x1f' read -r -a a <<< "$line"
    name="${a[0]}"; rec="${a[1]}"; mode="${a[2]}"; prog="${a[3]}"
    log="$(mktemp)"
    if "$@" "$rootp/$name" "$sdir/$rec" "$mode" "$prog" "${a[@]:4}" > "$log" 2>&1 < /dev/null; then
      echo "    ok    $name"
    else
      echo "    FAIL  $name"
      grep -E "expected|sync|audio:|FAIL|rror" "$log" | sed 's/^/          /' | head -12
      bad=1
    fi
    rm -f "$log"
  done < <(sample_tests)
  [ "$bad" -eq 0 ] || fail "$label sample tests"
}

test_linux() {
  echo
  echo "== test linux-x86_64: samples in upstream and CE cores"
  # host_test links a real core and includes the core's signal path, so it is
  # built on this machine against its VOLK and FFTW, then run with each
  # release's own libraries. The module under test is the release build.
  local tb="$HERE/build-test"
  releases linux-host
  headers
  cmake -S "$HERE" -B "$tb" -G Ninja -DCMAKE_BUILD_TYPE=Release -DVOLK_INCLUDE_DIR="$CACHE/hdr" -DFFTW3F_INCLUDE_DIR="$CACHE/hdr" \
        -DHDRADIO_TEST_CORE_LIB="$(find "$CACHE/upstream/linux-host" -name libsdrpp_core.so | head -1)" >"$tb.log" 2>&1 \
    && cmake --build "$tb" --target host_test >>"$tb.log" 2>&1 \
    || { tail -20 "$tb.log"; fail "linux test build (needs libvolk-dev and libfftw3-dev)"; return; }
  samples
  local f lib root
  for f in upstream ce; do
    lib="$(dirname "$(find "$CACHE/$f/linux-host" -name libsdrpp_core.so | head -1)")"
    root="$(mktemp -d)"
    echo "  -- $f"
    run_sample_tests "linux $f" "$CACHE/samples" "$root" \
        timeout 300 env LD_LIBRARY_PATH="$lib" "$tb/host_test" "$OUT/linux-x86_64/hdradio_decoder.so"
    rm -rf "$root"
  done
}

test_windows() {
  echo
  echo "== test windows-x64 under wine: samples in upstream and CE cores"
  command -v wine >/dev/null || { echo "  skipped: wine not installed"; return; }
  [ -s "$OUT/windows/host_test.exe" ] || { fail "windows: no host_test.exe (build with --test)"; return; }
  samples
  export WINEPREFIX="$CACHE/wineprefix" WINEDEBUG=-all
  # SDR++ is built against MSVC 14.40+, whose std::mutex needs the runtime it
  # ships; wine's built-in msvcp140 is older and deadlocks in it.
  export WINEDLLOVERRIDES="msvcp140=n,b;vcruntime140=n,b;vcruntime140_1=n,b;concrt140=n,b"
  [ -d "$WINEPREFIX" ] || timeout 300 wineboot -i >/dev/null 2>&1 || true
  local f run
  for f in upstream ce; do
    run="$CACHE/wine-run/$f"
    rm -rf "$run"; mkdir -p "$run/root"
    cp -r "$CACHE/win-stage/$f/." "$run/"
    cp "$OUT/windows/hdradio_decoder.dll" "$OUT/windows/host_test.exe" "$run/"
    echo "  -- $f"
    ( cd "$run" && run_sample_tests "windows $f" "Z:$(echo "$CACHE/samples" | tr / '\\')" "Z:$(echo "$run/root" | tr / '\\')" \
        timeout 300 wine host_test.exe "Z:$(echo "$run/hdradio_decoder.dll" | tr / '\\')" )
  done
}

# One host_test run on the Mac, its arguments quoted for the remote shell.
mac_host_test() {
  timeout 300 ssh -o BatchMode=yes "$MAC_HOST" \
      "cd ~/$MAC_DIR && DYLD_LIBRARY_PATH=\$(dirname \$(find \$PWD/sdrpp-$MAC_FLAVOUR -name libsdrpp_core.dylib | head -1)) ./build-test-$MAC_FLAVOUR/host_test \$PWD/q-$MAC_FLAVOUR/hdradio_decoder.dylib $(printf '%q ' "$@")"
}

test_mac() {
  echo
  echo "== test macos: samples in upstream and CE cores"
  [ -s "$OUT/mac/hdradio_decoder.dylib" ] || { echo "  skipped: no dylib"; return; }
  samples
  ( cd "$CACHE" && tar czf - samples ) | mac "cd ~/$MAC_DIR && rm -rf samples && tar xzf -"
  local f url
  for f in upstream ce; do
    if [ "$f" = upstream ]; then url="$UPSTREAM_URL/sdrpp_macos_arm.zip"; else url="$CE_URL/sdrpp_macos_arm.zip"; fi
    echo "  -- $f"
    # The signed dylib, quarantined as a browser would leave it, so the test
    # also shows Gatekeeper letting it load.
    if ! mac "cd ~/$MAC_DIR && ( [ -d sdrpp-$f ] && [ -z \"\$(find sdrpp-$f -maxdepth 0 -mmin +1440)\" ] || ( rm -rf sdrpp-$f && curl -fsSL -o sdrpp-$f.zip '$url' && mkdir sdrpp-$f && unzip -q sdrpp-$f.zip -d sdrpp-$f ) ) \
        && F=\$(dirname \$(find \$PWD/sdrpp-$f -name libsdrpp_core.dylib | head -1)) \
        && C=cmake/CMake.app/Contents/bin/cmake \
        && \$C -S src -B build-test-$f -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_BASE_DIR=\$PWD/fetch-test \
             -DVOLK_INCLUDE_DIR=\$PWD/hdr -DFFTW3F_INCLUDE_DIR=\$PWD/hdr \
             -DHDRADIO_TEST_CORE_LIB=\$F/libsdrpp_core.dylib -DHDRADIO_TEST_VOLK=\$(ls \$F/libvolk*.dylib | head -1) >/dev/null 2>&1 \
        && \$C --build build-test-$f --target host_test -j8 >/dev/null 2>&1 \
        && rm -rf q-$f && mkdir -p q-$f/root && cp build/hdradio_decoder.dylib q-$f/ \
        && xattr -w com.apple.quarantine '0083;00000000;Safari;' q-$f/hdradio_decoder.dylib"; then
      fail "macos $f test build"
      continue
    fi
    local qd
    qd="$(mac "cd ~/$MAC_DIR && pwd")"
    MAC_FLAVOUR="$f" run_sample_tests "macos $f" "$qd/samples" "$qd/q-$f/root" mac_host_test
  done
}

# --- packaging and publishing --------------------------------------------------------

package() { # platform-dir zip-name file
  local src="$OUT/$1/$3"
  [ -s "$src" ] || return 0
  local stage
  stage="$(mktemp -d)"
  cp "$src" "$stage/"
  cp "$HERE/INSTALL.txt" "$stage/"
  rm -f "$DIST/$2"
  ( cd "$stage" && zip -q -X "$DIST/$2" "$3" INSTALL.txt )
  rm -rf "$stage"
  rm -f "$DIST/$2.gatekeeper-ok"
  if [ "$1" = mac ] && [ -f "$src.gatekeeper-ok" ]; then cp "$src.gatekeeper-ok" "$DIST/$2.gatekeeper-ok"; fi
  echo "  $2   $(du -h "$DIST/$2" | cut -f1)"
}

publish_release() {
  command -v gh >/dev/null 2>&1 || { echo "not published: gh not found" >&2; return; }
  gh auth status >/dev/null 2>&1 || { echo "not published: gh is not logged in" >&2; return; }

  local uploads=() z
  for z in "$DIST"/sdrpp_nrsc5-*.zip; do
    [ -e "$z" ] || continue
    if [[ "$z" == *macos* ]] && [ ! -f "$z.gatekeeper-ok" ]; then
      echo "  not uploading $(basename "$z"): not notarised, and Gatekeeper would refuse it"
      continue
    fi
    uploads+=("$z")
  done
  [ "${#uploads[@]}" -gt 0 ] || { echo "nothing to publish"; return; }

  local exists=1 commit
  gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1 || exists=0
  commit="$(git -C "$HERE" rev-parse --short HEAD)"
  if [ "$exists" -eq 1 ]; then
    echo "  Upload to https://github.com/$REPO/releases/tag/$TAG, replacing what is there:"
  else
    echo "  Create release '$TAG' on https://github.com/$REPO (at $commit) and upload:"
  fi
  for z in "${uploads[@]}"; do echo "      $(basename "$z")   $(du -h "$z" | cut -f1)"; done
  if [ -n "$(git -C "$HERE" status --porcelain)" ]; then
    echo "  note: the working tree has uncommitted changes; the zips were built from them."
  fi
  if [ "$ASSUME_YES" -eq 1 ]; then
    echo "  --yes given; uploading."
  elif [ ! -t 0 ]; then
    echo "not published: --publish asks first and there is no terminal. Pass --yes." >&2
    return
  else
    local reply=''
    read -r -p "  type 'yes' to upload: " reply || true
    [ "$reply" = yes ] || { echo "  not published."; return; }
  fi
  if [ "$exists" -eq 0 ]; then
    gh release create "$TAG" --repo "$REPO" --target "$(git -C "$HERE" rev-parse HEAD)" \
        --title "HD Radio decoder for SDR++ ($TAG)" \
        --notes "HD Radio (NRSC-5) decoder module for SDR++ and SDR++ Community Edition. Download the zip for your platform; INSTALL.txt inside explains where the module goes." \
      || { echo "  could not create the release" >&2; return; }
  fi
  gh release upload "$TAG" "${uploads[@]}" --clobber --repo "$REPO" && echo "  published."
}

# --- main ----------------------------------------------------------------------------

mkdir -p "$CACHE" "$DIST" "$OUT"
want linux && build_linux linux/amd64 x86_64
want linux-arm && build_linux linux/arm64 aarch64
want windows && build_windows
want mac && build_mac

if [ "$TEST" -eq 1 ]; then
  want linux && [ -s "$OUT/linux-x86_64/hdradio_decoder.so" ] && test_linux
  want windows && [ -s "$OUT/windows/hdradio_decoder.dll" ] && test_windows
  want mac && test_mac
fi

echo
echo "== packages in $DIST"
want linux && package linux-x86_64 sdrpp_nrsc5-linux-x86_64.zip hdradio_decoder.so
want linux-arm && package linux-aarch64 sdrpp_nrsc5-linux-aarch64.zip hdradio_decoder.so
want mac && package mac sdrpp_nrsc5-macos.zip hdradio_decoder.dylib
want windows && package windows sdrpp_nrsc5-windows-x64.zip hdradio_decoder.dll

if [ "${#FAILED[@]}" -gt 0 ]; then
  echo
  echo "Failures:"
  printf '  %s\n' "${FAILED[@]}"
  [ "$PUBLISH" -eq 1 ] && echo "Not publishing a build with failures."
  exit 1
fi
[ "$PUBLISH" -eq 1 ] && publish_release
exit 0
