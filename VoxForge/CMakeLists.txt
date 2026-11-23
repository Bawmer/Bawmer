cmake_minimum_required(VERSION 3.22)

project(VoxForge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Static MSVC runtime: the .vst3 does not depend on the VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(VOXFORGE_WITH_ONNX  "Build the neural (ONNX Runtime) voice backend" OFF)
option(VOXFORGE_BUILD_TESTS "Build the headless engine tests" OFF)
option(VOXFORGE_STANDALONE "Also build a standalone app (handy for testing)" ON)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime package root (contains include/ and lib/)")
set(JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional, otherwise JUCE 8.0.6 is downloaded)")

#-------------------------------------------------------------------------------
# JUCE
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

#-------------------------------------------------------------------------------
set(VOXFORGE_FORMATS VST3)
if(VOXFORGE_STANDALONE)
    list(APPEND VOXFORGE_FORMATS Standalone)
endif()

juce_add_plugin(VoxForge
    COMPANY_NAME                "Bawmer"
    COMPANY_WEBSITE             "https://github.com/bawmer"
    BUNDLE_ID                   com.bawmer.voxforge
    PLUGIN_MANUFACTURER_CODE    Bawm
    PLUGIN_CODE                 VxFg
    FORMATS                     ${VOXFORGE_FORMATS}
    PRODUCT_NAME                "VoxForge"
    DESCRIPTION                 "AI vocal synth with voice cloning and vocal replacement"
    VERSION                     ${PROJECT_VERSION}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Instrument Synth
    VST3_AUTO_MANIFEST          FALSE   # moduleinfo.json is optional; the helper tool crashes on Windows runners
    COPY_PLUGIN_AFTER_BUILD     FALSE)

juce_generate_juce_header(VoxForge)

set(VOXFORGE_ENGINE_SOURCES
    Source/Core/VocalTypes.cpp
    Source/Core/Phonemes.cpp
    Source/Core/Lyrics.cpp
    Source/Core/Timeline.cpp
    Source/DSP/AudioUtils.cpp
    Source/DSP/FormantSynth.cpp
    Source/DSP/PitchDetector.cpp
    Source/DSP/VocalToMidi.cpp
    Source/DSP/Spectral.cpp
    Source/Engine/VoicePreset.cpp
    Source/Engine/VoiceAnalyzer.cpp
    Source/Engine/NeuralBackend.cpp
    Source/Engine/EngineClient.cpp
    Source/Engine/RenderEngine.cpp
    Source/Engine/StemAnalyzer.cpp)

target_sources(VoxForge PRIVATE
    ${VOXFORGE_ENGINE_SOURCES}
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UI/LookAndFeel.cpp
    Source/UI/PianoRoll.cpp
    Source/UI/EditorTab.cpp
    Source/UI/VoiceCloneTab.cpp
    Source/UI/PresetsTab.cpp
    Source/UI/ReplaceTab.cpp)

set(VOXFORGE_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_SILENCE_XCODE_15_LINKER_WARNING=1)

target_compile_definitions(VoxForge PUBLIC ${VOXFORGE_DEFINITIONS})

target_link_libraries(VoxForge
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#-------------------------------------------------------------------------------
# Optional neural backend (ONNX Runtime; on Windows the DirectML or CPU package)
function(voxforge_enable_onnx target)
    find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
        PATHS ${ONNXRUNTIME_ROOT}/include ${ONNXRUNTIME_ROOT}/build/native/include NO_DEFAULT_PATH)
    find_library(ORT_LIBRARY onnxruntime
        PATHS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/runtimes/win-x64/native NO_DEFAULT_PATH)
    if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
        message(FATAL_ERROR "VOXFORGE_WITH_ONNX=ON but ONNX Runtime was not found in ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}'")
    endif()
    target_include_directories(${target} PRIVATE ${ORT_INCLUDE_DIR})
    target_compile_definitions(${target} PUBLIC VOXFORGE_WITH_ONNX=1)
    target_link_libraries(${target} PUBLIC ${ORT_LIBRARY})
    if(MSVC)
        # delay-load so the plug-in can pre-load ITS onnxruntime.dll by full path
        # (hosts do not search the plug-in folder and System32 ships an old copy)
        get_target_property(_vf_type ${target} TYPE)
        if(_vf_type STREQUAL "EXECUTABLE")
            target_link_options(${target} PRIVATE "/DELAYLOAD:onnxruntime.dll")
        else()
            target_link_options(${target} INTERFACE "/DELAYLOAD:onnxruntime.dll")
        endif()
        target_link_libraries(${target} PUBLIC delayimp)
    endif()
    message(STATUS "VoxForge: neural backend enabled (${ORT_LIBRARY})")
endfunction()

if(VOXFORGE_WITH_ONNX)
    voxforge_enable_onnx(VoxForge)
    if(WIN32)
        file(GLOB ORT_DLLS "${ONNXRUNTIME_ROOT}/lib/*.dll" "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native/*.dll")
        foreach(fmt ${VOXFORGE_FORMATS})
            if(TARGET VoxForge_${fmt})
                foreach(dll ${ORT_DLLS})
                    add_custom_command(TARGET VoxForge_${fmt} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:VoxForge_${fmt}>")
                endforeach()
            endif()
        endforeach()
    endif()
endif()

#-------------------------------------------------------------------------------
# Headless tests of the engine (G2P, timeline, synth, Vocal-to-MIDI, analysis)
if(VOXFORGE_BUILD_TESTS)
    juce_add_console_app(VoxForgeTests PRODUCT_NAME "VoxForgeTests")
    juce_generate_juce_header(VoxForgeTests)
    target_sources(VoxForgeTests PRIVATE tests/EngineTests.cpp ${VOXFORGE_ENGINE_SOURCES})
    target_compile_definitions(VoxForgeTests PRIVATE ${VOXFORGE_DEFINITIONS} JUCE_USE_CURL=0)
    target_link_libraries(VoxForgeTests PRIVATE
        juce::juce_audio_formats juce::juce_dsp juce::juce_events juce::juce_data_structures
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(VOXFORGE_WITH_ONNX)
        voxforge_enable_onnx(VoxForgeTests)
    endif()
endif()

# Host smoke test: loads the built .vst3 like a DAW does
if(VOXFORGE_BUILD_TESTS)
    juce_add_console_app(VoxForgeHostTest PRODUCT_NAME "VoxForgeHostTest")
    juce_generate_juce_header(VoxForgeHostTest)
    target_sources(VoxForgeHostTest PRIVATE tests/HostSmokeTest.cpp)
    target_compile_definitions(VoxForgeHostTest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(VoxForgeHostTest PRIVATE
        juce::juce_audio_processors juce::juce_audio_utils juce::juce_gui_extra
        juce::juce_recommended_config_flags)
endif()

# Off-screen UI snapshots (docs)
if(VOXFORGE_BUILD_TESTS)
    juce_add_console_app(VoxForgeUiSnapshot PRODUCT_NAME "VoxForgeUiSnapshot")
    target_sources(VoxForgeUiSnapshot PRIVATE tests/UiSnapshot.cpp)
    if(JUCE_DIR)
        set(VF_JUCE_MODULES ${JUCE_DIR}/modules)
    else()
        set(VF_JUCE_MODULES ${juce_SOURCE_DIR}/modules)
    endif()
    target_include_directories(VoxForgeUiSnapshot PRIVATE ${CMAKE_BINARY_DIR}/VoxForge_artefacts/JuceLibraryCode ${VF_JUCE_MODULES})
    target_link_libraries(VoxForgeUiSnapshot PRIVATE VoxForge)
endif()
