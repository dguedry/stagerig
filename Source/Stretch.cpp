#include "Stretch.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>

namespace perf
{

using namespace juce;

namespace
{
    /** Resamples by `factor` (2 = an octave up, half the length) with a
        4-point Lagrange interpolator, in place. */
    void resample (std::vector<int16_t>& left, std::vector<int16_t>& right, double factor)
    {
        if (std::abs (factor - 1.0) < 1e-9 || left.empty()) return;
        const int64 inLen = (int64) left.size();
        const int64 outLen = jmax ((int64) 1, (int64) std::llround ((double) inLen / factor));
        std::vector<float> inL ((size_t) inLen), inR ((size_t) inLen), outL ((size_t) outLen), outR ((size_t) outLen);
        for (int64 i = 0; i < inLen; ++i) { inL[(size_t) i] = left[(size_t) i] / 32767.0f; inR[(size_t) i] = right[(size_t) i] / 32767.0f; }
        LagrangeInterpolator li, ri;
        li.process (factor, inL.data(), outL.data(), (int) outLen, (int) inLen, 0);
        ri.process (factor, inR.data(), outR.data(), (int) outLen, (int) inLen, 0);
        left.resize ((size_t) outLen);
        right.resize ((size_t) outLen);
        for (int64 i = 0; i < outLen; ++i)
        {
            left[(size_t) i]  = (int16_t) jlimit (-32767.0f, 32767.0f, outL[(size_t) i] * 32767.0f);
            right[(size_t) i] = (int16_t) jlimit (-32767.0f, 32767.0f, outR[(size_t) i] * 32767.0f);
        }
    }

    /** WSOLA: the length becomes `scale` times what it was; the pitch stays. */
    void stretch (std::vector<int16_t>& left, std::vector<int16_t>& right, double scale, double sampleRate,
                  const std::function<bool()>& shouldStop)
    {
        if (std::abs (scale - 1.0) < 1e-6 || left.size() < 4096) return;
        const int64 inLen = (int64) left.size();
        const int64 outLen = (int64) std::llround ((double) inLen * scale);

        const int N = jmax (1024, (int) (sampleRate * 0.045) & ~1);        // ~45 ms window
        const int Hs = N / 2;                                               // synthesis hop: windows overlap by half
        const double Ha = (double) Hs / scale;                              // analysis hop
        const int search = N / 4;                                           // how far a window may be nudged
        const int step = 2;                                                 // offsets tried, and samples compared, every so often
        const int cmpStep = 4;

        std::vector<float> window ((size_t) N);
        for (int i = 0; i < N; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (float) i / (float) N);

        // Mono for the search; the stereo follows the same cuts.
        std::vector<float> mono ((size_t) inLen);
        for (int64 i = 0; i < inLen; ++i) mono[(size_t) i] = (left[(size_t) i] + right[(size_t) i]) * (0.5f / 32767.0f);

        std::vector<float> outL ((size_t) outLen + (size_t) N, 0.0f), outR (outL.size(), 0.0f), norm (outL.size(), 0.0f);
        auto sample = [] (const std::vector<int16_t>& v, int64 i) { return i >= 0 && i < (int64) v.size() ? (float) v[(size_t) i] : 0.0f; };

        int64 prevIn = 0;                                                   // where the last window was taken from
        bool first = true;
        for (int64 outPos = 0; outPos < outLen; outPos += Hs)
        {
            if (shouldStop && shouldStop()) return;
            const int64 nominal = (int64) std::llround ((double) outPos / scale);
            int64 chosen = nominal;
            if (! first)
            {
                /* The natural continuation of the last window is Hs on from
                   it; of the candidates around the nominal place, take the one
                   that looks most like that continuation, so the join is clean. */
                const int64 want = prevIn + Hs;
                float best = -1e30f;
                for (int d = -search; d <= search; d += step)
                {
                    const int64 cand = nominal + d;
                    if (cand < 0 || cand + N > inLen) continue;
                    float corr = 0.0f;
                    for (int i = 0; i < N; i += cmpStep)
                    {
                        const int64 w = want + i;
                        const float a = w >= 0 && w < inLen ? mono[(size_t) w] : 0.0f;
                        corr += a * mono[(size_t) (cand + i)];
                    }
                    if (corr > best) { best = corr; chosen = cand; }
                }
            }
            first = false;
            prevIn = chosen;
            for (int i = 0; i < N; ++i)
            {
                const size_t o = (size_t) (outPos + i);
                if (o >= outL.size()) break;
                const float w = window[(size_t) i];
                outL[o] += w * sample (left, chosen + i);
                outR[o] += w * sample (right, chosen + i);
                norm[o] += w;
            }
        }

        left.resize ((size_t) outLen);
        right.resize ((size_t) outLen);
        for (int64 i = 0; i < outLen; ++i)
        {
            const float n = norm[(size_t) i] > 1e-3f ? norm[(size_t) i] : 1.0f;
            left[(size_t) i]  = (int16_t) jlimit (-32767.0f, 32767.0f, outL[(size_t) i] / n);
            right[(size_t) i] = (int16_t) jlimit (-32767.0f, 32767.0f, outR[(size_t) i] / n);
        }
    }
}

void stretchAndShift (std::vector<int16_t>& left, std::vector<int16_t>& right,
                      double timeScale, double semitones, double sampleRate, std::function<bool()> shouldStop)
{
    if (left.size() != right.size() || left.empty()) return;
    timeScale = jlimit (0.25, 4.0, timeScale);
    semitones = jlimit (-24.0, 24.0, semitones);
    const double pitch = std::pow (2.0, semitones / 12.0);               // resampling by this moves the pitch and shortens by it
    resample (left, right, pitch);
    stretch (left, right, timeScale * pitch, sampleRate, shouldStop);    // ...so the stretch puts the length back, and then where it should be
}

} // namespace perf
