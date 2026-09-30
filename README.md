# HD Radio decoder for SDR++

An [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) decoder module for
HD Radio (NRSC-5), built on [nrsc5](https://github.com/theori-io/nrsc5). It puts
a VFO over an FM or AM HD Radio station, decodes it, and plays the chosen
program (HD1–HD8) through an SDR++ audio stream. The menu shows sync, MER and
BER, the programs on air, the station's name, slogan and message, alerts, and
the title and artist now playing.

It works with any SDR++ source. FM needs about 400 kHz of the source's bandwidth
around the station (the digital sidebands reach ±198 kHz); AM needs about 30 kHz.

## Build

Needs CMake ≥ 3.24, a C/C++ compiler, `patch`, and FFTW and VOLK development
headers (`libfftw3-dev libvolk-dev` on Debian/Ubuntu). Everything else is
fetched at pinned versions: SDR++'s headers, nrsc5's library, and FAAD2, which
is patched with nrsc5's HDC support. nrsc5 and FAAD2 are linked in statically,
so the module needs nothing installed beyond what SDR++ already uses.

    cmake -S . -B build
    cmake --build build -j
    sudo cmake --install build --prefix /usr     # -> /usr/lib/sdrpp/plugins/hdradio_decoder.so

Then, in SDR++, open **Module Manager**, pick `hdradio_decoder`, name the
instance (e.g. "HD Radio") and add it.

## Use

Tune the VFO onto the station's centre frequency (it snaps to 100 kHz on FM),
wait a few seconds for sync, and pick a program. The audio goes to its own
stream in the **Sinks** menu, so its volume and output are set there. Use the
**Radio** module's own VFO alongside it for the analogue signal.

## Trying it without a station

nrsc5 ships a 17-second recording of KUT (90.5 MHz, Austin). Convert it for
SDR++'s **File Source** and play it:

    tools/sample_to_wav.py <nrsc5>/support/sample.xz build
    # -> build/nrsc5_sample_90500000Hz.wav; open it in File Source

## Tests

    cmake -S . -B build -DHDRADIO_TEST_CORE_LIB=/usr/lib/libsdrpp_core.so
    cmake --build build -j
    build/resampler_test
    xz -dc <nrsc5>/support/sample.xz > sample.cu8
    build/host_test $PWD/build/hdradio_decoder.so /tmp <path>/sample.cu8 1488375 [program] [out.wav]

`host_test` loads the module into a real SDR++ core (no window), plays the
recording through the core's IQ front end and the module's VFO, and captures
the module's audio; it passes when the decoder syncs and plays.

## How it fits together

- `src/main.cpp`: the SDR++ module: VFO, menu, audio stream, config.
- `src/hd_decoder.*`: nrsc5 behind a small interface; knows nothing of SDR++.
- `src/resampler.h`: arbitrary-ratio resampler. nrsc5 wants 744187.5 Hz (FM)
  or 46511.71875 Hz (AM), which SDR++'s rational resampler cannot reach from
  common SDR rates without an enormous filter, so the VFO delivers a round
  rate and this does the rest.
- `src/rtlsdr_stub.c`, `compat/rtlsdr/`: nrsc5's library calls librtlsdr
  directly; samples here always come from SDR++, so those calls are stubbed.

## Licence

GPL-3.0, as are SDR++ and nrsc5. FAAD2 is GPL-2.0-or-later.
