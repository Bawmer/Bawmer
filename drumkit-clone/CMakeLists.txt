cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

project(DrumkitClone VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(DKC_COPY_AFTER_BUILD "Install the plugin into the user's plugin folder after building" OFF)
option(DKC_BUILD_TESTS "Build the analyser/processor self-test" ON)

# Use a local JUCE checkout with -DJUCE_PATH=/path/to/JUCE, otherwise fetch it.
if(DEFINED JUCE_PATH)
    add_subdirectory("${JUCE_PATH}" juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(DKC_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND DKC_FORMATS AU)
endif()

juce_add_plugin(DrumkitClone
    PRODUCT_NAME "Drumkit Clone"
    COMPANY_NAME "Bawmer"
    BUNDLE_ID "com.bawmer.drumkitclone"
    PLUGIN_MANUFACTURER_CODE Bawm
    PLUGIN_CODE Dkcl
    FORMATS ${DKC_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Sampler Drum
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD ${DKC_COPY_AFTER_BUILD})

target_sources(DrumkitClone PRIVATE
    Source/KitAnalyzer.cpp
        Source/MidiExport.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(DrumkitClone PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(DrumkitClone
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

if(DKC_BUILD_TESTS)
    # Builds the plugin sources straight into a console app: no plugin wrapper needed.
    juce_add_console_app(DrumkitCloneTests PRODUCT_NAME "DrumkitCloneTests")
    target_sources(DrumkitCloneTests PRIVATE
        Tests/Main.cpp
        Source/KitAnalyzer.cpp
        Source/MidiExport.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(DrumkitCloneTests PRIVATE Source)
    target_compile_definitions(DrumkitCloneTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(DrumkitCloneTests PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
