// Plugin wrapper test: the JUCE processor must sound exactly like the bare engine fed the same
// notes and settings, map every parameter correctly, and restore its state.
// Build target: plugin_test (see CMakeLists.txt).
#include "../source/PluginProcessor.h"
#include <iostream>

namespace th = bytebent::throat;
static int fails = 0;
static void check (const char* name, bool ok, const juce::String& info = {})
{
    std::cout << (ok ? "PASS " : "FAIL ") << name << (info.isEmpty() ? "" : "  " + info) << std::endl;
    if (! ok) ++fails;
}

struct Ev { int at; int note; int vel; };   // vel 0 = note off

static void setParam (ThroatProcessor& p, const char* id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// render through the plugin (host blocks of `block`) and through the engine directly
static double compare (ThroatProcessor& p, const std::vector<Ev>& evs, int total, int block, double sr)
{
    p.setPlayConfigDetails (0, 2, sr, block);
    p.prepareToPlay (sr, block);
    th::Engine e; e.prepare (sr);
    for (int i = 0; i < th::kNumParams; ++i)
        e.setParam (i, p.apvts.getRawParameterValue (th::paramInfo (i).id)->load());

    std::vector<float> ref ((size_t) total);
    int pos = 0;
    for (auto& ev : evs)
    {
        if (ev.at > pos) { e.process (ref.data() + pos, ev.at - pos); pos = ev.at; }
        if (ev.vel > 0) e.noteOn (ev.note, ev.vel); else e.noteOff (ev.note);
    }
    if (pos < total) e.process (ref.data() + pos, total - pos);

    juce::AudioBuffer<float> buf (2, block);
    double maxd = 0, energy = 0;
    for (int start = 0; start < total; start += block)
    {
        const int n = std::min (block, total - start);
        juce::MidiBuffer midi;
        for (auto& ev : evs)
            if (ev.at >= start && ev.at < start + n)
                midi.addEvent (ev.vel > 0 ? juce::MidiMessage::noteOn (1, ev.note, (juce::uint8) ev.vel)
                                          : juce::MidiMessage::noteOff (1, ev.note), ev.at - start);
        buf.setSize (2, n, false, false, true);
        buf.clear();
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i)
        {
            const double a = buf.getSample (0, i), b = ref[(size_t) (start + i)];
            maxd = std::max (maxd, std::abs (a - b));
            maxd = std::max (maxd, (double) std::abs (buf.getSample (1, i) - buf.getSample (0, i)));
            energy += b * b;
        }
    }
    return energy > 1e-6 ? maxd : -1.0;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    {
        ThroatProcessor p;
        const double d = compare (p, { { 1000, 48, 100 }, { 20000, 48, 0 } }, 40000, 512, 48000);
        check ("defaults: plugin output = engine output, both channels", d == 0.0, "max diff " + juce::String (d));
    }
    {
        ThroatProcessor p;
        setParam (p, "breathy", 0.6f); setParam (p, "growl", 0.4f); setParam (p, "creaky", 0.3f);
        setParam (p, "transpose", -5); setParam (p, "detune", 23); setParam (p, "glide", 0.2f);
        setParam (p, "attack", 0.4f); setParam (p, "release", 1.5f); setParam (p, "volume", 2.5f);
        setParam (p, "source", 1); setParam (p, "jitter", 0.3f);
        const double d = compare (p, { { 300, 48, 90 }, { 7000, 55, 120 }, { 15000, 55, 0 }, { 26000, 48, 0 } }, 60000, 333, 44100);
        check ("mono with glide, many knobs, odd block size: identical to engine", d == 0.0, "max diff " + juce::String (d));
    }
    {
        ThroatProcessor p;
        setParam (p, "mode", 1); setParam (p, "thin", 0.7f); setParam (p, "shimmer", 0.5f);
        std::vector<Ev> evs;
        int n = 0;
        for (int note : { 48, 52, 55, 59, 62, 64, 67, 71, 74 }) evs.push_back ({ 100 + 37 * n++, note, 100 });
        for (int note : { 48, 52, 55, 59, 62, 64, 67, 71, 74 }) evs.push_back ({ 30000 + 11 * n++, note, 0 });
        const double d = compare (p, evs, 70000, 128, 96000);
        check ("poly chord of 9 with stealing at 96 kHz: identical to engine", d == 0.0, "max diff " + juce::String (d));
    }
    {
        ThroatProcessor a;
        setParam (a, "tense", 0.8f); setParam (a, "mode", 1); setParam (a, "source", 1); setParam (a, "glide", 0.5f);
        setParam (a, "transpose", 7); setParam (a, "sustain", 0.25f); setParam (a, "volume", -12.0f);
        juce::MemoryBlock mb; a.getStateInformation (mb);
        ThroatProcessor b; b.setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        juce::String bad;
        for (int i = 0; i < th::kNumParams; ++i)
        {
            const char* id = th::paramInfo (i).id;
            const float x = a.apvts.getRawParameterValue (id)->load(), y = b.apvts.getRawParameterValue (id)->load();
            if (std::abs (x - y) > 1e-6f) { same = false; bad << id << " "; }
        }
        check ("state saves and restores every parameter", same, bad);
        const float g = b.apvts.getRawParameterValue ("glide")->load();
        check ("glide range round-trips through the host's 0-1 value", std::abs (g - 0.5f) < 1e-4f, juce::String (g));
    }
    {
        ThroatProcessor p;
        bool ok = true;
        for (int i = 0; i < th::kNumParams; ++i)
        {
            const auto& inf = th::paramInfo (i);
            auto* prm = p.apvts.getParameter (inf.id);
            const float lo = prm->convertFrom0to1 (0.0f), hi = prm->convertFrom0to1 (1.0f), def = p.apvts.getRawParameterValue (inf.id)->load();
            if (std::abs (lo - inf.min) > 1e-5 || std::abs (hi - inf.max) > 1e-5 || std::abs (def - inf.def) > 1e-5)
            { ok = false; std::cout << "  " << inf.id << " " << lo << ".." << hi << " def " << def << std::endl; }
        }
        check ("every parameter's range and default match the engine", ok);
    }

    std::cout << (fails ? juce::String (fails) + " FAILED" : juce::String ("all passed")) << std::endl;
    return fails ? 1 : 0;
}
