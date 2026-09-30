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
#include <core.h>
#include <gui/gui.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

static std::vector<float> fftBuf(65536);
static float* acquireFFT(void*) { return fftBuf.data(); }
static void releaseFFT(void*) {}

// --- a sink that keeps what it is given ----------------------------------------
static std::atomic<long> audioFrames{ 0 };
static std::atomic<long> loudFrames{ 0 };
static std::mutex wavMtx;
static std::vector<int16_t> wav;

class CaptureSink : public SinkManager::Sink {
public:
    CaptureSink(SinkManager::Stream* stream) { hs.init(stream->sinkOut, handler, this); }
    void start() { hs.start(); }
    void stop() { hs.stop(); }
    void menuHandler() {}
    static SinkManager::Sink* create(SinkManager::Stream* stream, std::string, void*) {
        stream->setSampleRate(48000);
        return new CaptureSink(stream);
    }

private:
    static void handler(dsp::stereo_t* data, int count, void*) {
        std::lock_guard<std::mutex> lck(wavMtx);
        for (int i = 0; i < count; i++) {
            if (std::fabs(data[i].l) > 0.01f || std::fabs(data[i].r) > 0.01f) { loudFrames++; }
            wav.push_back((int16_t)std::clamp(data[i].l * 32767.0f, -32768.0f, 32767.0f));
            wav.push_back((int16_t)std::clamp(data[i].r * 32767.0f, -32768.0f, 32767.0f));
        }
        audioFrames += count;
    }
    dsp::sink::Handler<dsp::stereo_t> hs;
};

static void writeWav(const std::string& path, const std::vector<int16_t>& s, int rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    uint32_t bytes = (uint32_t)(s.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    f.write((const char*)s.data(), bytes);
}

// --- the menu, drawn headless, returned as text ---------------------------------
static std::string menuText() {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 700));
    ImGui::Begin("menu");
    ImGui::LogToBuffer();
    for (auto& opt : gui::menu.order) { opt.open = true; }
    gui::menu.draw(true); // true: take the open state set above
    ImGuiContext* g = ImGui::GetCurrentContext();
    std::string text = g->LogBuffer.Buf.Size > 0 ? std::string(g->LogBuffer.Buf.Data, g->LogBuffer.Buf.Size - 1) : "";
    g->LogBuffer.Buf.clear();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    return text;
}

static void printIndented(const std::string& text) {
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) { b = text.size(); }
        std::string line = text.substr(a, b - a);
        if (line.find_first_not_of(" ") != std::string::npos) { printf("    | %s\n", line.c_str()); }
        a = b + 1;
    }
}

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

    char a0[] = "host_test", a1[] = "--root";
    std::vector<char> rootArg(root.begin(), root.end());
    rootArg.push_back(0);
    char* av[] = { a0, a1, rootArg.data(), NULL };
    core::args.defineAll();
    if (core::args.parse(3, av) < 0) { return 2; }

    // Unbuffered, so the feeder below is paced by how fast the chain drains.
    dsp::stream<dsp::complex_t> input;
    sigpath::iqFrontEnd.init(&input, rate, false, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFT, releaseFFT, NULL);
    // Named as the audio sink module names its provider: a stream with no
    // config must find its way here on its own, as it does to the speakers.
    sigpath::sinkManager.registerSinkProvider("Audio", { CaptureSink::create, NULL });

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 800);
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

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
