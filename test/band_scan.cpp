// Looks for hybrid HD Radio stations on MW: steps a real source across the
// band and scores each channel for the digital sidebands' signature, before
// anything is asked to decode them.
//
//   band_scan <source module> <source name> <root dir> [start kHz] [stop kHz] [step kHz]
//
// Hybrid AM HD puts its primary sidebands from about 10.4 to 14.7 kHz either
// side of the carrier: flat, noise-like blocks. Analogue audio is usually
// band-limited to 10 kHz, and the neighbours 10-20 kHz away mostly put their
// energy near their own carriers, so a channel is flagged when both sides are
// flat across that span and stand clear of 15.5-17.5 kHz, just outside it.
// Needs at least 40 kHz of IQ from the source.
#include "harness.h"

#include <fftw3.h>

#include <chrono>
#include <cstdlib>
#include <thread>

static std::mutex capMtx;
static std::vector<dsp::complex_t> captured;
static std::atomic<bool> capturing{ false };

static void capture(dsp::complex_t* d, int n, void*) {
    if (!capturing) { return; }
    std::lock_guard<std::mutex> lck(capMtx);
    captured.insert(captured.end(), d, d + n);
}

struct Score {
    double carrierDb = 0;
    double side[2] = { 0, 0 };   // mean level 10.6-14.6 kHz, dB relative to the edge region
    double flat[2] = { 0, 0 };   // spread of 0.5 kHz sub-bands within that span, dB
    bool hd = false;
};

static Score score(const std::vector<dsp::complex_t>& x, double fs) {
    const int N = 4096;
    std::vector<double> P(N, 0.0);
    fftwf_complex* buf = fftwf_alloc_complex(N);
    fftwf_plan plan = fftwf_plan_dft_1d(N, buf, buf, FFTW_FORWARD, FFTW_ESTIMATE);
    int segs = (int)(x.size() / N);
    for (int s = 0; s < segs; s++) {
        for (int i = 0; i < N; i++) {
            double w = 0.5 - 0.5 * cos(2 * M_PI * i / (N - 1));
            buf[i][0] = x[s * N + i].re * w;
            buf[i][1] = x[s * N + i].im * w;
        }
        fftwf_execute(plan);
        for (int i = 0; i < N; i++) { P[i] += buf[i][0] * buf[i][0] + buf[i][1] * buf[i][1]; }
    }
    fftwf_destroy_plan(plan);
    fftwf_free(buf);

    auto bin = [&](double f) { return ((int)lround(f / fs * N) + N) % N; };
    auto meanDb = [&](double f0, double f1) {
        double sum = 0;
        int n = 0;
        for (double f = f0; f < f1; f += fs / N) {
            sum += P[bin(f)];
            n++;
        }
        return 10 * log10(sum / std::max(n, 1) + 1e-30);
    };

    Score sc;
    sc.carrierDb = 10 * log10(P[0] + P[1] + P[N - 1] + 1e-30);
    for (int side = 0; side < 2; side++) {
        double sign = side == 0 ? -1 : 1;
        auto band = [&](double a, double b) { return sign > 0 ? meanDb(a, b) : meanDb(-b, -a); };
        double lo = 1e9, hi = -1e9;
        for (double f = 10600; f < 14600; f += 500) {
            double v = band(f, f + 500);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        sc.side[side] = band(10600, 14600) - band(15500, 17500);
        sc.flat[side] = hi - lo;
    }
    sc.hd = sc.side[0] > 6 && sc.side[1] > 6 && sc.flat[0] < 6 && sc.flat[1] < 6;
    return sc;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <source module> <source name> <root> [start kHz] [stop kHz] [step kHz]\n", argv[0]);
        return 2;
    }
    std::string srcPath = argv[1], srcName = argv[2], root = argv[3];
    int start = argc > 4 ? atoi(argv[4]) : 540;
    int stop = argc > 5 ? atoi(argv[5]) : 1700;
    int step = argc > 6 ? atoi(argv[6]) : 10;

    if (!initHeadlessCore(root)) { return 2; }
    dsp::stream<dsp::complex_t> dummy;
    sigpath::iqFrontEnd.init(&dummy, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFT, releaseFFT, NULL);

    auto src = core::moduleManager.loadModule(srcPath);
    if (!src.handle || core::moduleManager.createInstance(srcName, src.info->name) != 0) {
        printf("FAIL: source module\n");
        return 1;
    }
    core::moduleManager.doPostInitAll();

    dsp::stream<dsp::complex_t> tap;
    dsp::sink::Handler<dsp::complex_t> tapSink;
    sigpath::iqFrontEnd.bindIQStream(&tap);
    tapSink.init(&tap, capture, NULL);
    tapSink.start();

    sigpath::sourceManager.selectSource(srcName);
    sigpath::iqFrontEnd.start();
    sigpath::sourceManager.tune(start * 1000.0);
    sigpath::sourceManager.start();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    double fs = sigpath::iqFrontEnd.getEffectiveSamplerate();
    printf("source rate %.0f Hz\n", fs);
    if (fs < 40000) {
        printf("FAIL: need at least 40 kHz of IQ\n");
        return 1;
    }

    printf("  kHz   carrier   lower: level flat   upper: level flat\n");
    std::vector<std::pair<int, Score>> hits;
    for (int k = start; k <= stop; k += step) {
        sigpath::sourceManager.tune(k * 1000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // let the retune reach the stream
        {
            std::lock_guard<std::mutex> lck(capMtx);
            captured.clear();
        }
        capturing = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        capturing = false;
        std::vector<dsp::complex_t> x;
        {
            std::lock_guard<std::mutex> lck(capMtx);
            x.swap(captured);
        }
        Score sc = score(x, fs);
        printf("%5d  %7.1f dB   %+6.1f %4.1f     %+6.1f %4.1f   %s\n", k, sc.carrierDb, sc.side[0], sc.flat[0], sc.side[1],
               sc.flat[1], sc.hd ? "<-- HD?" : "");
        fflush(stdout);
        if (sc.hd) { hits.push_back({ k, sc }); }
    }

    printf("candidates:");
    for (auto& [k, sc] : hits) { printf(" %d", k); }
    printf("%s\n", hits.empty() ? " none" : "");

    sigpath::sourceManager.stop();
    tapSink.stop();
    sigpath::iqFrontEnd.unbindIQStream(&tap);
    core::moduleManager.deleteInstance(srcName);
    if (src.end) { src.end(); }
    sigpath::iqFrontEnd.stop();
    return 0;
}
