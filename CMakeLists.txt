# HD Radio (NRSC-5) decoder module for SDR++, built out of tree.
#
# The module is compiled against SDR++'s headers only. It never links a copy of
# sdrpp_core: the core, and the VOLK and FFTW it was built with, are already
# loaded by the SDR++ process that opens the module, and the module's undefined
# symbols resolve against them there. That is why the headers are pinned: every
# struct the module shares with the core (VFO, SinkManager::Stream) has to have
# the layout the core was built with.
#
# Windows is the exception to "never links": a DLL's imports have to name the
# DLL they come from, so the link wants import libraries for sdrpp_core.dll,
# volk.dll and fftw3f.dll. build.sh generates them from the DLLs in an SDR++
# release and passes their directory as SDRPP_WINDOWS_IMPORT_DIR.
#
# The decoder is nrsc5's library, compiled here from a pinned commit and linked
# in statically, with its rtl-sdr and rtl_tcp calls stubbed out (samples come
# from SDR++, never from a dongle). Its AAC decoding needs FAAD2 with nrsc5's
# HDC patch, which no distribution ships, so that is fetched, patched and
# linked in statically too.

cmake_minimum_required(VERSION 3.24)
project(hdradio_decoder C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif ()

include(FetchContent)
# FetchContent_Populate is used on purpose: the SDR++ tree is only headers to
# this build, and nrsc5's own CMake (which wants rtl-sdr, libusb and autotools)
# is not used at all.
if (POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif ()

# --- SDR++ headers ------------------------------------------------------------
set(SDRPP_SOURCE_DIR "" CACHE PATH "SDR++ source tree to take headers from (default: fetch the pinned commit)")
if (NOT SDRPP_SOURCE_DIR)
    FetchContent_Declare(sdrpp_headers
        URL https://github.com/AlexandreRouma/SDRPlusPlus/archive/8c9f5ee8fe405775bfcd62c8c8f8c0fc928a64af.tar.gz
        URL_HASH SHA256=c3d884bb146957792823068cea62c78d11f33eda3d537f12b0c31484bde1eb67
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(sdrpp_headers)
    if (NOT sdrpp_headers_POPULATED)
        FetchContent_Populate(sdrpp_headers)
    endif ()
    set(SDRPP_SOURCE_DIR ${sdrpp_headers_SOURCE_DIR})
endif ()
if (NOT EXISTS "${SDRPP_SOURCE_DIR}/core/src/module.h")
    message(FATAL_ERROR "SDRPP_SOURCE_DIR (${SDRPP_SOURCE_DIR}) is not an SDR++ source tree")
endif ()

# --- nrsc5 --------------------------------------------------------------------
set(NRSC5_COMMIT 9beb2c77171472c707f285f51a1385ac4c4a8be6)
set(NRSC5_SOURCE_DIR "" CACHE PATH "nrsc5 source tree (default: fetch the pinned commit)")
if (NOT NRSC5_SOURCE_DIR)
    FetchContent_Declare(nrsc5
        URL https://github.com/theori-io/nrsc5/archive/${NRSC5_COMMIT}.tar.gz
        URL_HASH SHA256=ebcf9b0acbb72d27c53e65c474929df7283f5866939a728e48d5a7508c61f9bf
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(nrsc5)
    if (NOT nrsc5_POPULATED)
        FetchContent_Populate(nrsc5)
    endif ()
    set(NRSC5_SOURCE_DIR ${nrsc5_SOURCE_DIR})
endif ()
if (NOT EXISTS "${NRSC5_SOURCE_DIR}/src/nrsc5.c")
    message(FATAL_ERROR "NRSC5_SOURCE_DIR (${NRSC5_SOURCE_DIR}) is not an nrsc5 source tree")
endif ()

# --- FAAD2, patched for HDC -----------------------------------------------------
FetchContent_Declare(faad2
    URL https://github.com/knik0/faad2/archive/refs/tags/2.11.2.tar.gz
    URL_HASH SHA256=3fcbd305e4abd34768c62050e18ca0986f7d9c5eca343fb98275418013065c0e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(faad2)
if (NOT faad2_POPULATED)
    FetchContent_Populate(faad2)
    if (NOT EXISTS "${faad2_SOURCE_DIR}/.hdc_patched")
        find_program(PATCH_PROGRAM patch REQUIRED)
        execute_process(
            COMMAND ${PATCH_PROGRAM} -p1 -N -i "${NRSC5_SOURCE_DIR}/support/faad2-hdc-support.patch"
            WORKING_DIRECTORY "${faad2_SOURCE_DIR}"
            RESULT_VARIABLE _patch_result)
        if (NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "nrsc5's FAAD2 HDC patch did not apply")
        endif ()
        file(TOUCH "${faad2_SOURCE_DIR}/.hdc_patched")
    endif ()
    set(FAAD_BUNDLED_MODE ON CACHE BOOL "" FORCE)
    set(FAAD_BUILD_CLI OFF CACHE BOOL "" FORCE)
    add_subdirectory(${faad2_SOURCE_DIR} ${faad2_BINARY_DIR} EXCLUDE_FROM_ALL)
endif ()

# --- FFTW and VOLK (the ones sdrpp_core links) ----------------------------------
#
# The module includes SDR++'s signal path, whose DSP headers call VOLK kernels
# inline, so it needs VOLK's real (generated) header. The kernels themselves
# resolve at run time against the VOLK the core has already loaded, as nrsc5's
# FFTW calls do against the core's FFTW. Only the headers are needed to build;
# on Linux the module also records libfftw3f as a dependency, which every SDR++
# install already has.
find_path(FFTW3F_INCLUDE_DIR fftw3.h REQUIRED)
find_path(VOLK_INCLUDE_DIR volk/volk.h REQUIRED)
if (WIN32)
    set(SDRPP_WINDOWS_IMPORT_DIR "" CACHE PATH "Directory holding sdrpp_core.lib, volk.lib and fftw3f.lib import libraries")
    if (NOT SDRPP_WINDOWS_IMPORT_DIR)
        message(FATAL_ERROR "Windows builds need SDRPP_WINDOWS_IMPORT_DIR (see build.sh)")
    endif ()
    set(FFTW3F_LIBRARY ${SDRPP_WINDOWS_IMPORT_DIR}/fftw3f.lib)
elseif (APPLE)
    set(FFTW3F_LIBRARY "")  # resolved at load time (-undefined dynamic_lookup)
else ()
    find_library(FFTW3F_LIBRARY fftw3f REQUIRED)
endif ()

# --- libnrsc5, static -------------------------------------------------------------
# On Windows, nrsc5 is built with compat/win in front of the system headers:
# C99 <complex.h>, <pthread.h> and the few POSIX bits it uses (see each file).
if (WIN32)
    set(NRSC5_COMPAT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/compat/win)
    set(HAVE_CMPLXF ON)
    set(HAVE_COMPLEX_I ON)
    set(HAVE_STRNDUP OFF)
else ()
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_FLAGS --std=gnu11)
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(strndup string.h HAVE_STRNDUP)
    check_symbol_exists(CMPLXF complex.h HAVE_CMPLXF)
    check_symbol_exists(_Imaginary_I complex.h HAVE_IMAGINARY_I)
    check_symbol_exists(_Complex_I complex.h HAVE_COMPLEX_I)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_DEFINITIONS)
endif ()
set(USE_FAAD2 ON)
set(LIBRARY_DEBUG_LEVEL 5)  # nrsc5's own logging off: SDR++ has its own
configure_file(${NRSC5_SOURCE_DIR}/src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/nrsc5/config.h)

# patches/nrsc5-*.patch change nrsc5 (see each patch); they are applied to a
# copy in the build tree, so a checkout given as NRSC5_SOURCE_DIR is never
# touched.
set(NRSC5_PATCHED_DIR ${CMAKE_CURRENT_BINARY_DIR}/nrsc5-patched)
file(GLOB NRSC5_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/nrsc5-*.patch)
set(_nrsc5_patch_stamp "${NRSC5_SOURCE_DIR};${NRSC5_PATCHES}")
foreach(_p ${NRSC5_PATCHES})
    file(SHA256 ${_p} _h)
    string(APPEND _nrsc5_patch_stamp ";${_h}")
endforeach()
if (NOT EXISTS ${NRSC5_PATCHED_DIR}/.stamp OR NOT "${_nrsc5_patch_stamp}" STREQUAL "${NRSC5_PATCH_STAMP_DONE}")
    file(REMOVE_RECURSE ${NRSC5_PATCHED_DIR})
    # The whole of src/, not just the patched files: nrsc5 includes its headers
    # with quotes, which look beside the including file first.
    file(GLOB _nrsc5_src ${NRSC5_SOURCE_DIR}/src/*.c ${NRSC5_SOURCE_DIR}/src/*.h)
    file(COPY ${_nrsc5_src} DESTINATION ${NRSC5_PATCHED_DIR})
    find_program(PATCH_PROGRAM patch REQUIRED)
    foreach(_p ${NRSC5_PATCHES})
        execute_process(COMMAND ${PATCH_PROGRAM} -p2 -N -i ${_p} WORKING_DIRECTORY ${NRSC5_PATCHED_DIR} RESULT_VARIABLE _r)
        if (NOT _r EQUAL 0)
            message(FATAL_ERROR "${_p} did not apply to nrsc5")
        endif ()
    endforeach()
    file(TOUCH ${NRSC5_PATCHED_DIR}/.stamp)
    set(NRSC5_PATCH_STAMP_DONE "${_nrsc5_patch_stamp}" CACHE INTERNAL "")
endif ()

set(NRSC5_SOURCES acquire.c decode.c frame.c here_images.c input.c nrsc5.c output.c pids.c
    sync.c firdecim_cf32.c conv_dec.c rs_init.c rs_decode.c unicode.c strndup.c)
list(TRANSFORM NRSC5_SOURCES PREPEND ${NRSC5_PATCHED_DIR}/)
add_library(nrsc5_static STATIC ${NRSC5_SOURCES} src/rtlsdr_stub.c src/rtltcp_stub.c)
target_include_directories(nrsc5_static
    PUBLIC ${NRSC5_SOURCE_DIR}/include
    PRIVATE ${NRSC5_COMPAT_INCLUDE} ${NRSC5_PATCHED_DIR} ${CMAKE_CURRENT_BINARY_DIR}/nrsc5 compat/rtlsdr ${FFTW3F_INCLUDE_DIR})
string(SUBSTRING ${NRSC5_COMMIT} 0 7 _nrsc5_short)
target_compile_definitions(nrsc5_static PRIVATE _GNU_SOURCE HAVE_FAAD2 GIT_COMMIT_HASH="${_nrsc5_short}")
if (MSVC)
    # clang-cl: C11 with clang's _Complex. Complex multiply and divide call
    # __mulsc3/__divsc3, which the MSVC runtime lacks: src/win_complex_rt.c.
    target_sources(nrsc5_static PRIVATE src/win_complex_rt.c)
    target_compile_options(nrsc5_static PRIVATE /std:c11 /O2 -Wno-everything)
    target_compile_definitions(nrsc5_static PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else ()
    target_compile_options(nrsc5_static PRIVATE --std=gnu11 -O3 -fvisibility=hidden)
endif ()
# nrsc5's Viterbi decoder has SSE and NEON versions. A macOS universal build
# compiles each file once for both architectures, so there it uses the plain C
# one rather than pick an instruction set per slice.
list(LENGTH CMAKE_OSX_ARCHITECTURES _osx_arch_count)
if (_osx_arch_count GREATER 1)
    # plain C
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|i[3456]86)" OR CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    target_compile_options(nrsc5_static PRIVATE -msse2 -msse3 -mssse3)
    target_compile_definitions(nrsc5_static PRIVATE HAVE_SSE2 HAVE_SSE3)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64)" OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    target_compile_definitions(nrsc5_static PRIVATE HAVE_NEON)
endif ()
target_link_libraries(nrsc5_static PUBLIC faad_hdc ${FFTW3F_LIBRARY})
if (NOT WIN32)
    target_link_libraries(nrsc5_static PUBLIC m pthread)
endif ()

# --- the module -------------------------------------------------------------------
add_library(hdradio_decoder SHARED src/main.cpp src/hd_decoder.cpp)
set_target_properties(hdradio_decoder PROPERTIES PREFIX "")
target_include_directories(hdradio_decoder PRIVATE
    src
    ${VOLK_INCLUDE_DIR}
    ${SDRPP_SOURCE_DIR}/core/src
    ${SDRPP_SOURCE_DIR}/core/src/imgui)
target_link_libraries(hdradio_decoder PRIVATE nrsc5_static)
if (WIN32)
    target_compile_definitions(hdradio_decoder PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
    target_compile_options(hdradio_decoder PRIVATE /EHsc /utf-8)
    target_link_libraries(hdradio_decoder PRIVATE
        ${SDRPP_WINDOWS_IMPORT_DIR}/sdrpp_core.lib ${SDRPP_WINDOWS_IMPORT_DIR}/volk.lib)
elseif (APPLE)
    # SDR++ loads modules by the extension it was built with.
    set_target_properties(hdradio_decoder PROPERTIES SUFFIX ".dylib")
    target_link_options(hdradio_decoder PRIVATE -undefined dynamic_lookup)
else ()
    # Undefined SDR++ symbols are left for the SDR++ process to satisfy (build.sh
    # checks them against real cores); everything statically linked in (nrsc5,
    # FAAD2) stays private to the module.
    target_link_options(hdradio_decoder PRIVATE -Wl,--exclude-libs,ALL)
endif ()

install(TARGETS hdradio_decoder LIBRARY DESTINATION lib/sdrpp/plugins RUNTIME DESTINATION lib/sdrpp/plugins)

# --- tests ----------------------------------------------------------------------
#
#   ctest --test-dir build --output-on-failure
#
# resampler_test checks the resampler on its own. host_test loads the built
# module into a real SDR++ core, plays a recording from samples/ through the
# core's signal path and checks what the menu shows: test/samples.txt lists
# each recording and what it is known to carry. live_test does the same with a
# real source module tuned to a station on the air, and band_scan screens MW
# for hybrid AM HD; neither runs under ctest.
#
# The core-linked tests need an SDR++ core to link against, found on its own
# where SDR++ is installed, or given as -DHDRADIO_TEST_CORE_LIB=<core library>
# (with -DHDRADIO_TEST_VOLK=<volk library> where it is not found either).
enable_testing()
add_executable(resampler_test test/resampler_test.cpp)
target_include_directories(resampler_test PRIVATE src)
if (WIN32)
    target_compile_definitions(resampler_test PRIVATE _USE_MATH_DEFINES)
endif ()
add_test(NAME resampler COMMAND resampler_test)

set(HDRADIO_TEST_CORE_LIB "" CACHE FILEPATH "SDR++ core library to link host_test against (default: the installed one)")
set(HDRADIO_TEST_VOLK "" CACHE FILEPATH "VOLK library the core uses (default: found)")
if (NOT HDRADIO_TEST_CORE_LIB AND NOT WIN32)
    find_library(HDRADIO_FOUND_CORE sdrpp_core)
    if (HDRADIO_FOUND_CORE)
        set(HDRADIO_TEST_CORE_LIB ${HDRADIO_FOUND_CORE})
    endif ()
endif ()
if (HDRADIO_TEST_CORE_LIB)
    if (NOT HDRADIO_TEST_VOLK)
        find_library(HDRADIO_FOUND_VOLK volk REQUIRED)
        set(HDRADIO_TEST_VOLK ${HDRADIO_FOUND_VOLK})
    endif ()
    foreach(t host_test live_test band_scan)
        add_executable(${t} test/${t}.cpp)
        target_include_directories(${t} PRIVATE ${VOLK_INCLUDE_DIR} ${FFTW3F_INCLUDE_DIR} ${SDRPP_SOURCE_DIR}/core/src ${SDRPP_SOURCE_DIR}/core/src/imgui)
        target_link_libraries(${t} PRIVATE ${HDRADIO_TEST_CORE_LIB} ${HDRADIO_TEST_VOLK} ${FFTW3F_LIBRARY})
        if (WIN32)
            target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
            target_compile_options(${t} PRIVATE /EHsc /utf-8)
        elseif (APPLE)
            target_link_libraries(${t} PRIVATE pthread)
            target_link_options(${t} PRIVATE -undefined dynamic_lookup)
        else ()
            target_link_libraries(${t} PRIVATE pthread dl)
        endif ()
    endforeach()

    # One test per line of test/samples.txt.
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/test/samples.txt _samples REGEX "^[^#]")
    foreach(_line ${_samples})
        string(REPLACE "|" ";" _f "${_line}")
        list(POP_FRONT _f _name _rec _mode _program)
        add_test(NAME ${_name}
            COMMAND host_test $<TARGET_FILE:hdradio_decoder> ${CMAKE_CURRENT_BINARY_DIR}/test-root/${_name}
                    ${CMAKE_CURRENT_SOURCE_DIR}/samples/${_rec} ${_mode} ${_program} ${_f})
        set_tests_properties(${_name} PROPERTIES TIMEOUT 300)
    endforeach()
endif ()
