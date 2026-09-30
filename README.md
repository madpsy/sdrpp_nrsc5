# HD Radio decoder for SDR++

An [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) decoder module for
HD Radio (NRSC-5), built on [nrsc5](https://github.com/theori-io/nrsc5). It puts
a VFO over an FM or AM HD Radio station, decodes it, and plays the chosen
program (HD1–HD8) through an SDR++ audio stream. The menu shows sync, MER and
BER, the programs on air, the station's name, slogan and message, alerts, and
the title and artist now playing.

It works with any SDR++ source. FM needs about 400 kHz of the source's bandwidth
around the station (the digital sidebands reach ±198 kHz); AM needs about 30 kHz. Both hybrid and all-digital (MA3) AM are
supported.

## nrsc5 changes

`patches/nrsc5-ma3-timing-filter.patch` is applied to nrsc5 at build time.
nrsc5 finds AM symbol timing through a filter that passes only 10–15 kHz from
the carrier, where a hybrid station's primary sidebands are. An all-digital
(MA3) station keeps its subcarriers within ±9.5 kHz, so that filter sees
mostly noise and neighbouring channels, and a strong MA3 signal can fail to
sync (WSHE, 820 kHz, heard strongly at NA5B, never did). Once the station has
identified itself as MA3, the patch switches to a filter covering
1.5–14.7 kHz with a null at the carrier. Hybrid stations are unaffected.

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

## Samples

`samples/` holds recordings to try the module on without a station in range,
and that the tests play:

| File | Station | IQ |
|---|---|---|
| `kut_90500000Hz_1488375sps.cu8.xz` | KUT 90.5 MHz, Austin TX: FM hybrid, HD1 + HD2 (nrsc5's own sample) | 8-bit, 1488375 Hz, 17 s |
| `wshe_na5b_820000Hz_iq48.wav` | WSHE 820 kHz: AM all-digital (MA3), via the NA5B UberSDR | 16-bit, 48 kHz, lossless, 30 s |
| `wshe_na5b_820000Hz_iq12.wav` | the same station, same receiver, straight after | 16-bit, 12 kHz, lossless, 30 s |

The WAVs open directly in SDR++'s **File Source**, which reads the centre
frequency from the name; set the HD Radio mode to AM. The KUT recording needs
converting first:

    tools/sample_to_wav.py samples/kut_90500000Hz_1488375sps.cu8.xz build
    # -> build/nrsc5_sample_90500000Hz.wav

12 kHz of IQ is enough for all-digital AM: its core subcarriers sit within
±5 kHz and carry the audio on their own.

## Tests

    cmake -S . -B build && cmake --build build -j
    ctest --test-dir build --output-on-failure

`resampler` checks the resampler on its own. Every other test loads the module
into a real SDR++ core (no window), plays one of the samples through the core's
IQ front end and the module's VFO into a capture sink, and passes when the
decoder syncs, plays audio, and the menu shows what that recording is known to
carry: station name, facility, program type, titles. The core is found where
SDR++ is installed, or given with `-DHDRADIO_TEST_CORE_LIB=<libsdrpp_core>`; the
KUT test needs `xz`.

    build/host_test $PWD/build/hdradio_decoder.so <root> <recording> <FM|AM> [program] [expected text...]

runs one by hand (`HOST_TEST_WAV=out.wav` keeps the decoded audio).

`live_test` does the same with a real source module tuned to a station, and
prints what the menu shows as it changes. For an UberSDR receiver, put an
`ubersdr_source_config.json` naming it in the root dir first (`"minMargin": 0`
for lossless IQ); `IQ_DUMP=file` also saves the source's IQ:

    build/live_test $PWD/build/hdradio_decoder.so /usr/lib/sdrpp/plugins/ubersdr_source.so \
        UberSDR <root> 820000 AM 60 [program] [out.wav]

`band_scan` steps a source across MW (540-1700 kHz by default) and flags
channels whose spectrum has hybrid AM HD's sidebands: flat blocks from about
10.4 to 14.7 kHz on both sides of the carrier. It only screens; `live_test`
on a flagged channel is the proof.

    build/band_scan /usr/lib/sdrpp/plugins/ubersdr_source.so UberSDR <root> [start kHz] [stop kHz] [step kHz]

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
