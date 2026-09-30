// Checks PairResampler at the ratios the module uses: the output rate must be
// exact over a long run (nrsc5 tracks a few ppm of clock error, not more), a
// complex tone must come through at the right frequency with little distortion,
// and feeding in odd-sized blocks must give the same result as regular ones.
#include "resampler.h"

#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>

static bool check(const char* name, double inRate, double outRate, double toneHz, int taps, double minSnrDb) {
    const int n = (int)(inRate * 4); // four seconds
    std::vector<float> in(2 * n);
    for (int i = 0; i < n; i++) {
        double ph = 2 * M_PI * toneHz * i / inRate;
        in[2 * i] = (float)cos(ph);
        in[2 * i + 1] = (float)sin(ph);
    }

    PairResampler r;
    r.init(inRate, outRate, taps);
    std::vector<float> out;
    int pos = 0, blk = 1;
    while (pos < n) {
        int c = std::min(blk, n - pos);
        r.process(&in[2 * pos], c, out);
        pos += c;
        blk = (blk * 7 + 3) % 5003 + 1; // uneven block sizes
    }
    int m = (int)(out.size() / 2);
    double expected = n * outRate / inRate;

    PairResampler r2;
    r2.init(inRate, outRate, taps);
    std::vector<float> out2;
    // Fixed blocks, as SDR++ delivers them. (One huge block is no reference:
    // the position counter then grows into the millions and loses precision.)
    for (int p = 0; p < n; p += 65536) { r2.process(&in[2 * p], std::min(65536, n - p), out2); }
    bool blocksMatch = out2.size() == out.size();
    for (size_t i = 0; blocksMatch && i < out.size(); i++) { blocksMatch = std::fabs(out[i] - out2[i]) < 1e-5f; }

    // Fit the ideal tone (amplitude and phase free) away from the start-up
    // transient, and measure what is left over.
    std::complex<double> acc = 0;
    int start = taps * 4, cnt = 0;
    for (int i = start; i < m - taps; i++) {
        std::complex<double> y(out[2 * i], out[2 * i + 1]);
        acc += y * std::polar(1.0, -2 * M_PI * toneHz * i / outRate);
        cnt++;
    }
    std::complex<double> a = acc / (double)cnt;
    double sig = 0, err = 0;
    for (int i = start; i < m - taps; i++) {
        std::complex<double> y(out[2 * i], out[2 * i + 1]);
        std::complex<double> ideal = a * std::polar(1.0, 2 * M_PI * toneHz * i / outRate);
        sig += std::norm(ideal);
        err += std::norm(y - ideal);
    }
    double snr = 10 * log10(sig / err);
    double rateErrPpm = (m - expected) / expected * 1e6;
    bool ok = std::fabs(m - expected) <= taps && snr >= minSnrDb && std::fabs(std::abs(a) - 1) < 0.01 && blocksMatch;
    printf("%-26s %9.2f -> %11.5f Hz, tone %8.0f Hz: %d out (expected %.0f, %+.2f ppm), gain %.4f, SNR %.1f dB, blocks %s  %s\n",
           name, inRate, outRate, toneHz, m, expected, rateErrPpm, std::abs(a), snr,
           blocksMatch ? "match" : "DIFFER", ok ? "ok" : "FAIL");
    return ok;
}

int main() {
    bool ok = true;
    ok &= check("FM IQ", 750000, 744187.5, 190000, 32, 60);
    ok &= check("FM IQ, negative tone", 750000, 744187.5, -150000, 32, 60);
    ok &= check("AM IQ", 50000, 46511.71875, 12000, 32, 60);
    ok &= check("audio 44.1k -> 48k", 44100, 48000, 15000, 48, 60);
    ok &= check("audio 44.1k -> 44.1k", 44100, 44100, 1000, 48, 60);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
