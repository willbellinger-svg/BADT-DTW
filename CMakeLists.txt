# =============================================================================
# CMakeLists.txt  —  BADT Audio Plugin
# =============================================================================
#
# CMake is a "meta-build system": it doesn't compile your code itself, but
# generates project files for another build system (in our case, Visual Studio).
#
# Workflow:
#   1. Run CMake to CONFIGURE  →  generates a .sln (Visual Studio solution)
#   2. Open the .sln in Visual Studio 2022 (or build from the VS Code task)
#   3. Hit Build in Visual Studio  →  produces the actual .vst3 plugin file
#
# =============================================================================

# The minimum version of CMake we require.
# 3.22 is needed for full JUCE 8 CMake support.
cmake_minimum_required(VERSION 3.22)

# Declare the project name and version.
# VERSION uses semver format: major.minor.patch
project(BADT VERSION 1.0.0)

# =============================================================================
# JUCE SETUP
# =============================================================================
#
# JUCE is the framework that handles all the low-level audio plugin plumbing:
# connecting to VST3/AU formats, handling DAW communication, the GUI toolkit, etc.
#
# We tell CMake where JUCE lives on disk and add it as a subdirectory.
# "add_subdirectory" means: "go into that folder and process its CMakeLists.txt too".
# The second argument is where CMake puts JUCE's build output (inside our build folder).

set(JUCE_PATH "C:/Users/Will/Documents/JUCE" CACHE PATH
    "Path to your JUCE installation. Change this if you moved JUCE.")

add_subdirectory(${JUCE_PATH} "${CMAKE_CURRENT_BINARY_DIR}/JUCE")

# =============================================================================
# RUBBER BAND LIBRARY (Pitch Shifting)
# =============================================================================
#
# Rubber Band is a high-quality pitch-shifting and time-stretching library.
# We use the "single-file build" — one .cpp file that compiles the whole library
# using its built-in FFT and resampler, with no external dependencies.
#
# The file must stay at its original location relative to the rest of the
# Rubber Band source tree so its internal relative #includes resolve correctly.

set(RUBBERBAND_PATH "ThirdParty/rubberband-4.0.0")

# =============================================================================
# BINARY DATA (embedded fonts, images, etc.)
# =============================================================================
juce_add_binary_data(BADT_BinaryData
    SOURCES ThirdParty/fonts/digital-7.ttf
)

# =============================================================================
# PLUGIN DEFINITION
# =============================================================================
#
# juce_add_plugin() is the main JUCE CMake command. It declares our plugin target
# and sets a huge number of properties that control how the plugin is registered
# with DAWs (Digital Audio Workstations like Ableton, Reaper, FL Studio, etc.).

juce_add_plugin(BADT

    # Human-readable name shown in DAWs and in the window title bar
    PRODUCT_NAME "BADT"

    # The company name shown in DAWs
    COMPANY_NAME "RGS"

    # PLUGIN_MANUFACTURER_CODE: A 4-character ID for your company.
    # Must have at least one uppercase letter. Must be unique across all plugins.
    # (Lowercase characters can be used; Apple uses uppercase for their plugins.)
    PLUGIN_MANUFACTURER_CODE Wllb

    # PLUGIN_CODE: A 4-character ID unique to THIS plugin within your company.
    # Rules: exactly one uppercase character.
    PLUGIN_CODE Bdtv

    # The plugin formats to build.
    # VST3 — the modern standard, works in almost every DAW on Windows/Mac/Linux.
    # AU   — Audio Unit, macOS/iOS only.  JUCE skips this silently on Windows/Linux
    #        so listing it here is safe cross-platform: it will be built automatically
    #        when you configure on a Mac, with no code changes needed.
    # AAX  — Pro Tools (requires a separate Avid AAX SDK + signing — omitted for now).
    # Standalone was removed: it adds build time and we don't need a standalone app.
    FORMATS VST3 AU

    # This plugin processes audio (it is NOT a synthesizer/instrument)
    IS_SYNTH FALSE

    # We don't use MIDI at all
    NEEDS_MIDI_INPUT  FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT    FALSE

    # The GUI does not need keyboard focus (our knobs use mouse-dragging, not typing)
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE

    # Automatically copy the built plugin to the default VST3 folder after building.
    # On Windows this is: C:\Users\<You>\AppData\Local\Programs\Common\VST3\
    # Set to FALSE if you'd rather copy it manually.
    # FALSE: don't try to copy to C:\Program Files\Common Files\VST3\ (requires admin rights).
    # The built plugin lives at:
    #   build\BADT_artefacts\Debug\VST3\BADT.vst3
    # Point your DAW's VST3 scan path to that folder to load it.
    COPY_PLUGIN_AFTER_BUILD FALSE
)

# =============================================================================
# JUCEHEADER.H GENERATION
# =============================================================================
#
# This command tells JUCE to generate a file called JuceHeader.h in the build
# folder at configure time. Our source files use #include <JuceHeader.h> as a
# convenient single header that pulls in all the JUCE modules we've linked.
#
# Without this line, the compiler can't find JuceHeader.h and build fails with:
#   "C1083: Cannot open include file: 'JuceHeader.h': No such file or directory"
#
# What it does:
#   1. Creates  build/BADT_artefacts/JuceLibraryCode/JuceHeader.h
#   2. Adds that directory to the compiler's include search path automatically
#
juce_generate_juce_header(BADT)

# =============================================================================
# SOURCE FILES
# =============================================================================
#
# Tell CMake which .cpp files to compile into our plugin.
# .h header files don't need to be listed here (they're included via #include).
# PRIVATE means these sources are only for this target, not exported to other targets.

target_sources(BADT
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        # Rubber Band Library — single-file build (built-in FFT + resampler, no extra deps)
        ${RUBBERBAND_PATH}/single/RubberBandSingle.cpp
)

# =============================================================================
# INCLUDE DIRECTORIES
# =============================================================================
#
# Add the Rubber Band root so that #include <rubberband/RubberBandLiveShifter.h>
# resolves to ThirdParty/rubberband-4.0.0/rubberband/RubberBandLiveShifter.h

target_include_directories(BADT
    PRIVATE
        ${RUBBERBAND_PATH}
)

# =============================================================================
# PREPROCESSOR DEFINITIONS
# =============================================================================
#
# These are constants visible to the C++ compiler (like #define in code).
# JUCE checks many of these at compile time to enable/disable features.

target_compile_definitions(BADT
    PUBLIC
        # Disable the built-in web browser component (we don't need it)
        JUCE_WEB_BROWSER=0

        # Disable libcurl networking (we don't need it)
        JUCE_USE_CURL=0

        # We are not replacing an older VST2 plugin, so set this to 0
        JUCE_VST3_CAN_REPLACE_VST2=0

        # Required on Windows/MSVC for M_PI and other math constants used inside
        # Rubber Band's bundled FFT and resampler code.
        _USE_MATH_DEFINES

        # Prevents Windows.h from defining min() and max() as macros.
        # Without this, code like std::numeric_limits<T>::min() fails to compile
        # because the preprocessor expands "min" before the "::" can be parsed.
        # JUCE also relies on NOMINMAX, so this is safe to set globally.
        NOMINMAX
)

# =============================================================================
# LINKING
# =============================================================================
#
# Link our plugin against the libraries it depends on.
# JUCE is split into "modules" — we only include what we need to keep compile times fast.

target_link_libraries(BADT
    PRIVATE
        BADT_BinaryData

        # juce_audio_utils: AudioProcessor, AudioProcessorEditor, APVTS, etc.
        # This pulls in juce_core, juce_events, juce_audio_basics, etc. automatically.
        juce::juce_audio_utils

        # juce_dsp: DSP helpers (filters, oscillators, convolution, etc.)
        juce::juce_dsp

    PUBLIC
        # These three are JUCE's recommended compiler flag bundles.
        # They set up things like optimisation levels, warning levels, etc.
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
