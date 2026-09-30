// Shared by host_test and live_test: a headless SDR++ with a capture sink
// standing in for the audio device, and the menu drawn to text.
#pragma once
#include <core.h>
#include <gui/gui.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
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

// A headless core with --root <root>, the capture sink registered under the
// name the audio sink module uses, and an ImGui context to draw menus into.
static bool initHeadlessCore(const std::string& root) {
    static char a0[] = "sdrpp", a1[] = "--root";
    static std::vector<char> rootArg;
    rootArg.assign(root.begin(), root.end());
    rootArg.push_back(0);
    char* av[] = { a0, a1, rootArg.data(), NULL };
    core::args.defineAll();
    if (core::args.parse(3, av) < 0) { return false; }
    // Named as the audio sink module names its provider: a stream with no
    // config must find its way here on its own, as it does to the speakers.
    sigpath::sinkManager.registerSinkProvider("Audio", { CaptureSink::create, NULL });
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 800);
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    return true;
}
