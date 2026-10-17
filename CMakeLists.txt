cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(Throat VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is downloaded at configure time. To use a local copy instead:
#   cmake -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE ...
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(TH_FORMATS AU VST3 Standalone)
else()
    set(TH_FORMATS VST3 Standalone)
endif()

juce_add_plugin(Throat
    PRODUCT_NAME                "Throat"
    COMPANY_NAME                "Bytebent Audio"
    BUNDLE_ID                   "com.bytebent.throat"
    PLUGIN_MANUFACTURER_CODE    Bytb
    PLUGIN_CODE                 Thrt
    FORMATS                     ${TH_FORMATS}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    NEEDS_WEB_BROWSER           FALSE
    NEEDS_CURL                  FALSE
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    VST3_CATEGORIES             Instrument Synth
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources(Throat PRIVATE
    source/PluginProcessor.cpp)

target_compile_definitions(Throat PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Throat
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Command-line runner for the DSP core (see tests/compare.js).
add_executable(dsp_cli tests/dsp_cli.cpp)

# Wrapper test: processor vs bare engine, parameters, state (tests/plugin_test.cpp).
juce_add_console_app(plugin_test PRODUCT_NAME "plugin_test")
target_sources(plugin_test PRIVATE tests/plugin_test.cpp source/PluginProcessor.cpp)
target_compile_definitions(plugin_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="Throat")
target_link_libraries(plugin_test PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
