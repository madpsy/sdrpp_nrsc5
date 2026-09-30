// The module on the air: a headless SDR++ core with a real source module
// (UberSDR by default) feeding the HD Radio module, tuned to a live station.
// Prints what the module's menu shows every few seconds and keeps the audio.
//
//   live_test <hdradio module> <source module> <source name> <root dir> <freq Hz> <FM|AM> [seconds] [program] [wav out]
//
// With IQ_DUMP=<path>, the source's IQ is also saved there (complex float32),
// to look at the spectrum when the decoder finds nothing.
// The root dir must already hold the source module's config (for UberSDR:
// ubersdr_source_config.json naming the receiver and its IQ mode).
#include "harness.h"

#include <chrono>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s <hdradio module> <source module> <source name> <root> <freq Hz> <FM|AM> [seconds] [program] [wav out]\n", argv[0]);
        return 2;
    }
    std::string modPath = argv[1], srcPath = argv[2], srcName = argv[3], root = argv[4];
    double freq = atof(argv[5]);
    int mode = std::string(argv[6]) == "AM" ? 1 : 0;
    int seconds = argc > 7 ? atoi(argv[7]) : 60;
    int program = argc > 8 ? atoi(argv[8]) : 0;
    std::string wavPath = argc > 9 ? argv[9] : "";
    const std::string inst = "HD Radio";

    {
        std::ofstream cfg(root + "/hdradio_decoder_config.json");
        cfg << "{\"" << inst << "\":{\"mode\":" << mode << ",\"program\":" << program << "}}";
    }
    if (!initHeadlessCore(root)) { return 2; }

    dsp::stream<dsp::complex_t> dummy;
    sigpath::iqFrontEnd.init(&dummy, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFT, releaseFFT, NULL);

    auto src = core::moduleManager.loadModule(srcPath);
    auto mod = core::moduleManager.loadModule(modPath);
    if (!src.handle || !mod.handle) {
        printf("FAIL: a module did not load\n");
        return 1;
    }
    std::string srcModName = src.info->name;
    if (core::moduleManager.createInstance(srcName, srcModName) != 0 || core::moduleManager.createInstance(inst, "hdradio_decoder") != 0) {
        printf("FAIL: no instance\n");
        return 1;
    }
    core::moduleManager.doPostInitAll();

    dsp::stream<dsp::complex_t> iqTap;
    dsp::sink::Handler<dsp::complex_t> iqDump;
    FILE* dumpFile = getenv("IQ_DUMP") ? fopen(getenv("IQ_DUMP"), "wb") : nullptr;
    if (dumpFile) {
        sigpath::iqFrontEnd.bindIQStream(&iqTap);
        iqDump.init(&iqTap, [](dsp::complex_t* d, int n, void* f) { fwrite(d, sizeof(dsp::complex_t), n, (FILE*)f); }, dumpFile);
        iqDump.start();
    }

    sigpath::sourceManager.selectSource(srcName);
    gui::waterfall.onFFTRedraw.emit(ImGui::WaterFall::FFTRedrawArgs{});
    sigpath::iqFrontEnd.start();
    sigpath::sourceManager.tune(freq);
    sigpath::sourceManager.start();
    printf("tuned %s to %.0f Hz, %s mode, source rate %.0f Hz\n", srcName.c_str(), freq, mode ? "AM" : "FM",
           sigpath::iqFrontEnd.getEffectiveSamplerate());

    // Frame loop: the redraw event is what lets the module follow the
    // source's rate; the menu is printed when what it says changes.
    std::string last;
    bool everSynced = false;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < seconds * 20; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gui::waterfall.onFFTRedraw.emit(ImGui::WaterFall::FFTRedrawArgs{});
        if (i % 20 != 0) { continue; }
        std::string shown = menuText();
        size_t a = shown.find("Mode");
        std::string body = a == std::string::npos ? shown : shown.substr(a);
        everSynced = everSynced || body.find("Synced") != std::string::npos;
        // Only the lines that matter to a reader, to keep the log short.
        std::string key;
        size_t p = 0;
        while (p < body.size()) {
            size_t q = body.find('\n', p);
            if (q == std::string::npos) { q = body.size(); }
            std::string line = body.substr(p, q - p);
            if (line.find("MER") == std::string::npos && line.find("Reset") == std::string::npos) { key += line + "\n"; }
            p = q + 1;
        }
        if (key != last || i % 200 == 0) {
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("t=%4.0fs  audio %.1f s\n", t, audioFrames / 48000.0);
            printIndented(body);
            last = key;
        }
    }

    long frames = audioFrames, loud = loudFrames;
    printf("audio: %ld frames (%.1f s at 48 kHz), %.0f%% above -40 dBFS; ever synced: %s\n", frames, frames / 48000.0,
           frames ? 100.0 * loud / frames : 0.0, everSynced ? "yes" : "no");

    sigpath::sourceManager.stop();
    if (dumpFile) {
        iqDump.stop();
        sigpath::iqFrontEnd.unbindIQStream(&iqTap);
        fclose(dumpFile);
    }
    core::moduleManager.deleteInstance(inst);
    core::moduleManager.deleteInstance(srcName);
    if (mod.end) { mod.end(); }
    if (src.end) { src.end(); }
    sigpath::iqFrontEnd.stop();

    if (!wavPath.empty()) {
        std::lock_guard<std::mutex> lck(wavMtx);
        writeWav(wavPath, wav, 48000);
        printf("audio written to %s\n", wavPath.c_str());
    }
    fflush(stdout);
    return everSynced ? 0 : 1;
}
