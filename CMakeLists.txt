cmake_minimum_required(VERSION 3.22)
# Version: CI passes the tag (v1.2.3 -> 1.2.3); local builds use the default.
set(STAGERIG_VERSION "0.1.60" CACHE STRING "StageRig version")
project(StageRig VERSION ${STAGERIG_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)   # keeps JUCE's own install rules out of ours

juce_add_gui_app(StageRig
    PRODUCT_NAME "StageRig"
    COMPANY_NAME "dguedry"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/stagerig-512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/stagerig-64.png"
    VERSION ${PROJECT_VERSION})

# App icon, plus the user manual: embedding docs/MANUAL.md rather than copying it
# means the in-app help cannot drift from the file in the repository.
# CLAP is header-only: the SDK is just the C API the plugins implement.
# Distributions ship it (libclap-dev, clap-devel) and prefer their own copy;
# the submodule is the fallback. Version 1.2 or later is needed.
add_library(ClapHeaders INTERFACE)
option(STAGERIG_SYSTEM_CLAP "Use the system's CLAP headers when they are new enough" ON)
set(_clap_include "${CMAKE_CURRENT_SOURCE_DIR}/external/clap/include")
if(STAGERIG_SYSTEM_CLAP)
    find_path(SYSTEM_CLAP_INCLUDE_DIR clap/clap.h)
    if(SYSTEM_CLAP_INCLUDE_DIR AND EXISTS "${SYSTEM_CLAP_INCLUDE_DIR}/clap/version.h")
        file(STRINGS "${SYSTEM_CLAP_INCLUDE_DIR}/clap/version.h" _clap_major REGEX "#define CLAP_VERSION_MAJOR")
        file(STRINGS "${SYSTEM_CLAP_INCLUDE_DIR}/clap/version.h" _clap_minor REGEX "#define CLAP_VERSION_MINOR")
        string(REGEX MATCH "[0-9]+" _clap_major "${_clap_major}")
        string(REGEX MATCH "[0-9]+" _clap_minor "${_clap_minor}")
        if(_clap_major GREATER 1 OR (_clap_major EQUAL 1 AND _clap_minor GREATER_EQUAL 2))
            set(_clap_include "${SYSTEM_CLAP_INCLUDE_DIR}")
        endif()
    endif()
endif()
if(NOT EXISTS "${_clap_include}/clap/clap.h")
    message(FATAL_ERROR "CLAP headers not found: install libclap-dev / clap-devel, or check out the external/clap submodule")
endif()
message(STATUS "CLAP headers: ${_clap_include}")
target_include_directories(ClapHeaders INTERFACE "${_clap_include}")

juce_add_binary_data(StageRigAssets
    SOURCES Resources/stagerig-512.png Resources/stagerig-256.png Resources/stagerig-64.png
            docs/MANUAL.md)

target_sources(StageRig PRIVATE
    Source/Main.cpp
    Source/Model.cpp
    Source/PluginHost.cpp
    Source/PluginScanner.cpp
    Source/ClapPluginFormat.cpp
    Source/PluginIcons.cpp
    Source/MappingSuggestions.cpp
    Source/Ipc/Protocol.cpp
    Source/RemotePlugin.cpp
    Source/LowLatency.cpp
    Source/HelpWindow.cpp
    Source/SongsPanel.cpp
    Source/RemoteServer.cpp Source/Charts.cpp Source/Hotspot.cpp Source/QrCode.cpp Source/PluginView.cpp
    Source/Engine.cpp
    Source/MainComponent.cpp)

target_include_directories(StageRig PRIVATE Source)

target_compile_definitions(StageRig PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:StageRig,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:StageRig,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ALSA=1
    JUCE_JACK=1
    JUCE_JACK_CLIENT_NAME="StageRig"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_LADSPA=1
    JUCE_PLUGINHOST_VST=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(StageRig PRIVATE
    ClapHeaders
    StageRigAssets
    ${CMAKE_DL_LIBS}
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

#------------------------------------------------------------------------------
# stagerig-plugin-host: one plugin per process
juce_add_gui_app(StageRigPluginHost
    PRODUCT_NAME "stagerig-plugin-host"
    COMPANY_NAME "dguedry"
    VERSION ${PROJECT_VERSION})

target_sources(StageRigPluginHost PRIVATE
    Source/PluginHostProcess/PluginHostMain.cpp
    Source/ClapPluginFormat.cpp
    Source/Ipc/Protocol.cpp)

target_include_directories(StageRigPluginHost PRIVATE Source
    external/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK)

target_compile_definitions(StageRigPluginHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_LADSPA=1
    JUCE_PLUGINHOST_VST=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(StageRigPluginHost PRIVATE
    ClapHeaders
    StageRigAssets
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

add_dependencies(StageRig StageRigPluginHost)

# Put the helper next to the main executable so it is found at runtime.
#
# Done when the HELPER relinks, not only when StageRig does: a change to the
# host alone used to leave the stale copy in place, so the app kept launching
# an old helper while the new one sat unused in its own artefacts directory --
# and every test of the new code was silently running the old one.
# StageRig's own post-build copy is kept for the first build, when its
# directory may not exist yet when the helper is linked.
add_custom_command(TARGET StageRigPluginHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:StageRig>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:StageRigPluginHost> $<TARGET_FILE_DIR:StageRig>/stagerig-plugin-host
    COMMENT "Copying stagerig-plugin-host next to StageRig")
add_custom_command(TARGET StageRig POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:StageRigPluginHost> $<TARGET_FILE_DIR:StageRig>/stagerig-plugin-host
    COMMENT "Copying stagerig-plugin-host next to StageRig")

#------------------------------------------------------------------------------
# Tests
enable_testing()
juce_add_console_app(ModelTest PRODUCT_NAME "ModelTest")
target_sources(ModelTest PRIVATE Tests/ModelTest.cpp Source/ClapPluginFormat.cpp Source/Model.cpp Source/MappingSuggestions.cpp Source/QrCode.cpp)
target_include_directories(ModelTest PRIVATE Source)
target_compile_definitions(ModelTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_VST=0)
target_link_libraries(ModelTest PRIVATE
    ClapHeaders
    juce::juce_core juce::juce_data_structures juce::juce_events
    juce::juce_audio_processors
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME ModelTest COMMAND ModelTest)

juce_add_console_app(RemoteTest PRODUCT_NAME "RemoteTest")
target_sources(RemoteTest PRIVATE Tests/RemoteTest.cpp Source/RemoteServer.cpp Source/LowLatency.cpp Source/Charts.cpp Source/Hotspot.cpp Source/QrCode.cpp Source/PluginView.cpp Source/PluginView.cpp Source/QrCode.cpp Source/Model.cpp Source/ClapPluginFormat.cpp Source/PluginHost.cpp Source/PluginScanner.cpp Source/PluginIcons.cpp Source/MappingSuggestions.cpp Source/Ipc/Protocol.cpp Source/RemotePlugin.cpp Source/Engine.cpp)
target_include_directories(RemoteTest PRIVATE Source)
target_compile_definitions(RemoteTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ALSA=1 JUCE_JACK=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=1 JUCE_PLUGINHOST_LADSPA=1 JUCE_PLUGINHOST_VST=0)
target_link_libraries(RemoteTest PRIVATE
    ClapHeaders
    StageRigAssets
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_core juce::juce_data_structures juce::juce_events
    juce::juce_graphics juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME RemoteTest COMMAND RemoteTest)

juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
target_sources(EngineTest PRIVATE Tests/EngineTest.cpp Source/QrCode.cpp Source/LowLatency.cpp Source/Model.cpp Source/ClapPluginFormat.cpp Source/PluginHost.cpp Source/PluginScanner.cpp Source/PluginIcons.cpp Source/MappingSuggestions.cpp Source/Ipc/Protocol.cpp Source/RemotePlugin.cpp Source/Engine.cpp)
target_include_directories(EngineTest PRIVATE Source)
target_compile_definitions(EngineTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ALSA=1 JUCE_JACK=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=1 JUCE_PLUGINHOST_LADSPA=1 JUCE_PLUGINHOST_VST=0)
target_link_libraries(EngineTest PRIVATE
    ClapHeaders
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_core juce::juce_data_structures juce::juce_events
    juce::juce_graphics juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_dependencies(EngineTest StageRigPluginHost)
add_test(NAME EngineTest COMMAND EngineTest)
# Without an LV2 instrument to play (Calf Monosynth) the engine test has nothing
# to drive: it exits 77 and counts as skipped, so a distribution build without
# Calf among its build dependencies still passes.
set_tests_properties(EngineTest PROPERTIES ENVIRONMENT "STAGERIG_PLUGIN_HOST=$<TARGET_FILE:StageRigPluginHost>" SKIP_RETURN_CODE 77)

#------------------------------------------------------------------------------
# Install: `cmake --install build --prefix ~/.local` (or /usr/local) puts the two
# binaries side by side (StageRig finds its helper next to itself), plus the
# launcher entry and icon.
install(TARGETS StageRig StageRigPluginHost RUNTIME DESTINATION bin)
install(FILES Resources/io.github.dguedry.StageRig.desktop DESTINATION share/applications)
install(FILES Resources/io.github.dguedry.StageRig.metainfo.xml DESTINATION share/metainfo)
install(FILES docs/StageRig.1 docs/stagerig-plugin-host.1 DESTINATION share/man/man1)
install(FILES Resources/stagerig.svg DESTINATION share/icons/hicolor/scalable/apps)
foreach(size 16 24 32 48 64 128 256 512)
    install(FILES Resources/stagerig-${size}.png DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME stagerig.png)
endforeach()

#------------------------------------------------------------------------------
# Packages: `cpack -G DEB` / `cpack -G RPM` in the build dir (the GitHub workflow
# does both and attaches them to releases). Linked libraries are picked up
# automatically; the X11/GL libraries JUCE opens at runtime with dlopen are listed
# by hand.
set(CPACK_PACKAGE_NAME "stagerig")
set(CPACK_PACKAGE_VENDOR "dguedry")
set(CPACK_PACKAGE_CONTACT "Donnie Guedry <dguedry@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/dguedry/stagerig")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Live-performance plugin host for Linux")
set(CPACK_PACKAGE_DESCRIPTION "StageRig is a live-performance MIDI-to-plugin host: 128 programs per keyboard selected by MIDI Program Change, VST3/LV2/LADSPA instruments and effects with splits and velocity layers, MIDI learn, every plugin in its own process, low-latency PipeWire/JACK audio.")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_FILE_NAME "stagerig-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1, libgl1")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pipewire-jack | libjack0 | libjack-jackd2-0, pipewire-alsa | pulseaudio-utils")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "calf-plugins, yabridge")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_REQUIRES "libX11, libXext, libXrandr, libXinerama, libXcursor, mesa-libGL")
set(CPACK_RPM_PACKAGE_SUGGESTS "pipewire-jack-audio-connection-kit, calf")
# the icon and applications dirs belong to the filesystem/hicolor packages
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/share/applications" "/usr/share/icons" "/usr/share/icons/hicolor"
    "/usr/share/icons/hicolor/scalable" "/usr/share/icons/hicolor/scalable/apps")
foreach(size 16 24 32 48 64 128 256 512)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
         "/usr/share/icons/hicolor/${size}x${size}" "/usr/share/icons/hicolor/${size}x${size}/apps")
endforeach()

include(CPack)
