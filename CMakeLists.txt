cmake_minimum_required(VERSION 3.22)

project(LABX0 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime: the plugin then needs no VC++ redistributable on other machines.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(LABX0_JUCE_PATH "F:/_DEV/tools/JUCE" CACHE PATH "Path to a JUCE 9 checkout")
if(NOT EXISTS "${LABX0_JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${LABX0_JUCE_PATH}. Configure with -DLABX0_JUCE_PATH=<path to JUCE 9>.")
endif()
add_subdirectory("${LABX0_JUCE_PATH}" JUCE)

juce_add_plugin(LabX0
    PRODUCT_NAME "LAB X-0"
    VERSION 0.1.0
    COMPANY_NAME "OPERATION FAIRWAY, LLC"
    COMPANY_WEBSITE "https://github.com/crystalball-dev/lab-x-0"
    BUNDLE_ID com.operationfairway.labx0
    PLUGIN_MANUFACTURER_CODE Opfw
    PLUGIN_CODE Lbx0
    FORMATS VST3 Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Drum Sampler)

set(LABX0_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Library.cpp
    Source/SampleBank.cpp
    Source/AudioAnalysis.cpp
    Source/Pattern.cpp
    Source/FactoryPresets.cpp
    Source/UI/PatternGrid.cpp
    Source/UI/Browser.cpp)

set(LABX0_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_sources(LabX0 PRIVATE ${LABX0_SOURCES})
target_include_directories(LabX0 PRIVATE Source)
target_compile_definitions(LabX0 PUBLIC ${LABX0_DEFINITIONS})
target_compile_options(LabX0 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(LabX0
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline render and test harness: same sources, driven from the command line.
juce_add_console_app(LabX0Render PRODUCT_NAME "LabX0Render")
target_sources(LabX0Render PRIVATE Tests/RenderHarness.cpp ${LABX0_SOURCES})
target_include_directories(LabX0Render PRIVATE Source)
target_compile_definitions(LabX0Render PRIVATE ${LABX0_DEFINITIONS} LABX0_HARNESS=1)
target_compile_options(LabX0Render PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(LabX0Render
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
