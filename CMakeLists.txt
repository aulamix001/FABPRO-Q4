cmake_minimum_required(VERSION 3.22)

project(ProQ3Clone VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Caminho para o JUCE 8.0.4 (pode ser passado via -DJUCE_DIR="C:/JUCE" ou colocar a pasta JUCE ao lado/dentro do projeto)
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" JUCE_build EXCLUDE_FROM_ALL)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/JUCE" JUCE_build EXCLUDE_FROM_ALL)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" JUCE_build EXCLUDE_FROM_ALL)
elseif(EXISTS "C:/JUCE/CMakeLists.txt")
    add_subdirectory("C:/JUCE" JUCE_build EXCLUDE_FROM_ALL)
else()
    find_package(JUCE CONFIG REQUIRED)
endif()

juce_add_plugin(ProQ3Clone
    COMPANY_NAME "SC Audio"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE ScAu
    PLUGIN_CODE Pq3C
    FORMATS VST3 Standalone
    PRODUCT_NAME "FABPRO-Q4"
)

target_sources(ProQ3Clone
    PRIVATE
        Source/PluginProcessor.h
        Source/PluginProcessor.cpp
        Source/PluginEditor.h
        Source/PluginEditor.cpp
)

target_compile_definitions(ProQ3Clone
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        _USE_MATH_DEFINES=1
)

if(MSVC)
    target_compile_options(ProQ3Clone PRIVATE /W4 /MP /permissive- /Zc:__cplusplus /utf-8)
endif()

target_link_libraries(ProQ3Clone
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
