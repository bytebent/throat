// Plugin wrapper test: the JUCE processor must sound exactly like the bare engine fed the same
// notes and settings, map every parameter correctly, and restore its state.
// Build target: plugin_test (see CMakeLists.txt).
#include "../source/PluginProcessor.h"
#include "../source/PluginEditor.h"
#include <iostream>

namespace th = bytebent::throat;
static int fails = 0;
static void check (const char* name, bool ok, const juce::String& info = {})
{
    std::cout << (ok ? "PASS " : "FAIL ") << name << (info.isEmpty() ? "" : "  " + info) << std::endl;
    if (! ok) ++fails;
}

struct Ev { int at; int note; int vel; };   // vel 0 = note off
static bool exactlyZero (double d) { return d >= 0.0 && d <= 0.0; }   // -1 means the render was silent

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


// ---- the web interface, end to end: host → page and page → host ----
static juce::var evalJs (juce::WebBrowserComponent& b, const juce::String& js, std::function<void()> pump)
{
    juce::var result; bool done = false;
    b.evaluateJavascript (js, [&] (juce::WebBrowserComponent::EvaluationResult r) {
        if (auto* v = r.getResult()) result = *v;
        done = true; });
    for (int i = 0; i < 200 && ! done; ++i) pump();
    return result;
}

static void editorTest()
{
    ThroatProcessor p;
    p.setPlayConfigDetails (0, 2, 48000, 480);
    p.prepareToPlay (48000, 480);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->addToDesktop (0);
    ed->setVisible (true);
    auto& browser = dynamic_cast<ThroatEditor&> (*ed).getBrowser();

    juce::AudioBuffer<float> buf (2, 480);
    bool noteSent = false;
    auto pump = [&]
    {
        juce::MidiBuffer midi;
        if (! noteSent) { midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100), 0); noteSent = true; }
        buf.clear();
        p.processBlock (buf, midi);                       // 10 ms of audio per pump
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    };
    for (int i = 0; i < 300; ++i) pump();                 // page loads, frames start flowing

    setParam (p, "breathy", 0.6f);
    for (int i = 0; i < 30; ++i) pump();
    const auto breathyShown = evalJs (browser, "[...document.querySelectorAll('.knob')].find(k=>k.querySelector('.name').textContent==='Breathy').querySelector('.val').textContent", pump).toString();
    check ("page shows a host-side parameter change", breathyShown == "60%", breathyShown);

    const auto ro = evalJs (browser, "[document.getElementById('roF0').textContent, document.getElementById('roVo').textContent].join('|')", pump).toString();
    check ("live view receives frames (pitch and voice count)", ro.startsWith ("110.0 Hz|1"), ro);

    evalJs (browser, "(()=>{const d=[...document.querySelectorAll('.knob')].find(k=>k.querySelector('.name').textContent==='Tense').querySelector('.dial'); for(let i=0;i<5;i++) d.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true})); return 1;})()", pump);
    evalJs (browser, "(document.querySelector('#segSrc button[data-v=\"1\"]').click(), document.querySelector('#segMode button[data-v=\"1\"]').click(), 1)", pump);
    for (int i = 0; i < 30; ++i) pump();
    const float tense = p.apvts.getRawParameterValue ("tense")->load();
    check ("turning a knob on the page changes the plugin parameter", std::abs (tense - 0.1f) < 1e-4f, juce::String (tense));
    check ("page switches set Source and Voice mode",
           p.apvts.getRawParameterValue ("source")->load() > 0.5f && p.apvts.getRawParameterValue ("mode")->load() > 0.5f);
    if (std::getenv ("THROAT_HOLD") != nullptr)            // keep the window up for a screenshot
    {
        setParam (p, "growl", 0.3f);
        juce::MidiBuffer chord;
        for (int nn : { 45, 52, 57 }) chord.addEvent (juce::MidiMessage::noteOn (1, nn, (juce::uint8) 100), 0);
        buf.clear(); p.processBlock (buf, chord);
        for (int i = 0; i < 600; ++i) pump();
    }
    ed = nullptr;
}

// ---- presets: factory values, user files, the page's preset bar, project state ----
static void presetTest()
{
    {
        ThroatProcessor p;
        auto& pm = p.presets;
        check ("factory list starts with Init and has all phonation presets", pm.factoryNames().size() == 11 && pm.factoryNames()[0] == "Init");
        setParam (p, "jitter", 0.9f);
        pm.loadFactory ("Vocal Fry");
        auto val = [&] (const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        check ("factory preset sets its values and resets the rest to defaults",
               std::abs (val ("creaky") - 0.85f) < 1e-4f && std::abs (val ("transpose") + 12.0f) < 1e-4f && val ("jitter") < 1e-6f && std::abs (val ("voicing") - 1.0f) < 1e-6f);
        check ("not marked modified right after loading", ! pm.isModified());
        setParam (p, "breathy", 0.3f);
        check ("marked modified after a knob moves", pm.isModified());
    }

    ThroatProcessor p;
    p.setPlayConfigDetails (0, 2, 48000, 480);
    p.prepareToPlay (48000, 480);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->addToDesktop (0);
    ed->setVisible (true);
    auto& browser = dynamic_cast<ThroatEditor&> (*ed).getBrowser();
    juce::AudioBuffer<float> buf (2, 480);
    auto pump = [&] { juce::MidiBuffer m; buf.clear(); p.processBlock (buf, m); juce::MessageManager::getInstance()->runDispatchLoopUntil (10); };
    for (int i = 0; i < 300; ++i) pump();
    auto js = [&] (const juce::String& code) { auto r = evalJs (browser, code, pump); for (int i = 0; i < 25; ++i) pump(); return r; };
    auto val = [&] (const char* id) { return p.apvts.getRawParameterValue (id)->load(); };

    // save through the page: Save, type a name, Enter
    setParam (p, "breathy", 0.42f); setParam (p, "growl", 0.25f); setParam (p, "mode", 1);
    js ("document.getElementById('pSave').click(), 1");
    js ("(()=>{const i=document.querySelector('#pName input'); i.value='My Voice'; i.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true})); return 1;})()");
    const auto file = PresetManager::userFolder().getChildFile ("My Voice.throatpreset");
    check ("Save on the page writes a user preset file", file.existsAsFile(), file.getFullPathName());
    check ("page shows the saved name without a change mark",
           js ("document.getElementById('pTxt').textContent + '|' + document.getElementById('pMod').hidden").toString() == "My Voice|true");

    // load a factory preset from the menu, then the user preset back
    js ("(document.getElementById('pName').click(), [...document.querySelectorAll('#pMenu .it')].find(e=>e.textContent==='Whisper').click(), 1)");
    check ("menu loads a factory preset", val ("voicing") < 1e-6f && std::abs (val ("tense") - 0.7f) < 1e-4f && val ("mode") < 0.5f);
    js ("(document.getElementById('pName').click(), [...document.querySelectorAll('#pMenu .it')].find(e=>e.textContent==='My Voice').click(), 1)");
    check ("menu loads the user preset with its values",
           std::abs (val ("breathy") - 0.42f) < 1e-4f && std::abs (val ("growl") - 0.25f) < 1e-4f && val ("mode") > 0.5f && std::abs (val ("voicing") - 1.0f) < 1e-6f);

    // change mark
    setParam (p, "thin", 0.5f);
    for (int i = 0; i < 10; ++i) pump();
    check ("page marks the preset as changed", js ("String(document.getElementById('pMod').hidden)").toString() == "false");

    // arrows: next from the last user preset wraps to Init, previous goes back
    js ("(document.getElementById('pNext').click(), 1)");
    check ("next arrow wraps to the first factory preset", p.presets.currentName() == "Init" && val ("breathy") < 1e-6f, p.presets.currentName());
    js ("(document.getElementById('pPrev').click(), 1)");
    check ("previous arrow goes back to the user preset", p.presets.currentName() == "My Voice", p.presets.currentName());

    // the preset name is saved with the project
    juce::MemoryBlock mb; p.getStateInformation (mb);
    ThroatProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
    check ("project state keeps the preset name", q.presets.currentName() == "My Voice" && q.presets.currentKind() == "user");

    // delete from the menu
    js ("(document.getElementById('pName').click(), [...document.querySelectorAll('#pMenu .it')].find(e=>e.textContent==='My Voice').querySelector('.del').click(), 1)");
    check ("delete button removes the user preset file", ! file.existsAsFile());
    const auto menuNow = js ("[document.getElementById('pMenu').hidden, [...document.querySelectorAll('#pMenu .it')].map(e=>e.textContent).join(',')].join('|')").toString();
    check ("menu no longer lists it", ! menuNow.contains ("My Voice"), menuNow);
    ed = nullptr;
}

int main()
{
    // keep test presets out of the real ~/Music
    const auto home = juce::File::createTempFile ("throat-home");
    home.createDirectory();
    setenv ("HOME", home.getFullPathName().toRawUTF8(), 1);
    juce::ScopedJuceInitialiser_GUI init;

    {
        ThroatProcessor p;
        const double d = compare (p, { { 1000, 48, 100 }, { 20000, 48, 0 } }, 40000, 512, 48000);
        check ("defaults: plugin output = engine output, both channels", exactlyZero (d), "max diff " + juce::String (d));
    }
    {
        ThroatProcessor p;
        setParam (p, "breathy", 0.6f); setParam (p, "growl", 0.4f); setParam (p, "creaky", 0.3f);
        setParam (p, "transpose", -5); setParam (p, "detune", 23); setParam (p, "glide", 0.2f);
        setParam (p, "attack", 0.4f); setParam (p, "release", 1.5f); setParam (p, "volume", 2.5f);
        setParam (p, "source", 1); setParam (p, "jitter", 0.3f);
        const double d = compare (p, { { 300, 48, 90 }, { 7000, 55, 120 }, { 15000, 55, 0 }, { 26000, 48, 0 } }, 60000, 333, 44100);
        check ("mono with glide, many knobs, odd block size: identical to engine", exactlyZero (d), "max diff " + juce::String (d));
    }
    {
        ThroatProcessor p;
        setParam (p, "mode", 1); setParam (p, "thin", 0.7f); setParam (p, "shimmer", 0.5f);
        std::vector<Ev> evs;
        int n = 0;
        for (int note : { 48, 52, 55, 59, 62, 64, 67, 71, 74 }) evs.push_back ({ 100 + 37 * n++, note, 100 });
        for (int note : { 48, 52, 55, 59, 62, 64, 67, 71, 74 }) evs.push_back ({ 30000 + 11 * n++, note, 0 });
        const double d = compare (p, evs, 70000, 128, 96000);
        check ("poly chord of 9 with stealing at 96 kHz: identical to engine", exactlyZero (d), "max diff " + juce::String (d));
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

    editorTest();
    presetTest();

    std::cout << (fails ? juce::String (fails) + " FAILED" : juce::String ("all passed")) << std::endl;
    return fails ? 1 : 0;
}
