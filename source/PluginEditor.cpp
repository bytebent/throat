#include "PluginEditor.h"
#include "BinaryData.h"

ThroatEditor::ThroatEditor (ThroatProcessor& p)
    : AudioProcessorEditor (p),
      proc (p),
      browser (juce::WebBrowserComponent::Options {}
                   .withNativeIntegrationEnabled()
                   .withInitialisationData ("version", juce::String (JucePlugin_VersionString))
                   .withResourceProvider ([] (const juce::String& url) { return getResource (url); })
                   .withEventListener ("setParam", [this] (const juce::var& v) { onSetParam (v); })
                   .withEventListener ("gesture", [this] (const juce::var& v) { onGesture (v); })
                   .withEventListener ("uiReady", [this] (const juce::var&) { needFullSync = true; }))
{
    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    setSize (width, height);
    proc.editorOpen = true;
    startTimerHz (30);
}

ThroatEditor::~ThroatEditor()
{
    stopTimer();
    proc.editorOpen = false;
    for (auto& id : openGestures)
        if (auto* param = proc.apvts.getParameter (id))
            param->endChangeGesture();
}

void ThroatEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xffdcdce0));   // platinum, shown for a moment while the page loads
}

void ThroatEditor::resized()
{
    browser.setBounds (getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> ThroatEditor::getResource (const juce::String& url)
{
    const auto path = url.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false);
    if (path.isEmpty() || path == "index.html")
    {
        const auto* bytes = reinterpret_cast<const std::byte*> (BinaryData::index_html);
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (bytes, bytes + BinaryData::index_htmlSize), "text/html; charset=utf-8" };
    }
    return std::nullopt;
}

void ThroatEditor::onSetParam (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const float value = juce::jlimit (0.0f, 1.0f, (float) (double) v.getProperty ("value", 0.0));

    const bool inGesture = openGestures.count (id) > 0;
    if (! inGesture) param->beginChangeGesture();
    param->setValueNotifyingHost (value);
    if (! inGesture) param->endChangeGesture();
    lastSent[id] = param->getValue();   // don't echo our own change back
}

void ThroatEditor::onGesture (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const bool begin = (bool) v.getProperty ("begin", false);
    if (begin && openGestures.insert (id).second) param->beginChangeGesture();
    if (! begin && openGestures.erase (id) > 0) param->endChangeGesture();
}

void ThroatEditor::timerCallback()
{
    // 1) parameter values the page doesn't know yet (automation, presets, undo, first load)
    for (auto* p : proc.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr) continue;
        const auto id = rp->getParameterID();
        const float value = rp->getValue();
        auto it = lastSent.find (id);
        if (needFullSync || it == lastSent.end() || std::abs (it->second - value) > 1.0e-6f)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id", id);
            obj->setProperty ("value", value);
            browser.emitEventIfBrowserIsVisible ("param", juce::var (obj));
            lastSent[id] = value;
        }
    }
    needFullSync = false;

    // 2) live view: last two glottal cycles, readouts, and recent output for the spectrum
    auto& v = proc.view;
    juce::Array<juce::var> g, n, samples;
    g.ensureStorageAllocated (ThroatView::N);
    n.ensureStorageAllocated (ThroatView::N);
    for (int i = 0; i < ThroatView::N; ++i)
    {
        g.add (v.g[(size_t) i].load (std::memory_order_relaxed));
        n.add (v.n[(size_t) i].load (std::memory_order_relaxed));
    }
    const int pos = v.ringPos.load (std::memory_order_acquire);
    samples.ensureStorageAllocated (ThroatView::RING);
    for (int i = 0; i < ThroatView::RING; ++i)
        samples.add (v.ring[(size_t) ((pos + i) % ThroatView::RING)].load (std::memory_order_relaxed));

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("g", g);
    obj->setProperty ("n", n);
    obj->setProperty ("samples", samples);
    obj->setProperty ("f0", v.f0.load());
    obj->setProperty ("rd", v.rd.load());
    obj->setProperty ("oq", v.oq.load());
    obj->setProperty ("voices", v.voices.load());
    obj->setProperty ("sampleRate", proc.currentSampleRate.load());
    browser.emitEventIfBrowserIsVisible ("view", juce::var (obj));
}
