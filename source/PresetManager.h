#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ThroatDSP.h"
#include <map>

// Presets are small XML files in ~/Music/Bytebent/Throat/Presets (values stored in real units,
// so they survive range tweaks). There are no factory presets; "Init" just means nothing loaded.
// Everything here runs on the message thread.
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& s) : apvts (s) { snapshot(); }

    static juce::File userFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Bytebent").getChildFile ("Throat").getChildFile ("Presets");
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
    juce::String currentKind() const { return apvts.state.getProperty ("presetKind", "none").toString(); }

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
            apvts.state.setProperty ("presetKind", "none", nullptr);   // keeps the name, marks it unsaved
        return ok;
    }

    // prev/next through the saved presets, wrapping around
    void step (int dir)
    {
        const auto all = userNames();
        if (all.isEmpty()) return;
        const int idx = currentKind() == "user" ? all.indexOf (currentName()) : -1;
        const int n = all.size();
        const int next = idx < 0 ? (dir > 0 ? 0 : n - 1) : ((idx + dir) % n + n) % n;
        loadUser (all[next]);
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
