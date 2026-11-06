cmake_minimum_required(VERSION 3.16)
project(nava-studio VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path/../libs/")
else()
    set(CMAKE_INSTALL_RPATH "$ORIGIN/lib:$ORIGIN/../lib:$ORIGIN/../build/ffmpeg-dist/lib")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Check for GTK4
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

# FFmpeg libraries and includes
set(FFMPEG_LIBS avcodec avformat avutil swscale swresample)

if(APPLE)
    include_directories(/opt/homebrew/include /usr/local/include)
    link_directories(/opt/homebrew/lib /usr/local/lib)
endif()

pkg_check_modules(AVCODEC libavcodec)
if(AVCODEC_FOUND)
    include_directories(${AVCODEC_INCLUDE_DIRS})
    link_directories(${AVCODEC_LIBRARY_DIRS})
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg-dist/lib")
    link_directories("${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg-dist/lib")
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg-dist/include")
endif()

# Platform-specific audio backends & dependencies
set(EXTRA_LIBS "")
set(EXTRA_COMPILE_DEFS "")

if(APPLE)
    message(STATUS "Configuring for macOS (CoreAudio backend)")
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    list(APPEND EXTRA_LIBS ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB})
    list(APPEND EXTRA_COMPILE_DEFS -DNAVA_AUDIO_COREAUDIO=1)
elseif(WIN32)
    message(STATUS "Configuring for Windows (WASAPI backend)")
    list(APPEND EXTRA_LIBS ole32 mmdevapi avrt uuid winmm dwmapi)
    list(APPEND EXTRA_COMPILE_DEFS -DNAVA_AUDIO_WASAPI=1)
else()
    message(STATUS "Configuring for Linux/Unix (PulseAudio backend)")
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)
    list(APPEND EXTRA_LIBS PkgConfig::PULSE m dl)
    list(APPEND EXTRA_COMPILE_DEFS -DNAVA_AUDIO_PULSE=1)
endif()

# GLib resources compilation
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(RESOURCE_XML "${CMAKE_CURRENT_SOURCE_DIR}/resources/nava.gresource.xml")
set(RESOURCE_C "${CMAKE_CURRENT_BINARY_DIR}/resources.c")

add_custom_command(
    OUTPUT ${RESOURCE_C}
    COMMAND ${GLIB_COMPILE_RESOURCES}
        --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/resources
        --generate-source
        --target=${RESOURCE_C}
        ${RESOURCE_XML}
    DEPENDS ${RESOURCE_XML}
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/style.css
    VERBATIM
)

# Core sources
set(SOURCES
    src/main.c
    src/app.c
    src/engine.c
    src/dsp.c
    src/vis.c
    src/audio_out.c
    ${RESOURCE_C}
)

add_executable(nava-studio ${SOURCES})

target_include_directories(nava-studio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_compile_definitions(nava-studio PRIVATE
    ${EXTRA_COMPILE_DEFS}
)

target_compile_options(nava-studio PRIVATE
    -Wall -Wextra
    -Wno-misleading-indentation
    -Wno-deprecated-declarations
    -Wno-unused-parameter
)

target_link_libraries(nava-studio PRIVATE
    PkgConfig::GTK4
    ${FFMPEG_LIBS}
    Threads::Threads
    ${EXTRA_LIBS}
)

if(WIN32)
    set_target_properties(nava-studio PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(nava-studio PRIVATE -mwindows)
endif()

# Headless engine test target
add_executable(nava-engine-test
    src/engine_test.c
    src/engine.c
    src/dsp.c
    src/audio_out.c
)

target_include_directories(nava-engine-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(nava-engine-test PRIVATE
    ${EXTRA_COMPILE_DEFS}
)

target_link_libraries(nava-engine-test PRIVATE
    ${FFMPEG_LIBS}
    Threads::Threads
    ${EXTRA_LIBS}
)

# Installation targets
include(GNUInstallDirs)
install(TARGETS nava-studio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/nava-studio.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icons/nava-studio.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
