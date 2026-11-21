cmake_minimum_required(VERSION 3.22)
project(LumenMC64 VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(LUMEN_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/Standalone)" ON)
option(LUMEN_BUILD_TESTS  "Build the DSP test suite" ON)

# --- JUCE-independent, header-only DSP core -------------------------------------------------
add_library(lumen_dsp INTERFACE)
target_include_directories(lumen_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)
target_compile_features(lumen_dsp INTERFACE cxx_std_17)

# --- tests ----------------------------------------------------------------------------------
if(LUMEN_BUILD_TESTS)
    enable_testing()
    add_executable(lumen_dsp_tests tests/TestMain.cpp)
    target_link_libraries(lumen_dsp_tests PRIVATE lumen_dsp)
    if(MSVC)
        target_compile_options(lumen_dsp_tests PRIVATE /W4 /O2 /fp:precise)
    else()
        target_compile_options(lumen_dsp_tests PRIVATE -Wall -Wextra -O2)
    endif()
    add_test(NAME lumen_dsp_tests COMMAND lumen_dsp_tests)
endif()

# --- plugin ---------------------------------------------------------------------------------
if(LUMEN_BUILD_PLUGIN)
    include(FetchContent)
    # Use a local checkout with -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE to avoid downloading.
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)

    set(LUMEN_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND LUMEN_FORMATS AU)
    endif()

    juce_add_plugin(LumenMC64
        COMPANY_NAME "Lumen Audio"
        BUNDLE_ID com.lumenaudio.mc64
        PLUGIN_MANUFACTURER_CODE Lmna
        PLUGIN_CODE Lm64
        PRODUCT_NAME "Lumen MC-64"
        FORMATS ${LUMEN_FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES Fx Dynamics Mastering
        AU_MAIN_TYPE kAudioUnitType_Effect)

    target_sources(LumenMC64 PRIVATE
        plugin/Source/Parameters.cpp
        plugin/Source/PresetManager.cpp
        plugin/Source/PluginProcessor.cpp
        plugin/Source/PluginEditor.cpp
        plugin/Source/LookAndFeel.cpp
        plugin/Source/Displays.cpp)

    target_compile_definitions(LumenMC64 PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    # The DSP headers are compiled with -Wall -Wextra by the test target; JUCE's stricter
    # warning set (-Wfloat-equal flags intentional exact-change checks) is not applied to them.
    target_include_directories(LumenMC64 SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)

    target_link_libraries(LumenMC64
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

# --- developer tool: render the UI with live audio to a PNG ---------------------------------
option(LUMEN_BUILD_UI_SNAPSHOT "Build the UI snapshot developer tool" OFF)
if(LUMEN_BUILD_PLUGIN AND LUMEN_BUILD_UI_SNAPSHOT)
    juce_add_console_app(lumen_ui_snapshot PRODUCT_NAME "lumen_ui_snapshot")
    target_sources(lumen_ui_snapshot PRIVATE tools/UiSnapshot.cpp
        plugin/Source/Parameters.cpp plugin/Source/PresetManager.cpp plugin/Source/PluginProcessor.cpp
        plugin/Source/PluginEditor.cpp plugin/Source/LookAndFeel.cpp plugin/Source/Displays.cpp)
    target_include_directories(lumen_ui_snapshot SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)
    target_compile_definitions(lumen_ui_snapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Lumen MC-64" JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(lumen_ui_snapshot PRIVATE juce::juce_audio_utils juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()

# --- plugin-level tests (state, undo, presets, A/B, threading); headless -------------------
if(LUMEN_BUILD_PLUGIN AND LUMEN_BUILD_TESTS)
    juce_add_console_app(lumen_plugin_tests PRODUCT_NAME "lumen_plugin_tests")
    target_sources(lumen_plugin_tests PRIVATE tools/PluginTests.cpp
        plugin/Source/Parameters.cpp plugin/Source/PresetManager.cpp plugin/Source/PluginProcessor.cpp
        plugin/Source/PluginEditor.cpp plugin/Source/LookAndFeel.cpp plugin/Source/Displays.cpp)
    target_include_directories(lumen_plugin_tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)
    target_compile_definitions(lumen_plugin_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Lumen MC-64" JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1) # tests pump the message loop to run async callbacks
    target_link_libraries(lumen_plugin_tests PRIVATE juce::juce_audio_utils juce::juce_dsp
        juce::juce_recommended_config_flags)
    add_test(NAME lumen_plugin_tests COMMAND lumen_plugin_tests)
endif()
