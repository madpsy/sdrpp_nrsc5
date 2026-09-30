// HD Radio (NRSC-5) decoder module for SDR++.
//
// Takes a VFO over an FM or AM HD Radio station, decodes it with nrsc5 and
// plays the chosen program (HD1..HD8) through an SDR++ audio stream, showing
// the station's information and what is playing in the menu.
#include "hd_decoder.h"

#include <config.h>
#include <core.h>
#include <dsp/sink/handler_sink.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <imgui.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

SDRPP_MOD_INFO{
    /* Name:            */ "hdradio_decoder",
    /* Description:     */ "HD Radio (NRSC-5) decoder for SDR++, based on nrsc5",
    /* Author:          */ "MadPsy",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// The bandwidths cover the digital sidebands, which reach +/-198.4 kHz for FM
// and +/-15 kHz for AM, with room for the VFO filter's transition band. The
// minimum rate keeps the sidebands inside src/resampler.h's passband.
struct ModeParams {
    const char* label;
    double minRate;
    double bandwidth;
    double snap;
};
static const ModeParams MODES[] = {
    { "FM", 480000.0, 440000.0, 100000.0 },
    { "AM", 40000.0, 34000.0, 1000.0 },
};

// The VFO's output rate: the source's rate halved as often as the mode allows.
// The core's VFO then only runs its power-of-two decimator. Any other ratio
// makes it build a polyphase filter of (out / gcd(in, out)) phases, and for an
// odd source rate such as 1488375 Hz that is millions of taps, built on the
// GUI thread. src/resampler.h takes it the rest of the way to nrsc5's rate.
static double vfoRateFor(double sourceRate, const ModeParams& m) {
    double r = sourceRate;
    while (r / 2.0 >= m.minRate) { r /= 2.0; }
    return r;
}
static const char* MODE_NAMES = "FM\0AM\0";

class HdRadioModule : public ModuleManager::Instance {
public:
    HdRadioModule(std::string name) : name(name) {
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["mode"] = 0;
            config.conf[name]["program"] = 0;
        }
        mode = std::clamp<int>(config.conf[name].value("mode", 0), 0, 1);
        decoder.setProgram(std::clamp<int>(config.conf[name].value("program", 0), 0, HdDecoder::MAX_PROGRAMS - 1));
        config.release(true);

        decoder.setAudioHandler(audioHandler, this);

        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(&audioOut, &srChangeHandler, 48000);
        // The core starts every new stream on the "None" sink unless its
        // config names one; the Radio module only plays because SDR++'s
        // default config has an entry for it. See postInit().
        core::configManager.acquire();
        streamConfigured = core::configManager.conf.contains("streams") && core::configManager.conf["streams"].contains(name);
        core::configManager.release();
        sigpath::sinkManager.registerStream(name, &stream);
        stream.start();

        retuneHandler.ctx = this;
        retuneHandler.handler = retuneEventHandler;
        sigpath::sourceManager.onRetune.bindHandler(&retuneHandler);
        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);

        enable();
        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~HdRadioModule() {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        disable();
        stream.stop();
        sigpath::sourceManager.onRetune.unbindHandler(&retuneHandler);
        sigpath::sinkManager.unregisterStream(name);
    }

    // Every module's sinks are registered by now. A stream never configured
    // before goes to the audio device, as the Radio module's does.
    void postInit() {
        if (!streamConfigured) { sigpath::sinkManager.setStreamSink(name, "Audio"); }
    }

    void enable() {
        if (enabled) { return; }
        const ModeParams& m = MODES[mode];
        double bw = gui::waterfall.getBandwidth();
        sourceRate = sigpath::iqFrontEnd.getEffectiveSamplerate();
        vfoRate = vfoRateFor(sourceRate, m);
        double vfoBw = std::min(m.bandwidth, vfoRate);
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            vfoBw, vfoRate, vfoBw, vfoBw, true);
        vfo->setSnapInterval(m.snap);

        decoder.open((HdDecoder::Mode)mode, vfoRate);
        centerFreq = gui::waterfall.getCenterFrequency();
        lastTuned = NAN;

        iqSink.init(vfo->output, iqHandler, this);
        audioOut.clearWriteStop();
        iqSink.start();
        enabled = true;
    }

    void disable() {
        if (!enabled) { return; }
        // The IQ thread may be blocked handing audio to the sink; release it
        // before joining it.
        audioOut.stopWriter();
        iqSink.stop();
        audioOut.clearWriteStop();
        decoder.close();
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = nullptr;
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    void setMode(int newMode) {
        if (newMode == mode) { return; }
        bool wasEnabled = enabled;
        disable();
        mode = newMode;
        if (wasEnabled) { enable(); }
        config.acquire();
        config.conf[name]["mode"] = mode;
        config.release(true);
    }

    void setProgram(int program) {
        decoder.setProgram(program);
        config.acquire();
        config.conf[name]["program"] = program;
        config.release(true);
    }

    // IQ thread. Also notices a retune (of the source or of this VFO) and has
    // the decoder start over, so one station's details never sit under the
    // next one's name.
    static void iqHandler(dsp::complex_t* data, int count, void* ctx) {
        HdRadioModule* _this = (HdRadioModule*)ctx;
        double tuned = _this->centerFreq.load() + _this->vfo->getOffset();
        if (tuned != _this->lastTuned) {
            if (!std::isnan(_this->lastTuned)) { _this->decoder.requestReset(); }
            _this->lastTuned = tuned;
        }
        _this->decoder.process((const float*)data, count);
    }

    static void audioHandler(const float* stereo, int frames, void* ctx) {
        HdRadioModule* _this = (HdRadioModule*)ctx;
        while (frames > 0) {
            int n = std::min(frames, STREAM_BUFFER_SIZE);
            memcpy(_this->audioOut.writeBuf, stereo, n * sizeof(dsp::stereo_t));
            if (!_this->audioOut.swap(n)) { return; }
            stereo += 2 * n;
            frames -= n;
        }
    }

    // GUI thread, every frame. The core has no event for a new source rate, and
    // it has already reconfigured the VFO for it (from the new rate to the
    // old output rate) by the time this runs; this puts the VFO back on a
    // power-of-two ratio and tells the decoder.
    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        HdRadioModule* _this = (HdRadioModule*)ctx;
        if (!_this->enabled) { return; }
        double sr = sigpath::iqFrontEnd.getEffectiveSamplerate();
        if (sr == _this->sourceRate) { return; }
        _this->sourceRate = sr;
        const ModeParams& m = MODES[_this->mode];
        double rate = vfoRateFor(sr, m);
        if (rate == _this->vfoRate) { return; }
        _this->vfoRate = rate;
        double vfoBw = std::min(m.bandwidth, rate);
        _this->vfo->setBandwidthLimits(vfoBw, vfoBw, true);
        _this->vfo->setSampleRate(rate, vfoBw);
        _this->decoder.setInputRate(rate);
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        HdRadioModule* _this = (HdRadioModule*)ctx;
        _this->decoder.setAudioRate(sampleRate);
    }

    static void retuneEventHandler(double freq, void* ctx) {
        HdRadioModule* _this = (HdRadioModule*)ctx;
        _this->centerFreq = freq;
    }

    static void menuHandler(void* ctx) {
        HdRadioModule* _this = (HdRadioModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;
        std::string id = "##hdradio_" + _this->name;

        ImGui::LeftLabel("Mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        int mode = _this->mode;
        if (ImGui::Combo(("##mode" + id).c_str(), &mode, MODE_NAMES)) { _this->setMode(mode); }

        if (!_this->enabled) { style::beginDisabled(); }

        HdDecoder::Status s = _this->decoder.status();

        if (s.synced) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Synced");
            ImGui::SameLine();
            ImGui::Text("(%+.0f Hz)", s.freqOffset);
        }
        else {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Searching for HD signal...");
        }
        if (s.haveMer) { ImGui::Text("MER  %.1f / %.1f dB", s.merLower, s.merUpper); }
        if (s.haveBer) {
            ImGui::SameLine();
            ImGui::Text("   BER %.4f%%", s.ber * 100.0f);
        }

        // Programs: every one the station has announced or been heard on.
        // The selected one is always listed, so it can be seen waiting.
        int selected = _this->decoder.getProgram();
        ImGui::Text("Program");
        bool any = false;
        for (int p = 0; p < HdDecoder::MAX_PROGRAMS; p++) {
            if (!s.programs[p].present && p != selected) { continue; }
            if (any) { ImGui::SameLine(); }
            any = true;
            char label[32];
            snprintf(label, sizeof(label), "HD%d%s", p + 1, id.c_str());
            if (ImGui::RadioButton(label, selected == p)) { _this->setProgram(p); }
        }

        const HdDecoder::Program& prog = s.programs[selected];
        if (!prog.typeName.empty()) { ImGui::TextDisabled("%s", prog.typeName.c_str()); }
        if (s.synced && prog.present && prog.audioFrames == 0) { ImGui::TextDisabled("Waiting for audio..."); }

        ImGui::Separator();
        if (!s.name.empty() || s.facilityId > 0) {
            if (s.facilityId > 0) {
                ImGui::Text("%s  (%s facility %d)", s.name.empty() ? "?" : s.name.c_str(), s.country.c_str(), s.facilityId);
            }
            else {
                ImGui::Text("%s", s.name.c_str());
            }
        }
        if (!s.slogan.empty()) { ImGui::TextWrapped("%s", s.slogan.c_str()); }
        if (!s.message.empty()) { ImGui::TextWrapped("%s", s.message.c_str()); }
        if (s.haveLocation) { ImGui::TextDisabled("%.4f, %.4f  %d m", s.latitude, s.longitude, s.altitude); }
        if (!s.alert.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "ALERT: %s", s.alert.c_str()); }

        if (!prog.title.empty() || !prog.artist.empty()) {
            ImGui::Separator();
            if (!prog.title.empty()) { ImGui::TextWrapped("%s", prog.title.c_str()); }
            if (!prog.artist.empty()) { ImGui::TextWrapped("%s", prog.artist.c_str()); }
            if (!prog.album.empty()) { ImGui::TextDisabled("%s", prog.album.c_str()); }
        }

        if (ImGui::Button(("Reset" + id).c_str(), ImVec2(menuWidth, 0))) { _this->decoder.requestReset(); }

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::string name;
    bool enabled = false;
    int mode = 0;

    VFOManager::VFO* vfo = nullptr;
    dsp::sink::Handler<dsp::complex_t> iqSink;
    HdDecoder decoder;

    std::atomic<double> centerFreq{ 0 };
    double lastTuned = NAN; // IQ thread only
    EventHandler<double> retuneHandler;
    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    double sourceRate = 0; // GUI thread
    double vfoRate = 0;
    bool streamConfigured = false;

    dsp::stream<dsp::stereo_t> audioOut;
    EventHandler<float> srChangeHandler;
    SinkManager::Stream stream;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/hdradio_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new HdRadioModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (HdRadioModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
