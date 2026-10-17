#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ThroatDSP.h"
#include <map>

// Factory presets live in the code; user presets are small XML files in
// ~/Music/Bytebent/Throat/Presets (values stored in real units, so they survive range tweaks).
// Everything here runs on the message thread.
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& s) : apvts (s) { snapshot(); }

    struct Factory { const char* name; std::vector<std::pair<const char*, float>> values; };

    // each preset starts from the defaults, then sets only what it lists
    static const std::vector<Factory>& factory()
    {
        static const std::vector<Factory> f {
            { "Init",           {} },
            { "Breathy",        { { "breathy", 0.7f }, { "attack", 0.08f } } },
            { "Pressed",        { { "tense", 0.8f }, { "source", 1.0f } } },
            { "Vocal Fry",      { { "creaky", 0.85f }, { "tense", 0.2f }, { "transpose", -12.0f } } },
            { "Falsetto",       { { "thin", 0.8f }, { "breathy", 0.2f }, { "transpose", 12.0f } } },
            { "Growl",          { { "growl", 0.75f }, { "tense", 0.4f }, { "jitter", 0.2f }, { "shimmer", 0.2f }, { "source", 1.0f } } },
            { "Whisper",        { { "voicing", 0.0f }, { "tense", 0.7f } } },
            { "Breath",         { { "voicing", 0.0f }, { "breathy", 0.8f } } },
            { "Murmur",         { { "voicing", 0.5f }, { "breathy", 0.6f } } },
            { "Rough",          { { "jitter", 0.6f }, { "shimmer", 0.5f }, { "creaky", 0.2f } } },
            { "Choir",          { { "mode", 1.0f }, { "breathy", 0.3f }, { "jitter", 0.15f }, { "shimmer", 0.15f },
                                  { "attack", 0.25f }, { "release", 0.8f } } },
        };
        return f;
    }

    static juce::File userFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Bytebent").getChildFile ("Throat").getChildFile ("Presets");
    }

    juce::StringArray factoryNames() const
    {
        juce::StringArray a;
        for (auto& p : factory()) a.add (p.name);
        return a;
    }

    juce::StringArray userNames() const
    {
        juce::StringArray a;
        for (auto& f : userFolder().findChildFiles (juce::File::findFiles, false, "*.throatpreset"))
            a.add (f.getFileNameWithoutExtension());
        a.sortNatural();
        return a;
    }

    juce::String currentName() const { return apvts.state.getProperty ("presetName", "Init").toString(); }
    juce::String currentKind() const { return apvts.state.getProperty ("presetKind", "factory").toString(); }

    bool loadFactory (const juce::String& name)
    {
        for (auto& p : factory())
            if (name == p.name)
            {
                std::map<juce::String, float> v;
                for (auto& kv : p.values) v[kv.first] = kv.second;
                apply (v);
                setCurrent (name, "factory");
                return true;
            }
        return false;
    }

    bool loadUser (const juce::String& name)
    {
        auto xml = juce::parseXML (fileFor (name));
        if (xml == nullptr || ! xml->hasTagName ("ThroatPreset")) return false;
        std::map<juce::String, float> v;
        for (auto* e : xml->getChildWithTagNameIterator ("Param"))
            v[e->getStringAttribute ("id")] = (float) e->getDoubleAttribute ("value");
        apply (v);
        setCurrent (name, "user");
        return true;
    }

    bool saveUser (const juce::String& rawName)
    {
        const auto name = juce::File::createLegalFileName (rawName.trim()).substring (0, 60);
        if (name.isEmpty()) return false;
        juce::XmlElement xml ("ThroatPreset");
        xml.setAttribute ("name", name);
        xml.setAttribute ("version", JucePlugin_VersionString);
        for (int i = 0; i < bytebent::throat::kNumParams; ++i)
        {
            const char* id = bytebent::throat::paramInfo (i).id;
            auto* e = xml.createNewChildElement ("Param");
            e->setAttribute ("id", id);
            e->setAttribute ("value", (double) apvts.getRawParameterValue (id)->load());
        }
        userFolder().createDirectory();
        if (! xml.writeTo (fileFor (name))) return false;
        setCurrent (name, "user");
        snapshot();   // what was just saved is the new reference for the change mark
        return true;
    }

    bool deleteUser (const juce::String& name)
    {
        const bool ok = fileFor (name).deleteFile();
        if (ok && currentKind() == "user" && currentName() == name)
            apvts.state.setProperty ("presetKind", "factory", nullptr);   // keeps the name, marks it unsaved
        return ok;
    }

    // prev/next through factory then user presets, wrapping around
    void step (int dir)
    {
        struct Entry { juce::String name; bool user; };
        std::vector<Entry> all;
        for (auto& n : factoryNames()) all.push_back ({ n, false });
        for (auto& n : userNames()) all.push_back ({ n, true });
        if (all.empty()) return;
        int idx = -1;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].name == currentName() && all[i].user == (currentKind() == "user")) idx = (int) i;
        const int n = (int) all.size();
        const int next = idx < 0 ? (dir > 0 ? 0 : n - 1) : ((idx + dir) % n + n) % n;
        const auto& e = all[(size_t) next];
        if (e.user) loadUser (e.name); else loadFactory (e.name);
    }

    // true once any parameter differs from what the preset (or the last load of the project) set
    bool isModified() const
    {
        for (auto& kv : loaded)
            if (std::abs (apvts.getRawParameterValue (kv.first)->load() - kv.second) > 1.0e-5f) return true;
        return false;
    }

    void snapshot()
    {
        loaded.clear();
        for (int i = 0; i < bytebent::throat::kNumParams; ++i)
        {
            const char* id = bytebent::throat::paramInfo (i).id;
            loaded[id] = apvts.getRawParameterValue (id)->load();
        }
    }

private:
    juce::File fileFor (const juce::String& name) const { return userFolder().getChildFile (name + ".throatpreset"); }

    void apply (const std::map<juce::String, float>& values)
    {
        for (int i = 0; i < bytebent::throat::kNumParams; ++i)
        {
            const auto& inf = bytebent::throat::paramInfo (i);
            auto* p = apvts.getParameter (inf.id);
            if (p == nullptr) continue;
            auto it = values.find (inf.id);
            const float v = it != values.end() ? it->second : (float) inf.def;
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (v));
            p->endChangeGesture();
        }
        snapshot();
    }

    void setCurrent (const juce::String& name, const juce::String& kind)
    {
        apvts.state.setProperty ("presetName", name, nullptr);
        apvts.state.setProperty ("presetKind", kind, nullptr);
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::map<juce::String, float> loaded;
};
