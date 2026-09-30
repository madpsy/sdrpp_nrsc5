// SDR++ without its window: loads the built module through the core's own
// ModuleManager, as SDR++ does at startup, and plays a recording through the
// core's IQ front end, into the module's VFO, and out of its audio stream into
// a capture sink. That exercises what a unit test cannot: that the core finds
// the module, that the VFO and sink stream it shares with the core match the
// core's layout, that the VFO's output decodes, that the menu draws, and that
// the module shuts down cleanly.
//
//   host_test <module.so> <root dir> <recording> <FM|AM> [program] [expected text ...]
//
// The recording is centred on the station, and is either a 16-bit stereo WAV
// (what SDR++'s recorder and File Source use; the rate comes from its header)
// or unsigned 8-bit I/Q pairs at 1488375 Hz (rtl_sdr's format, and nrsc5's),
// raw or xz-compressed. Each expected text (station name, program type, a
// title...) must appear in the module's menu once the recording has played.
// HOST_TEST_WAV=<path> keeps the decoded audio. The root dir is created if
// it does not exist.
#include "harness.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

// Reads IQ from a WAV or an 8-bit recording, as complex float at `rate`.
class Recording {
public:
    bool open(const std::string& path) {
        auto ends = [&](const char* e) { return path.size() >= strlen(e) && path.compare(path.size() - strlen(e), strlen(e), e) == 0; };
        if (ends(".wav")) {
            f = fopen(path.c_str(), "rb");
            if (!f) { return false; }
            char id[4];
            uint32_t size;
            if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) || fread(&size, 4, 1, f) != 1 || fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4)) { return false; }
            while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
                if (!memcmp(id, "fmt ", 4)) {
                    uint16_t fmt, ch, align, bits;
                    uint32_t sr, bps;
                    if (fread(&fmt, 2, 1, f) != 1 || fread(&ch, 2, 1, f) != 1 || fread(&sr, 4, 1, f) != 1 || fread(&bps, 4, 1, f) != 1 ||
                        fread(&align, 2, 1, f) != 1 || fread(&bits, 2, 1, f) != 1) { return false; }
                    if (fmt != 1 || ch != 2 || bits != 16) { return false; }
                    rate = sr;
                    fseek(f, size - 16, SEEK_CUR);
                }
                else if (!memcmp(id, "data", 4)) {
                    wav = true;
                    return rate > 0;
                }
                else {
                    fseek(f, size, SEEK_CUR);
                }
            }
            return false;
        }
        rate = 1488375;
        if (ends(".xz")) {
            f = popen(("xz -dc '" + path + "'").c_str(), "r");
            piped = true;
        }
        else {
            f = fopen(path.c_str(), "rb");
        }
        return f != nullptr;
    }

    // Up to `max` samples into `out`; 0 at the end.
    int read(dsp::complex_t* out, int max) {
        if (wav) {
            buf16.resize(2 * max);
            size_t got = fread(buf16.data(), 4, max, f);
            for (size_t i = 0; i < got; i++) { out[i] = { buf16[2 * i] / 32768.0f, buf16[2 * i + 1] / 32768.0f }; }
            return (int)got;
        }
        buf8.resize(2 * max);
        size_t got = fread(buf8.data(), 2, max, f);
        for (size_t i = 0; i < got; i++) { out[i] = { (buf8[2 * i] - 127.5f) / 128.0f, (buf8[2 * i + 1] - 127.5f) / 128.0f }; }
        return (int)got;
    }

    void close() {
        if (f) { piped ? pclose(f) : fclose(f); }
        f = nullptr;
    }

    double rate = 0;

private:
    FILE* f = nullptr;
    bool wav = false, piped = false;
    std::vector<int16_t> buf16;
    std::vector<uint8_t> buf8;
};

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <module> <root> <recording> <FM|AM> [program] [expected text ...]\n", argv[0]);
        return 2;
    }
    std::string modPath = argv[1], root = argv[2], iqPath = argv[3];
    int mode = std::string(argv[4]) == "AM" ? 1 : 0;
    int program = argc > 5 ? atoi(argv[5]) : 0;
    std::vector<std::string> expected(argv + std::min(argc, 6), argv + argc);
    std::string wavPath = getenv("HOST_TEST_WAV") ? getenv("HOST_TEST_WAV") : "";
    std::filesystem::create_directories(root);
    const std::string inst = "HD Radio";

    Recording rec;
    if (!rec.open(iqPath)) {
        printf("FAIL: cannot read %s\n", iqPath.c_str());
        return 1;
    }
    double rate = rec.rate;
    printf("%s: %.0f Hz, %s mode\n", iqPath.c_str(), rate, mode ? "AM" : "FM");

    {
        std::ofstream cfg(root + "/hdradio_decoder_config.json");
        cfg << "{\"" << inst << "\":{\"mode\":" << mode << ",\"program\":" << program << "}}";
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
    auto t0 = std::chrono::steady_clock::now();
    long fed = 0;
    int draws = 0;
    for (;;) {
        int got = rec.read(input.writeBuf, 65536);
        if (got == 0) { break; }
        if (!input.swap(got)) { break; }
        fed += got;
        if (fed / (long)(rate / 2) > draws) {
            draws++;
            menuText();
        }
    }
    rec.close();
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
    bool allShown = true;
    for (const auto& e : expected) {
        bool found = shown.find(e) != std::string::npos;
        printf("expected \"%s\": %s\n", e.c_str(), found ? "shown" : "MISSING");
        allShown = allShown && found;
    }
    bool ok = synced && allShown && frames > 48000 * 3 && loud > frames / 4;
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
