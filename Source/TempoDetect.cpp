#include "TempoDetect.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace perf
{

using namespace juce;

namespace
{
    constexpr int kOrder = 10, kFrame = 1 << kOrder, kHop = kFrame / 2;
    constexpr double kMinBpm = 50.0, kMaxBpm = 220.0, kPriorBpm = 120.0, kPriorOctaves = 1.0;
    constexpr double kLowHz = 200.0;             // the kick lives below this

    double at (const std::vector<float>& v, double t)
    {
        const int i = (int) std::llround (t);
        if (i < 0 || i >= (int) v.size()) return 0.0;
        // The strongest of the frame and its neighbours: a beat a frame off still counts.
        double m = v[(size_t) i];
        if (i > 0) m = jmax (m, (double) v[(size_t) i - 1]);
        if (i + 1 < (int) v.size()) m = jmax (m, (double) v[(size_t) i + 1]);
        return m;
    }

    /** The curve less its local mean (±half a second), half-wave rectified:
        onsets stand out, the level of the mix does not. */
    std::vector<float> emphasised (const std::vector<float>& in, int halfWindow)
    {
        const int T = (int) in.size();
        std::vector<float> out ((size_t) T, 0.0f);
        std::vector<double> prefix ((size_t) T + 1, 0.0);
        for (int t = 0; t < T; ++t) prefix[(size_t) t + 1] = prefix[(size_t) t] + in[(size_t) t];
        for (int t = 0; t < T; ++t)
        {
            const int a = jmax (0, t - halfWindow), b = jmin (T, t + halfWindow + 1);
            const double mean = (prefix[(size_t) b] - prefix[(size_t) a]) / (double) (b - a);
            out[(size_t) t] = (float) jmax (0.0, in[(size_t) t] - mean);
        }
        return out;
    }
}

TempoEstimate detectTempo (AudioFormatReader& reader, int beatsPerBar,
                           std::function<bool()> shouldStop, std::function<void (float)> progress)
{
    TempoEstimate r;
    const double sr = reader.sampleRate;
    const int64 total = reader.lengthInSamples;
    if (sr <= 0.0 || total < (int64) (sr * 5.0))
    {
        r.problem = "The file is too short to find a tempo in (under five seconds).";
        return r;
    }
    beatsPerBar = jlimit (1, 16, beatsPerBar);
    const double fps = sr / kHop;

    // --- the onset curves: spectral flux of the whole spectrum, and of the low end ----------
    dsp::FFT fft (kOrder);
    std::vector<float> window ((size_t) kFrame);
    for (int i = 0; i < kFrame; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (float) i / (float) (kFrame - 1));
    const int bins = kFrame / 2 + 1;
    const int lowBins = jlimit (1, bins, (int) (kLowHz * kFrame / sr) + 1);
    std::vector<float> fftBuf ((size_t) kFrame * 2), prevLog ((size_t) bins, 0.0f), prevLow ((size_t) lowBins, 0.0f);
    std::vector<float> flux, lowFlux;
    flux.reserve ((size_t) (total / kHop) + 2);
    lowFlux.reserve (flux.capacity());

    constexpr int chunk = 1 << 16;
    AudioBuffer<float> buf ((int) jmax (1u, reader.numChannels), chunk);
    std::vector<float> pending;
    pending.reserve ((size_t) chunk + kFrame);
    size_t head = 0;
    bool first = true;
    for (int64 pos = 0; pos < total; pos += chunk)
    {
        if (shouldStop && shouldStop()) { r.problem = "Stopped."; return r; }
        const int n = (int) jmin ((int64) chunk, total - pos);
        // A read that fails part way (a damaged MP3 frame) ends the listening there; what came before still counts.
        if (! reader.read (&buf, 0, n, pos, true, true)) { if (pos == 0) { r.problem = "The file could not be read."; return r; } break; }
        // Mono: the average of the channels.
        const int ch = buf.getNumChannels();
        for (int i = 0; i < n; ++i)
        {
            float m = 0.0f;
            for (int c = 0; c < ch; ++c) m += buf.getSample (c, i);
            pending.push_back (m / (float) ch);
        }
        while (pending.size() - head >= (size_t) kFrame)
        {
            for (int i = 0; i < kFrame; ++i) fftBuf[(size_t) i] = pending[head + (size_t) i] * window[(size_t) i];
            std::fill (fftBuf.begin() + kFrame, fftBuf.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (fftBuf.data(), true);
            /* The whole spectrum's flux is taken on log magnitudes, so a
               quiet hi-hat counts beside a loud snare; the low end's on plain
               magnitudes, so a kick's one big bin outweighs a snare's spread. */
            float f = 0.0f, lf = 0.0f;
            for (int b = 0; b < bins; ++b)
            {
                const float mag = fftBuf[(size_t) b];
                const float lg = std::log1p (20.0f * mag);
                f += jmax (0.0f, lg - prevLog[(size_t) b]);
                if (b < lowBins) lf += jmax (0.0f, mag - prevLow[(size_t) b]);
                prevLog[(size_t) b] = lg;
                if (b < lowBins) prevLow[(size_t) b] = mag;
            }
            if (! first) { flux.push_back (f); lowFlux.push_back (lf); }
            first = false;
            head += (size_t) kHop;
        }
        if (head > (size_t) chunk) { pending.erase (pending.begin(), pending.begin() + (long) head); head = 0; }
        if (progress) progress ((float) jmin (1.0, (double) (pos + n) / (double) total));
    }

    const int T = (int) flux.size();
    if (T < (int) (fps * 4.0)) { r.problem = "The file is too short to find a tempo in."; return r; }
    const auto osf = emphasised (flux, (int) (fps * 0.5));
    const auto low = emphasised (lowFlux, (int) (fps * 0.5));

    // --- the tempo: autocorrelation over the plausible lags, with a preference near 120 bpm --
    double energy = 0.0;
    for (float v : osf) energy += (double) v * v;
    if (energy <= 1e-9) { r.problem = "No beat could be heard in the file."; return r; }

    const int minLag = jmax (2, (int) std::floor (fps * 60.0 / kMaxBpm));
    const int maxLag = jmin (T / 2 - 1, (int) std::ceil (fps * 60.0 / kMinBpm));
    if (maxLag <= minLag + 2) { r.problem = "The file is too short to find a tempo in."; return r; }
    auto acf = [&] (int lag)
    {
        if (lag <= 0 || lag >= T) return 0.0;
        double s = 0.0;
        for (int t = 0; t + lag < T; ++t) s += (double) osf[(size_t) t] * osf[(size_t) t + (size_t) lag];
        return s / (double) (T - lag);
    };
    std::vector<double> acfs ((size_t) (3 * maxLag + 4), 0.0);
    for (int lag = minLag; lag < (int) acfs.size(); ++lag) acfs[(size_t) lag] = acf (lag);
    std::vector<double> score ((size_t) maxLag + 1, 0.0);
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        const double bpm = 60.0 * fps / lag;
        const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / kPriorBpm) / kPriorOctaves, 2.0));
        const double a = acfs[(size_t) lag]
                       + 0.5 * (2 * lag < (int) acfs.size() ? acfs[(size_t) (2 * lag)] : 0.0)
                       + 0.25 * (3 * lag < (int) acfs.size() ? acfs[(size_t) (3 * lag)] : 0.0);
        score[(size_t) lag] = prior * a;
    }
    int best = minLag;
    for (int lag = minLag; lag <= maxLag; ++lag) if (score[(size_t) lag] > score[(size_t) best]) best = lag;
    double lagF = best;
    if (best > minLag && best < maxLag)
    {
        const double y0 = score[(size_t) best - 1], y1 = score[(size_t) best], y2 = score[(size_t) best + 1];
        const double denom = y0 - 2.0 * y1 + y2;
        if (std::abs (denom) > 1e-12) lagF = best + 0.5 * (y0 - y2) / denom;
    }
    double meanScore = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag) meanScore += score[(size_t) lag];
    meanScore /= (double) (maxLag - minLag + 1);
    r.confidence = meanScore > 0.0 ? jlimit (0.0, 1.0, (score[(size_t) best] / meanScore - 1.0) / 4.0) : 0.0;

    // --- the beats: the phase that lands on the most onsets, then a straight-line fit ------
    auto phaseScore = [&] (double phi, double period)
    {
        double s = 0.0;
        for (double t = phi; t < (double) T; t += period) s += at (osf, t);
        return s;
    };
    double phi = 0.0, bestPhase = -1.0;
    for (double p = 0.0; p < lagF; p += 0.25)
        if (const double s = phaseScore (p, lagF); s > bestPhase) { bestPhase = s; phi = p; }

    double period = lagF;
    for (int pass = 0; pass < 3; ++pass)
    {
        /* Each predicted beat is pulled to the strongest onset within an
           eighth of a beat of it; a line fitted through the ones that found
           an onset gives the tempo more exactly than a lag of whole frames. */
        const int reach = jmax (1, (int) (period * 0.125));
        double sw = 0.0, sk = 0.0, st = 0.0, skk = 0.0, skt = 0.0;
        int found = 0;
        for (int k = 0; ; ++k)
        {
            const double pred = phi + k * period;
            const int c = (int) std::llround (pred);
            if (c >= T) break;
            int bi = -1;
            float bv = 0.0f;
            for (int i = jmax (0, c - reach); i <= jmin (T - 1, c + reach); ++i)
                if (osf[(size_t) i] > bv) { bv = osf[(size_t) i]; bi = i; }
            if (bi < 0 || bv <= 0.0f) continue;
            // Sub-frame: the peak's parabola.
            double tb = bi;
            if (bi > 0 && bi + 1 < T)
            {
                const double y0 = osf[(size_t) bi - 1], y1 = osf[(size_t) bi], y2 = osf[(size_t) bi + 1];
                const double denom = y0 - 2.0 * y1 + y2;
                if (std::abs (denom) > 1e-12) tb += jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / denom);
            }
            const double w = bv;
            sw += w; sk += w * k; st += w * tb; skk += w * k * k; skt += w * k * tb;
            ++found;
        }
        if (found < 8) break;
        const double denom = sw * skk - sk * sk;
        if (std::abs (denom) < 1e-9) break;
        const double slope = (sw * skt - sk * st) / denom;
        const double intercept = (st - slope * sk) / sw;
        if (slope <= 0.0 || std::abs (slope - period) > period * 0.1) break;    // a fit gone astray: keep what we had
        period = slope;
        phi = intercept;
    }
    // Beat one of the grid inside the file: step back while a beat before the first still fits.
    while (phi - period >= -0.5) phi -= period;
    while (phi < -0.5) phi += period;

    // --- the downbeat: of the beat positions in a bar, the one with the most low end ---------
    int downbeat = 0;
    if (beatsPerBar > 1)
    {
        double bestD = -1.0;
        for (int d = 0; d < beatsPerBar; ++d)
        {
            double s = 0.0;
            int n = 0;
            for (double t = phi + d * period; t < (double) T; t += period * beatsPerBar) { s += at (low, t) + 0.3 * at (osf, t); ++n; }
            if (n > 0) s /= (double) n;
            if (s > bestD) { bestD = s; downbeat = d; }
        }
    }

    /* Curve frame i is the frame starting at sample (i + 1) * hop (the very
       first frame has nothing to differ from), and its flux peaks when the
       onset sits about a quarter of the way into the window. */
    r.bpm = 60.0 * fps / period;
    r.firstBeat = jmax (0.0, ((phi + downbeat * period + 1.0) * kHop + kFrame * 0.25) / sr);
    r.ok = r.bpm >= kMinBpm * 0.9 && r.bpm <= kMaxBpm * 1.1;
    if (! r.ok) r.problem = "No steady beat could be found in the file.";
    return r;
}

} // namespace perf
