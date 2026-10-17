// Throat DSP core · bytebent
//
// C++ port of reference/throat-engine.js, which defines the sound. tests/compare.js checks that
// both produce the same audio. No JUCE dependency.
//
// Real-time rules: everything is allocated in prepare() (tables are built once and shared);
// process() never allocates, locks or does I/O. The engine works in fixed 32-sample blocks
// whatever the host buffer size, so it reports 32 samples of latency.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wfloat-equal"     // exact compares are intended (NaN check, 0/1 switches)
 #pragma GCC diagnostic ignored "-Wsign-conversion" // table building indexes vectors with int offsets
#endif

namespace bytebent { namespace throat {

constexpr int OS = 4;                 // internal oversampling
constexpr int TN = 2048;              // table points per glottal period
constexpr int TW = TN + 1;            // stored row width (wrap point)
constexpr int MIP_LEVELS = 9;         // harmonic limits 1024 … 4
constexpr double RD_LO = 0.3, RD_HI = 2.7, RD_STEP = 0.05;
constexpr int RD_COUNT = 49;
constexpr double RD_MODAL = 1.0;
constexpr double RD_SUB = 0.45;
constexpr double TABLE_RMS = 0.1;
constexpr double NORM_POW = 0.7;
constexpr int FIR_LEN = 128;
constexpr int BLOCK = 32;             // fixed internal block = latency in samples
constexpr int MAX_VOICES = 8;
constexpr double NOISE_SOFT_HZ = 2500, NOISE_BRIGHT_HZ = 1800, NOISE_AIR_HZ = 7000;
constexpr double NOISE_SOFT_GAIN = 0.89, NOISE_BRIGHT_GAIN = 0.6;
constexpr double ROUGH_HZ = 45;
constexpr double PI = 3.141592653589793;
constexpr double LN2 = 0.6931471805599453;

enum Param {
    pVoicing, pBreathy, pTense, pCreaky, pThin, pGrowl, pJitter, pShimmer,
    pTranspose, pDetune, pMode, pGlide, pAttack, pDecay, pSustain, pRelease, pVolume, pSource,
    kNumParams
};

struct ParamInfo { const char* id; const char* name; double def, min, max; };

// ids and ranges match reference/throat-engine.js PARAMS exactly
inline const ParamInfo& paramInfo(int i) {
    static const ParamInfo t[kNumParams] = {
        { "voicing",   "Voicing",   1,     0,     1 },
        { "breathy",   "Breathy",   0,     0,     1 },
        { "tense",     "Tense",     0,     0,     1 },
        { "creaky",    "Creaky",    0,     0,     1 },
        { "thin",      "Thin",      0,     0,     1 },
        { "growl",     "Growl",     0,     0,     1 },
        { "jitter",    "Jitter",    0,     0,     1 },
        { "shimmer",   "Shimmer",   0,     0,     1 },
        { "transpose", "Transpose", 0,    -24,    24 },     // semitones
        { "detune",    "Detune",    0,    -100,   100 },    // cents
        { "mode",      "Voice mode",0,     0,     1 },      // 0 mono, 1 poly
        { "glide",     "Glide",     0.06,  0,     2 },      // seconds, mono only
        { "attack",    "Attack",    0.02,  0.001, 5 },
        { "decay",     "Decay",     0.3,   0.001, 5 },
        { "sustain",   "Sustain",   0.8,   0,     1 },
        { "release",   "Release",   0.25,  0.001, 8 },
        { "volume",    "Volume",   -6,    -48,    6 },      // dB
        { "source",    "Source",    0,     0,     1 },      // 0 flow, 1 derivative
    };
    return t[i];
}

inline int paramIndex(const char* id) {
    for (int i = 0; i < kNumParams; ++i) if (std::strcmp(paramInfo(i).id, id) == 0) return i;
    return -1;
}

/* ---------------- LF model and tables ---------------- */

struct LF { double Rd, Tp, Te, Ta, eps, a, wg, sinTe, ed, flowTe; };

inline LF lfSolve(double Rd) {
    const double Ra = (-1 + 4.8 * Rd) / 100;
    const double Rk = (22.4 + 11.8 * Rd) / 100;
    const double Rg = 0.25 * Rk / (0.11 * Rd / (0.5 + 1.2 * Rk) - Ra);
    const double Tp = 1 / (2 * Rg), Te = Tp * (1 + Rk), Ta = Ra;
    const double d = 1 - Te;
    double eps = 1 / Ta;
    for (int i = 0; i < 300; ++i) eps = (1 - std::exp(-eps * d)) / Ta;
    const double wg = PI / Tp, sinTe = std::sin(wg * Te), cosTe = std::cos(wg * Te);
    const double ed = std::exp(-eps * d);
    const double retArea = -((1 - ed) / eps - d * ed) / (eps * Ta);
    auto openArea = [&](double a) {
        return (a * sinTe - wg * cosTe + wg * std::exp(-a * Te)) * (-1 / sinTe) / (a * a + wg * wg);
    };
    double lo = -200, hi = 200;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (openArea(mid) + retArea > 0) lo = mid; else hi = mid;
    }
    const double a = 0.5 * (lo + hi);
    return { Rd, Tp, Te, Ta, eps, a, wg, sinTe, ed, openArea(a) };
}

inline void lfEval(const LF& p, double t, double& flow, double& deriv) {
    if (t <= p.Te) {
        const double e = std::exp(p.a * (t - p.Te));
        const double s = std::sin(p.wg * t), c = std::cos(p.wg * t);
        deriv = -e * s / p.sinTe;
        flow = (e * (p.a * s - p.wg * c) + p.wg * std::exp(-p.a * p.Te)) * (-1 / p.sinTe) / (p.a * p.a + p.wg * p.wg);
    } else {
        const double u = t - p.Te, eu = std::exp(-p.eps * u);
        deriv = -(eu - p.ed) / (p.eps * p.Ta);
        flow = p.flowTe - ((1 - eu) / p.eps - u * p.ed) / (p.eps * p.Ta);
    }
}

// same operation order as the reference, so tables match bit for bit
inline void fft(double* re, double* im, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2 : -2) * PI / len, wr = std::cos(ang), wi = std::sin(ang);
        const int h = len >> 1;
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int j = 0; j < h; ++j) {
                const int a = i + j, b = a + h;
                const double vr = re[b] * cr - im[b] * ci, vi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - vr; im[b] = im[a] - vi; re[a] += vr; im[a] += vi;
                const double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

template <typename T>
inline double rmsOf(const T* arr, int n, bool removeMean) {
    double m = 0;
    if (removeMean) { for (int i = 0; i < n; ++i) m += arr[i]; m /= n; }
    double s = 0;
    for (int i = 0; i < n; ++i) { const double v = arr[i] - m; s += v * v; }
    return std::sqrt(s / n);
}

struct Tables {
    std::vector<float> flow, deriv, shape, te, sFlow, sDeriv, sShape;
    std::vector<float> flowMip[MIP_LEVELS], derivMip[MIP_LEVELS];

    Tables() {
        std::vector<double> flowRaw(RD_COUNT * TW), derivRaw(RD_COUNT * TW);
        flow.assign(RD_COUNT * TW, 0.f); deriv.assign(RD_COUNT * TW, 0.f); shape.assign(RD_COUNT * TW, 0.f);
        te.assign(RD_COUNT, 0.f);
        for (int r = 0; r < RD_COUNT; ++r) {
            const LF p = lfSolve(RD_LO + r * RD_STEP);
            te[r] = (float) p.Te;
            const int o = r * TW;
            double f, d;
            for (int i = 0; i < TN; ++i) {
                lfEval(p, (double) i / TN, f, d);
                flowRaw[o + i] = f; derivRaw[o + i] = d;
            }
            lfEval(p, 1, f, d);
            const double endFlow = f;
            for (int i = 0; i < TN; ++i) flowRaw[o + i] -= endFlow * ((double) i / TN);
            flowRaw[o + TN] = flowRaw[o];
            derivRaw[o + TN] = derivRaw[o];
        }
        const int ref = (int) std::lround((RD_MODAL - RD_LO) / RD_STEP);
        const double refF = rmsOf(&flowRaw[ref * TW], TN, true), refD = rmsOf(&derivRaw[ref * TW], TN, false);
        for (int l = 0; l < MIP_LEVELS; ++l) { flowMip[l].assign(RD_COUNT * TW, 0.f); derivMip[l].assign(RD_COUNT * TW, 0.f); }
        std::vector<double> re(TN), im(TN), sre(TN), sim(TN);
        for (int r = 0; r < RD_COUNT; ++r) {
            const int o = r * TW;
            const double rf = rmsOf(&flowRaw[o], TN, true), rd = rmsOf(&derivRaw[o], TN, false);
            const double gf = (TABLE_RMS / refF) * std::pow(refF / rf, NORM_POW);
            const double gd = (TABLE_RMS / refD) * std::pow(refD / rd, NORM_POW);
            double peak = 0;
            for (int i = 0; i < TW; ++i) peak = std::max(peak, flowRaw[o + i]);
            for (int i = 0; i < TW; ++i) {
                flow[o + i] = (float) (flowRaw[o + i] * gf);
                deriv[o + i] = (float) (derivRaw[o + i] * gd);
                shape[o + i] = (float) std::max(0.0, flowRaw[o + i] / peak);
            }
            for (int kind = 0; kind < 2; ++kind) {
                const std::vector<float>& src = kind ? deriv : flow;
                std::vector<float>* dst = kind ? derivMip : flowMip;
                for (int i = 0; i < TN; ++i) { sre[i] = src[o + i]; sim[i] = 0; }
                fft(sre.data(), sim.data(), TN, false);
                for (int l = 0; l < MIP_LEVELS; ++l) {
                    const int H = (TN / 2) >> l;
                    std::fill(re.begin(), re.end(), 0.0); std::fill(im.begin(), im.end(), 0.0);
                    re[0] = sre[0];
                    for (int k = 1; k <= H && k < TN / 2; ++k) {
                        re[k] = sre[k]; im[k] = sim[k]; re[TN - k] = sre[TN - k]; im[TN - k] = sim[TN - k];
                    }
                    if (H >= TN / 2) re[TN / 2] = sre[TN / 2];
                    fft(re.data(), im.data(), TN, true);
                    float* dd = dst[l].data();
                    for (int i = 0; i < TN; ++i) dd[o + i] = (float) (re[i] / TN);
                    dd[o + TN] = dd[o];
                }
            }
        }
        sFlow.assign(TW, 0.f); sDeriv.assign(TW, 0.f); sShape.assign(TW, 0.f);
        for (int i = 0; i < TW; ++i) {
            const double t = (double) i / TN;
            sShape[i] = (float) (0.5 * (1 - std::cos(2 * PI * t)));
            sFlow[i] = sShape[i];
            sDeriv[i] = (float) (PI * std::sin(2 * PI * t));
        }
        const double thinLevel = 0.8 * TABLE_RMS;
        const double g1 = thinLevel / rmsOf(sFlow.data(), TN, true), g2 = thinLevel / rmsOf(sDeriv.data(), TN, false);
        for (int i = 0; i < TW; ++i) { sFlow[i] = (float) (sFlow[i] * g1); sDeriv[i] = (float) (sDeriv[i] * g2); }
    }

    static const Tables& get() { static const Tables t; return t; }   // thread-safe, built once
};

// band-limited table for a pulse lasting `len` samples at the 4x rate
inline int mipLevel(double len) {
    const double maxH = len * (1 - 0.5 / OS);
    int l = 0, H = TN / 2;
    while (H > maxH && l < MIP_LEVELS - 1) { H >>= 1; ++l; }
    return l;
}

/* ---------------- engine ---------------- */

class Engine;

class Voice {
public:
    Engine* e = nullptr;
    uint32_t seed = 1, rng = 1;
    int stage = 0;          // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
    double env = 0;
    int note = -1;
    bool held = false;
    double vel = 1;
    long long age = 0;
    double logF = 0, glideStep = 0;
    long long glideLeft = 0;
    bool hasPitch = false;
    double cyc = 0, per = 0, pulse = 1, inc = 1;
    long long cycleN = 0;
    double amp = 1;
    int ri = 0; double rf = 0, sm = 0;
    const float* tab = nullptr; const float* sTab = nullptr; const float* subTab = nullptr;
    double subPos = 0;
    double n1 = 0, n2 = 0, n3 = 0, rough = 0;
    double lastF0 = 0, lastRd = RD_MODAL, lastOq = 0;

    void init(Engine* eng, uint32_t s) { e = eng; seed = s ? s : 1; rng = seed; reset(); }
    void reset() {
        stage = 0; env = 0; note = -1; held = false; vel = 1; age = 0;
        logF = std::log(110.0); glideLeft = 0; glideStep = 0; hasPitch = false;
        cyc = 0; per = 0; pulse = 1; inc = 1; cycleN = 0; amp = 1;
        ri = 0; rf = 0; sm = 0; tab = sTab = subTab = nullptr; subPos = 0;
        n1 = n2 = n3 = rough = 0; lastF0 = 0; lastRd = RD_MODAL; lastOq = 0;
    }
    double rand() {
        uint32_t x = rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rng = x;
        return rng / 4294967296.0;
    }
    double gauss() {   // sequenced explicitly: C++ doesn't fix operand evaluation order
        double s = rand(); s += rand(); s += rand(); s += rand();
        return (s - 2) * 1.7320508;
    }
    bool active() const { return stage != 0; }
    inline void setTarget(int note, bool glide);
    inline void trigger(double v);
    void release() { if (stage != 0) stage = 4; }
    inline void startCycle();
    inline void render(int n);
};

class Engine {
public:
    double sr = 48000, srOS = 192000;
    const Tables* T = nullptr;
    int subRow = 3;
    double p[kNumParams] = {};
    // smoothed knobs
    double sVoicing = 1, sBreathy = 0, sTense = 0, sCreaky = 0, sThin = 0, sGrowl = 0;
    double smoothCoef = 0;
    double lnRd = 0, sineMix = 0, pitchLog = 0;
    bool sourceDeriv = false;
    Voice voices[MAX_VOICES];
    long long ageCounter = 0;
    int stack[128] = {}; int stackLen = 0;
    float osBuf[BLOCK * OS] = {};
    float blk[BLOCK] = {}; int blkPos = BLOCK;
    float rVoicing[BLOCK] = {}, rSteady[BLOCK] = {}, rPulse[BLOCK] = {}, rBright[BLOCK] = {}, rGrowl[BLOCK] = {}, rVol[BLOCK] = {};
    double cVoicing = 1, cSteady = 0, cPulse = 0, cBright = 0.35, cGrowl = 0, cVol = 1;
    double nA1 = 0, nA2 = 0, nA3 = 0, rA = 0, roughGain = 1, nG1 = NOISE_SOFT_GAIN, nG2 = NOISE_BRIGHT_GAIN;
    float fir[FIR_LEN] = {};
    float hist[FIR_LEN * 2] = {}; int hw = 0;
    double dcR = 0, dcX = 0, dcY = 0;
    // live view: the latest voice's glottal signal, two cycles at a time
    Voice* vizVoice = nullptr;
    std::vector<float> capG, capN;
    int capLen = 0, capCycles = 0;
    float vizG[256] = {}, vizN[256] = {};
    bool vizReady = false;
    double vizF0 = 0, vizRd = 1, vizOq = 0; int vizVoices = 0;

    static constexpr int latency() { return BLOCK; }

    // allocates; call off the audio thread
    void prepare(double sampleRate, uint32_t seedIn = 1234) {
        sr = sampleRate; srOS = sampleRate * OS;
        T = &Tables::get();
        subRow = (int) std::lround((RD_SUB - RD_LO) / RD_STEP);
        for (int i = 0; i < kNumParams; ++i) p[i] = paramInfo(i).def;
        sVoicing = 1; sBreathy = sTense = sCreaky = sThin = sGrowl = 0;
        smoothCoef = 1 - std::exp(-BLOCK / (0.02 * sampleRate));
        lnRd = std::log(RD_MODAL); sineMix = 0; sourceDeriv = false; pitchLog = 0;
        const uint32_t base = seedIn ? seedIn : 0x9e3779b9u;
        for (int i = 0; i < MAX_VOICES; ++i) voices[i].init(this, base + (uint32_t) i * 0x6d2b79f5u);
        ageCounter = 0; stackLen = 0;
        std::fill(std::begin(osBuf), std::end(osBuf), 0.f);
        std::fill(std::begin(blk), std::end(blk), 0.f); blkPos = BLOCK;
        cVoicing = 1; cSteady = 0; cPulse = 0; cBright = 0.35; cGrowl = 0; cVol = std::pow(10.0, paramInfo(pVolume).def / 20);
        auto op = [&](double hz) { return 1 - std::exp(-2 * PI * hz / srOS); };
        nA1 = op(NOISE_SOFT_HZ); nA2 = op(NOISE_BRIGHT_HZ); nA3 = op(NOISE_AIR_HZ); rA = op(ROUGH_HZ);
        const double sdU = 1 / std::sqrt(3.0);
        roughGain = 1 / (sdU * std::sqrt(rA / (2 - rA)));
        nG1 = NOISE_SOFT_GAIN; nG2 = NOISE_BRIGHT_GAIN;
        designFir();
        std::fill(std::begin(hist), std::end(hist), 0.f); hw = 0;
        dcR = 1 - 2 * PI * 20 / sampleRate; dcX = dcY = 0;
        vizVoice = nullptr;
        capG.assign(65536, 0.f); capN.assign(65536, 0.f);
        capLen = 0; capCycles = 0; vizReady = false;
        vizF0 = 0; vizRd = 1; vizOq = 0; vizVoices = 0;
    }

    void designFir() {
        const double fc = 0.5 / OS;
        const int M = FIR_LEN - 1;
        double sum = 0;
        for (int i = 0; i < FIR_LEN; ++i) {
            const double x = i - M / 2.0;
            const double sinc = x == 0 ? 2 * fc : std::sin(2 * PI * fc * x) / (PI * x);
            const double w = 0.42 - 0.5 * std::cos(2 * PI * i / M) + 0.08 * std::cos(4 * PI * i / M);
            fir[i] = (float) (sinc * w);
            sum += fir[i];
        }
        for (int i = 0; i < FIR_LEN; ++i) fir[i] = (float) (fir[i] / sum);
    }

    void setParam(int id, double v) {
        if (id < 0 || id >= kNumParams) return;
        if (!(v == v)) return;
        const ParamInfo& d = paramInfo(id);
        if (v < d.min) v = d.min; else if (v > d.max) v = d.max;
        if (id == pMode) {
            v = v >= 0.5 ? 1 : 0;
            if (v != p[pMode]) { allNotesOff(); stackLen = 0; }
        }
        if (id == pSource) v = v >= 0.5 ? 1 : 0;
        p[id] = v;
    }

    static double velAmp(double vel) { return std::max(1.0, std::min(127.0, vel)) / 127; }

    void noteOn(int note, double vel = 100) {
        const double va = velAmp(vel);
        if (p[pMode] == 0) {
            Voice& v = voices[0];
            const bool legato = stackLen > 0;
            stackRemove(note);
            if (stackLen < 128) stack[stackLen++] = note;
            v.setTarget(note, true);
            if (!legato || v.stage == 0 || v.stage == 4) v.trigger(va); else v.vel = va;
            vizVoice = &v;
        } else {
            Voice* v = nullptr;
            for (auto& x : voices) if (x.active() && x.note == note) { v = &x; break; }
            if (!v) for (auto& x : voices) if (!x.active()) { v = &x; break; }
            if (!v) {
                Voice* best = nullptr;
                for (auto& x : voices) if (x.stage == 4 && (!best || x.env < best->env)) best = &x;
                if (!best) for (auto& x : voices) if (!best || x.age < best->age) best = &x;
                v = best;
            }
            v->setTarget(note, false);
            v->held = true;
            v->trigger(va);
            vizVoice = v;
        }
    }

    void noteOff(int note) {
        if (p[pMode] == 0) {
            Voice& v = voices[0];
            if (!stackRemove(note)) return;
            if (stackLen == 0) v.release();
            else if (v.note == note) v.setTarget(stack[stackLen - 1], true);
        } else {
            for (auto& x : voices) if (x.held && x.note == note) { x.held = false; x.release(); }
        }
    }

    void allNotesOff() {
        stackLen = 0;
        for (auto& x : voices) { x.held = false; x.release(); }
    }

    // host calls with any n; work happens in fixed BLOCK chunks
    void process(float* out, int n) {
        int pos = blkPos;
        for (int j = 0; j < n; ++j) {
            if (pos == BLOCK) { processBlock(blk, BLOCK); pos = 0; }
            out[j] = blk[pos++];
        }
        blkPos = pos;
    }

    // live view handoff (the wrapper copies these out after process)
    void vizPush(double g, double nAmp) {
        if (capLen < (int) capG.size()) { capG[capLen] = (float) g; capN[capLen] = (float) nAmp; ++capLen; }
    }
    void vizCycle() {
        ++capCycles;
        if (capCycles >= 3) {
            const int len = capLen;
            if (len > 16) {
                for (int b = 0; b < 256; ++b) {
                    const int i0 = (int) std::floor((double) b * len / 256);
                    const int i1 = std::max(i0 + 1, (int) std::floor((double) (b + 1) * len / 256));
                    float mx = 0;
                    for (int i = i0; i < i1; ++i) if (capN[i] > mx) mx = capN[i];
                    vizG[b] = capG[i0]; vizN[b] = mx;
                }
                vizF0 = vizVoice->lastF0; vizRd = vizVoice->lastRd; vizOq = vizVoice->lastOq;
                vizReady = true;
            }
            capLen = 0; capCycles = 1;
        }
    }

private:
    bool stackRemove(int note) {
        for (int i = 0; i < stackLen; ++i) if (stack[i] == note) {
            for (int k = i; k < stackLen - 1; ++k) stack[k] = stack[k + 1];
            --stackLen; return true;
        }
        return false;
    }

    static double ramp(float* arr, double a, double b, int n) {
        const double st = (b - a) / n;
        for (int j = 0; j < n; ++j) arr[j] = (float) (a + st * (j + 1));
        return b;
    }

    void updateBlock(int n) {
        const double c = smoothCoef;
        sVoicing += (p[pVoicing] - sVoicing) * c;
        sBreathy += (p[pBreathy] - sBreathy) * c;
        sTense += (p[pTense] - sTense) * c;
        sCreaky += (p[pCreaky] - sCreaky) * c;
        sThin += (p[pThin] - sThin) * c;
        sGrowl += (p[pGrowl] - sGrowl) * c;
        double ln = std::log(RD_MODAL) + sBreathy * std::log(2.7) + sTense * std::log(0.3)
                  + sCreaky * std::log(0.5) + sThin * std::log(2.0);
        const double lo = std::log(RD_LO), hi = std::log(RD_HI);
        lnRd = ln < lo ? lo : ln > hi ? hi : ln;
        sineMix = sThin * 0.85;
        sourceDeriv = p[pSource] == 1;
        pitchLog = (p[pTranspose] + p[pDetune] / 100) * LN2 / 12;
        const double vo = sVoicing;
        double bright = 0.35 + 0.55 * sTense - 0.3 * sBreathy + 0.25 * sThin;
        bright = bright < 0 ? 0 : bright > 1 ? 1 : bright;
        const double steady = (1 - vo) * 1.0 + vo * (sBreathy * 0.05 + sThin * 0.03);
        const double pulse = vo * (sBreathy * 0.3 + sThin * 0.12);
        cVoicing = ramp(rVoicing, cVoicing, vo, n);
        cSteady = ramp(rSteady, cSteady, steady, n);
        cPulse = ramp(rPulse, cPulse, pulse, n);
        cBright = ramp(rBright, cBright, bright, n);
        cGrowl = ramp(rGrowl, cGrowl, sGrowl, n);
        cVol = ramp(rVol, cVol, std::pow(10.0, p[pVolume] / 20), n);
    }

    void processBlock(float* out, int n) {
        updateBlock(n);
        std::fill(osBuf, osBuf + n * OS, 0.f);
        int count = 0;
        for (auto& v : voices) if (v.stage != 0) { v.render(n); ++count; }
        if (vizVoice && vizVoice->stage == 0) vizVoice = nullptr;
        vizVoices = count;
        const int L = FIR_LEN;
        double x1 = dcX, y1 = dcY;
        const double R = dcR;
        for (int j = 0, o = 0; j < n; ++j) {
            for (int q = 0; q < OS; ++q, ++o) {
                const float x = osBuf[o];
                hist[hw] = x; hist[hw + L] = x;
                hw = hw + 1 == L ? 0 : hw + 1;
            }
            double acc = 0;
            for (int t = 0; t < L; ++t) acc += (double) fir[t] * (double) hist[hw + t];
            const double y = acc - x1 + R * y1;
            x1 = acc; y1 = y;
            double s = y * rVol[j];
            if (s > 1) s = 1; else if (s < -1) s = -1;
            out[j] = (float) s;
        }
        dcX = x1; dcY = y1;
        if (std::abs(dcY) < 1e-20) dcY = 0;
    }

    friend class Voice;
};

/* ---------------- voice bodies ---------------- */

inline void Voice::setTarget(int n, bool glide) {
    const double target = std::log(440.0) + (n - 69) * LN2 / 12;
    note = n;
    const double gs = e->p[pGlide];
    if (glide && gs > 0 && hasPitch) {
        glideLeft = std::max(1LL, (long long) std::floor(gs * e->sr + 0.5));
        glideStep = (target - logF) / (double) glideLeft;
    } else {
        logF = target;
        glideLeft = 0;
    }
    hasPitch = true;
}

inline void Voice::trigger(double v) {
    vel = v;
    if (stage == 0) { cyc = 0; per = 0; cycleN = 0; env = 0; }
    stage = 1;
    age = ++e->ageCounter;
}

inline void Voice::startCycle() {
    const Tables& T = *e->T;
    const double* P = e->p;
    double f0 = std::exp(logF + e->pitchLog);
    if (f0 < 20) f0 = 20; else if (f0 > 2500) f0 = 2500;
    const long long k = cycleN++;
    double perN = e->srOS / f0;
    double a = 1;
    if (P[pJitter] > 0) perN *= 1 + P[pJitter] * 0.05 * gauss();
    const double growl = e->sGrowl, creak = e->sCreaky;
    if (growl > 0) {
        perN *= 1 + ((k & 1) ? -1 : 1) * growl * 0.12;
        if (k & 1) a *= 1 - 0.55 * growl;
    }
    double pl = perN;
    if (creak > 0) {
        pl = perN * (1 - 0.45 * creak);
        perN = perN * (1 + creak * 0.35 * std::abs(gauss()));
        if (rand() < creak * 0.3) perN *= 1.4 + rand() * 1.2 * creak;
        if (k & 1) a *= 1 - 0.5 * creak * (0.6 + 0.4 * rand());
        a *= 1 + 0.9 * creak;
    }
    if (P[pShimmer] > 0) a *= std::exp(P[pShimmer] * 0.25 * gauss());
    if (pl < 8) pl = 8;
    if (perN < pl) perN = pl;

    const double rdx = (std::exp(e->lnRd) - RD_LO) / RD_STEP;
    int r = (int) std::floor(rdx);
    if (r < 0) r = 0; else if (r > RD_COUNT - 2) r = RD_COUNT - 2;
    double f = rdx - r; if (f < 0) f = 0; else if (f > 1) f = 1;
    ri = r; rf = f; sm = e->sineMix;
    const bool deriv = e->sourceDeriv;
    const int lvl = mipLevel(pl);
    tab = deriv ? T.derivMip[lvl].data() : T.flowMip[lvl].data();
    subTab = deriv ? T.derivMip[mipLevel(perN * 2)].data() : T.flowMip[mipLevel(perN * 2)].data();
    sTab = deriv ? T.sDeriv.data() : T.sFlow.data();
    amp = a;
    inc = 1 / pl;
    pulse = pl;
    per = perN;
    lastF0 = e->srOS / perN;
    lastRd = std::exp(e->lnRd);
    const double oq = ((double) T.te[r] + ((double) T.te[r + 1] - (double) T.te[r]) * f) * (1 - sm) + sm;
    lastOq = oq * pl / perN;
    if (this == e->vizVoice) e->vizCycle();
}

inline void Voice::render(int n) {
    const Tables& T = *e->T;
    const double* P = e->p;
    float* buf = e->osBuf;
    const float* shapeT = T.shape.data();
    const float* sShape = T.sShape.data();
    const int subRow = e->subRow * TW;
    const bool isViz = this == e->vizVoice;
    const double sr = e->sr;
    const double aInc = 1 / (std::max(P[pAttack], 0.001) * sr);
    const double dCoef = std::exp(-4.6 / (std::max(P[pDecay], 0.001) * sr));
    const double rCoef = std::exp(-4.6 / (std::max(P[pRelease], 0.001) * sr));
    const double S = P[pSustain];
    const double a1 = e->nA1, a2 = e->nA2, a3 = e->nA3, ar = e->rA, roughG = e->roughGain;
    const double g1 = e->nG1, g2 = e->nG2;
    int o = 0;
    for (int j = 0; j < n; ++j) {
        switch (stage) {
            case 1: env += aInc; if (env >= 1) { env = 1; stage = 2; } break;
            case 2: env = S + (env - S) * dCoef; if (std::abs(env - S) < 1e-4) { env = S; stage = 3; } break;
            case 3: env = S; break;
            case 4: env *= rCoef; if (env < 1e-5) { env = 0; stage = 0; } break;
            default: break;
        }
        if (glideLeft > 0) { logF += glideStep; --glideLeft; }
        const double gain = env * vel;
        const double voicing = e->rVoicing[j], steady = e->rSteady[j], pulsed = e->rPulse[j],
                     bright = e->rBright[j], growl = e->rGrowl[j];
        const double subInc = std::exp(logF + e->pitchLog) * 0.5 / e->srOS;
        for (int q = 0; q < OS; ++q, ++o) {
            if (cyc >= per) {
                cyc -= per;
                startCycle();
                if (cyc >= per) cyc = 0;
            }
            double g = 0, sh = 0;
            if (cyc < pulse) {
                const double x = cyc * inc * TN;
                const int i = (int) x;
                const double fr = x - i;
                const int b0 = ri * TW + i, b1 = b0 + TW;
                const double v0 = tab[b0] + ((double) tab[b0 + 1] - tab[b0]) * fr;
                const double v1 = tab[b1] + ((double) tab[b1 + 1] - tab[b1]) * fr;
                double v = v0 + (v1 - v0) * rf;
                sh = shapeT[b0] + ((double) shapeT[b0 + 1] - shapeT[b0]) * fr;
                if (sm > 0) {
                    const double s = sTab[i] + ((double) sTab[i + 1] - sTab[i]) * fr;
                    v += (s - v) * sm;
                    const double ss = sShape[i] + ((double) sShape[i + 1] - sShape[i]) * fr;
                    sh += (ss - sh) * sm;
                }
                g = v * amp;
            }
            cyc += 1;
            const double w = rand() * 2 - 1;
            double rgh = 0;
            if (growl > 0) {
                rough += (w - rough) * ar;
                rgh = rough * roughG;
                if (rgh > 2.5) rgh = 2.5; else if (rgh < -2.5) rgh = -2.5;
                double gm = 1 + growl * 0.35 * rgh;
                if (gm < 0) gm = 0;
                g *= gm;
                subPos += subInc;
                if (subPos >= 1) subPos -= 1;
                const double sx = subPos * TN;
                const int si = (int) sx;
                const double sf = sx - si;
                const float* st = subTab ? subTab : T.flow.data();
                g += (st[subRow + si] + ((double) st[subRow + si + 1] - st[subRow + si]) * sf) * growl * 0.4 * (1 + 0.25 * rgh);
            }
            n1 += (w - n1) * a1;
            n2 += (w - n2) * a2;
            const double soft = n1 * g1, hi = (w - n2) * g2;
            n3 += (soft + (hi - soft) * bright - n3) * a3;
            const double noise = n3;
            const double nAmp = steady + pulsed * sh;
            const double voiced = g * voicing;
            buf[o] = (float) (buf[o] + (voiced + noise * nAmp) * gain);
            if (isViz) e->vizPush(voiced, nAmp);
        }
    }
}

}} // namespace bytebent::throat

#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif
