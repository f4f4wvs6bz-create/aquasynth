cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
endif()

project(AquaSynth VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: use a local copy in ./JUCE if present, otherwise download it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

juce_add_plugin(AquaSynth
    COMPANY_NAME "Shohei"
    BUNDLE_ID "com.shohei.aquasynth"
    PLUGIN_MANUFACTURER_CODE Shsg
    PLUGIN_CODE Aqsy
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "AquaSynth"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    VST3_CATEGORIES Instrument Synth)

# Zstandard decoder (BSD licence) for reading .SerumPreset files
file(GLOB ZSTD_SOURCES ThirdParty/zstd/common/*.c ThirdParty/zstd/decompress/*.c)
add_library(zstd_decoder STATIC ${ZSTD_SOURCES})
target_include_directories(zstd_decoder PUBLIC ThirdParty/zstd ThirdParty/zstd/common)
target_compile_definitions(zstd_decoder PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD=0 ZSTD_LEGACY_SUPPORT=0)
set_target_properties(zstd_decoder PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

juce_add_binary_data(AquaSynthData SOURCES Resources/Doto-Black-Round.ttf)
set_target_properties(AquaSynthData PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_sources(AquaSynth PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SerumImport.cpp)

target_compile_definitions(AquaSynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(AquaSynth
    PRIVATE
        AquaSynthData
        zstd_decoder
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline test: renders audio files and a screenshot of the panel (used before each release).
option(AQUASYNTH_TESTS "Build the render test" OFF)
if(AQUASYNTH_TESTS)
    add_executable(RenderTest tests/RenderTest.cpp)
    target_include_directories(RenderTest PRIVATE $<TARGET_PROPERTY:AquaSynth,INCLUDE_DIRECTORIES>)
    target_compile_definitions(RenderTest PRIVATE $<TARGET_PROPERTY:AquaSynth,COMPILE_DEFINITIONS>)
    target_link_libraries(RenderTest PRIVATE AquaSynth AquaSynthData zstd_decoder)
endif()
