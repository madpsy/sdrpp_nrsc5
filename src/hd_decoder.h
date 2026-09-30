// HdDecoder: nrsc5's library behind an interface the SDR++ module can drive.
//
// IQ goes in at whatever rate the VFO produces; it is resampled to the rate
// nrsc5 wants and piped in. nrsc5 decodes on the calling thread and reports
// through a callback, from which the selected program's audio is resampled to
// the audio sink's rate and everything else is kept for the menu to show.
//
// Nothing here knows about SDR++, so it can be tested on its own.
#pragma once
#include "resampler.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <nrsc5.h>
}

class HdDecoder {
public:
    enum Mode { MODE_FM = 0, MODE_AM = 1 };
    static constexpr int MAX_PROGRAMS = 8; // nrsc5 numbers them 0..7; stations show them as HD1..HD8

    struct Program {
        bool present = false;    // announced by the station, or its audio has been heard
        int type = -1;           // NRSC5_PROGRAM_TYPE_*, -1 until known
        std::string typeName;
        std::string title, artist, album, genre;
        uint64_t audioFrames = 0;
        uint64_t audioErrors = 0;
    };

    struct Status {
        bool synced = false;
        float freqOffset = 0; // Hz
        int psmi = 0;         // primary service mode
        bool haveMer = false;
        float merLower = 0, merUpper = 0;
        bool haveBer = false;
        float ber = 0;
        std::string country;
        int facilityId = -1;
        std::string name, slogan, message, alert;
        bool haveLocation = false;
        float latitude = 0, longitude = 0;
        int altitude = 0;
        Program programs[MAX_PROGRAMS];
    };

    // Called with interleaved stereo float samples at the audio rate.
    typedef void (*AudioHandler)(const float* stereo, int frames, void* ctx);

    HdDecoder();
    ~HdDecoder();

    // (Re)opens the decoder. Safe to call again to change mode or rate.
    void open(Mode mode, double inputRate);
    void close();
    bool isOpen();

    void setAudioHandler(AudioHandler handler, void* ctx);
    void setAudioRate(double rate);
    void setProgram(int program);
    int getProgram() { return _program; }

    // Forgets the station and starts acquiring afresh, e.g. after a retune.
    // Takes effect on the next process() call.
    void requestReset() { _resetRequested = true; }

    // IQ as interleaved float pairs, at the rate given to open().
    void process(const float* iq, int count);

    Status status();

    static double nativeRate(Mode mode) {
        return mode == MODE_FM ? NRSC5_SAMPLE_RATE_NATIVE_FM : NRSC5_SAMPLE_RATE_NATIVE_AM;
    }

private:
    static void callback(const nrsc5_event_t* evt, void* opaque);
    void handleEvent(const nrsc5_event_t* evt);
    void resetState();

    std::mutex _dspMtx;       // nrsc5 and both resamplers
    nrsc5_t* _radio = nullptr;
    Mode _mode = MODE_FM;
    double _inputRate = 0;
    double _audioRate = 48000;
    PairResampler _iqResamp;
    PairResampler _audioResamp;
    std::vector<float> _iqBuf;
    std::vector<float> _audioIn;
    std::vector<float> _audioOut; // resampled, waiting to go out once _dspMtx is released

    std::atomic<int> _program{ 0 };
    int _audioProgram = -1; // program the audio resampler last carried
    std::atomic<bool> _resetRequested{ false };

    AudioHandler _audioHandler = nullptr;
    void* _audioCtx = nullptr;

    std::mutex _stateMtx;
    Status _state;
};
