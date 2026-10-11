cmake_minimum_required(VERSION 3.22)
project(ScreenRec VERSION 0.5.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(ScreenRec
    COMPANY_NAME "Pouria Motabean"
    PLUGIN_MANUFACTURER_CODE Pmtb
    PLUGIN_CODE Scrc
    FORMATS VST3
    PRODUCT_NAME "ScreenRec"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Tools)

target_sources(ScreenRec PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Recorder.cpp
    Source/ScreenCapture.cpp
    Source/WinProcess.cpp
    Source/Timing.cpp
    Source/FrameOverlay.cpp
    Source/WinUtils.cpp)

juce_add_binary_data(ScreenRecAssets SOURCES
    Resources/plate.jpg
    Resources/knob.png)

target_compile_definitions(ScreenRec PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(ScreenRec
    PRIVATE
        ScreenRecAssets
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if (WIN32)
    target_link_libraries(ScreenRec PRIVATE user32 gdi32 winmm)
endif()
