# HD Radio (NRSC-5) decoder module for SDR++, built out of tree.
#
# The module is compiled against SDR++'s headers only. It never links a copy of
# sdrpp_core: the core, and the VOLK it was built with, are already loaded by the
# SDR++ process that opens the module, and the module's undefined symbols
# resolve against them there. That is why the headers are pinned: every struct
# the module shares with the core (VFO, SinkManager::Stream) has to have the
# layout the core was built with.
#
# The decoder is nrsc5's library, compiled here from a pinned commit and linked
# in statically, with its rtl-sdr calls stubbed out (samples come from SDR++,
# never from a dongle). Its AAC decoding needs FAAD2 with nrsc5's HDC patch,
# which no distribution ships, so that is fetched, patched and linked in
# statically too. FFTW comes from the system: sdrpp_core already depends on it.

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
# resolve at run time against the libvolk the core has already loaded.
find_library(FFTW3F_LIBRARY fftw3f REQUIRED)
find_path(FFTW3F_INCLUDE_DIR fftw3.h REQUIRED)
find_path(VOLK_INCLUDE_DIR volk/volk.h REQUIRED)

# --- libnrsc5, static -------------------------------------------------------------
include(CheckSymbolExists)
set(CMAKE_REQUIRED_FLAGS --std=gnu11)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(strndup string.h HAVE_STRNDUP)
check_symbol_exists(CMPLXF complex.h HAVE_CMPLXF)
check_symbol_exists(_Imaginary_I complex.h HAVE_IMAGINARY_I)
check_symbol_exists(_Complex_I complex.h HAVE_COMPLEX_I)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_DEFINITIONS)
set(USE_FAAD2 ON)
set(LIBRARY_DEBUG_LEVEL 5)  # nrsc5's own logging off: SDR++ has its own
configure_file(${NRSC5_SOURCE_DIR}/src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/nrsc5/config.h)

set(NRSC5_SOURCES acquire.c decode.c frame.c here_images.c input.c nrsc5.c output.c pids.c
    rtltcp.c sync.c firdecim_cf32.c conv_dec.c rs_init.c rs_decode.c unicode.c strndup.c)
list(TRANSFORM NRSC5_SOURCES PREPEND ${NRSC5_SOURCE_DIR}/src/)
add_library(nrsc5_static STATIC ${NRSC5_SOURCES} src/rtlsdr_stub.c)
target_include_directories(nrsc5_static
    PUBLIC ${NRSC5_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/nrsc5 ${NRSC5_SOURCE_DIR}/src compat/rtlsdr ${FFTW3F_INCLUDE_DIR})
string(SUBSTRING ${NRSC5_COMMIT} 0 7 _nrsc5_short)
target_compile_definitions(nrsc5_static PRIVATE _GNU_SOURCE HAVE_FAAD2 GIT_COMMIT_HASH="${_nrsc5_short}")
target_compile_options(nrsc5_static PRIVATE --std=gnu11 -O3 -fvisibility=hidden)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|i[3456]86)")
    target_compile_options(nrsc5_static PRIVATE -msse2 -msse3 -mssse3)
    target_compile_definitions(nrsc5_static PRIVATE HAVE_SSE2 HAVE_SSE3)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64)")
    target_compile_definitions(nrsc5_static PRIVATE HAVE_NEON)
endif ()
target_link_libraries(nrsc5_static PUBLIC faad_hdc ${FFTW3F_LIBRARY} m pthread)

# --- the module -------------------------------------------------------------------
add_library(hdradio_decoder SHARED src/main.cpp src/hd_decoder.cpp)
set_target_properties(hdradio_decoder PROPERTIES PREFIX "")
target_include_directories(hdradio_decoder PRIVATE
    src
    ${VOLK_INCLUDE_DIR}
    ${SDRPP_SOURCE_DIR}/core/src
    ${SDRPP_SOURCE_DIR}/core/src/imgui)
target_link_libraries(hdradio_decoder PRIVATE nrsc5_static)
# Undefined SDR++ symbols are left for the SDR++ process to satisfy; everything
# statically linked in (nrsc5, FAAD2) stays private to the module.
target_link_options(hdradio_decoder PRIVATE -Wl,--exclude-libs,ALL)

install(TARGETS hdradio_decoder LIBRARY DESTINATION lib/sdrpp/plugins)

# --- tests ----------------------------------------------------------------------
#
# resampler_test checks the fractional resampler on its own. host_test loads the
# built module into a real SDR++ core and plays nrsc5's sample capture through
# the core's signal path; live_test does the same with a real source module
# tuned to a station on the air:
#   -DHDRADIO_TEST_CORE_LIB=/usr/lib/libsdrpp_core.so
add_executable(resampler_test test/resampler_test.cpp)
target_include_directories(resampler_test PRIVATE src)

set(HDRADIO_TEST_CORE_LIB "" CACHE FILEPATH "SDR++ core library to link host_test against")
if (HDRADIO_TEST_CORE_LIB)
    find_library(HDRADIO_FOUND_VOLK volk REQUIRED)
    foreach(t host_test live_test)
        add_executable(${t} test/${t}.cpp)
        target_include_directories(${t} PRIVATE ${VOLK_INCLUDE_DIR} ${SDRPP_SOURCE_DIR}/core/src ${SDRPP_SOURCE_DIR}/core/src/imgui)
        target_link_libraries(${t} PRIVATE ${HDRADIO_TEST_CORE_LIB} ${HDRADIO_FOUND_VOLK} ${FFTW3F_LIBRARY} pthread dl)
    endforeach()
endif ()
