// SDR++ without its window: loads the built module through the core's own
// ModuleManager, as SDR++ does at startup, and plays a recording through the
// core's IQ front end, into the module's VFO, and out of its audio stream into
// a capture sink. That exercises what a unit test cannot: that the core finds
// the module, that the VFO and sink stream it shares with the core match the
// core's layout, that the VFO's output decodes, that the menu draws, and that
// the module shuts down cleanly.
//
//   host_test <module.so> <root dir> <iq file> <rate> [program] [wav out]
//
// The IQ file is unsigned 8-bit I/Q pairs (rtl_sdr's format, and that of
// nrsc5's support/sample) at <rate>, centred on the station.
#include "harness.h"

#include <chrono>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <module> <root> <iq file> <rate> [program] [wav out]\n", argv[0]);
        return 2;
    }
    std::string modPath = argv[1], root = argv[2], iqPath = argv[3];
    double rate = atof(argv[4]);
    int program = argc > 5 ? atoi(argv[5]) : 0;
    std::string wavPath = argc > 6 ? argv[6] : "";
    const std::string inst = "HD Radio";

    {
        std::ofstream cfg(root + "/hdradio_decoder_config.json");
        cfg << "{\"" << inst << "\":{\"mode\":0,\"program\":" << program << "}}";
    }

    if (!initHeadlessCore(root)) { return 2; }

    // Unbuffered, so the feeder below is paced by how fast the chain drains.
    dsp::stream<dsp::complex_t> input;
    sigpath::iqFrontEnd.init(&input, rate, false, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFT, releaseFFT, NULL);

    auto mod = core::moduleManager.loadModule(modPath);
    if (!mod.handle) {
        printf("FAIL: module did not load\n");
        return 1;
    }
    if (core::moduleManager.createInstance(inst, "hdradio_decoder") != 0) {
        printf("FAIL: no instance\n");
        return 1;
    }
    core::moduleManager.doPostInitAll();
    menuText();

    // A source changing rate under the module, as picking another source
    // does: the core reconfigures the VFO first, then the module (on its next
    // frame) moves the VFO back onto a power-of-two ratio.
    for (double r : { 2400000.0, rate }) {
        auto c0 = std::chrono::steady_clock::now();
        sigpath::iqFrontEnd.setSampleRate(r);
        auto c1 = std::chrono::steady_clock::now();
        gui::waterfall.onFFTRedraw.emit(ImGui::WaterFall::FFTRedrawArgs{});
        auto c2 = std::chrono::steady_clock::now();
        printf("source rate -> %.0f Hz: core reconfigure %.0f ms, module follow-up %.0f ms\n", r,
               std::chrono::duration<double, std::milli>(c1 - c0).count(), std::chrono::duration<double, std::milli>(c2 - c1).count());
    }

    sigpath::iqFrontEnd.start();

    // Feed the recording, drawing the menu now and then as SDR++ would.
    FILE* f = fopen(iqPath.c_str(), "rb");
    if (!f) {
        printf("FAIL: cannot open %s\n", iqPath.c_str());
        return 1;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> raw(2 * 65536);
    long fed = 0;
    int draws = 0;
    for (;;) {
        size_t got = fread(raw.data(), 2, 65536, f);
        if (got == 0) { break; }
        for (size_t i = 0; i < got; i++) {
            input.writeBuf[i].re = (raw[2 * i] - 127.5f) / 128.0f;
            input.writeBuf[i].im = (raw[2 * i + 1] - 127.5f) / 128.0f;
        }
        if (!input.swap((int)got)) { break; }
        fed += got;
        if (fed / (long)(rate / 2) > draws) {
            draws++;
            menuText();
        }
    }
    fclose(f);
    // Let the chain drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double inSecs = fed / rate;

    std::string shown = menuText();
    printf("menu after playback:\n");
    printIndented(shown);

    long frames = audioFrames, loud = loudFrames;
    printf("fed %.1f s of IQ in %.1f s (%.1fx real time)\n", inSecs, secs, inSecs / secs);
    printf("audio: %ld frames (%.1f s at 48 kHz), %.0f%% above -40 dBFS\n", frames, frames / 48000.0,
           frames ? 100.0 * loud / frames : 0.0);

    bool synced = shown.find("Synced") != std::string::npos;
    bool ok = synced && frames > 48000 * 3 && loud > frames / 4;
    printf("sync %s\n", synced ? "yes" : "NO");

    // Tear down as SDR++ does on exit.
    core::moduleManager.deleteInstance(inst);
    if (mod.end) { mod.end(); }
    sigpath::iqFrontEnd.stop();
    printf("teardown done\n");

    if (!wavPath.empty()) {
        std::lock_guard<std::mutex> lck(wavMtx);
        writeWav(wavPath, wav, 48000);
        printf("audio written to %s\n", wavPath.c_str());
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    return ok ? 0 : 1;
}
