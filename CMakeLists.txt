cmake_minimum_required(VERSION 3.13)  # 3.13: Debian 10, the PortMaster build image
project(gmrunner CXX C)

# The version, from VERSION (the Switch and Android builds read it too); 1.2.3 is build 10203.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GMR_VERSION LIMIT_COUNT 1)
string(REGEX MATCHALL "[0-9]+" GMR_VERSION_PARTS "${GMR_VERSION}.0.0")
list(GET GMR_VERSION_PARTS 0 _major)
list(GET GMR_VERSION_PARTS 1 _minor)
list(GET GMR_VERSION_PARTS 2 _patch)
math(EXPR GMR_BUILD "${_major} * 10000 + ${_minor} * 100 + ${_patch}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # (C and C++ only: the iOS build also compiles Swift, whose driver rejects these.)
  add_compile_options("$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-Wall;-Wextra;-Wno-unused-parameter;-Wno-unused-function>")
  # Source paths in the binaries (__FILE__ in assertions, debug info) relative to the checkout,
  # so builds don't carry the folder (and user name) they were made in.
  add_compile_options("$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=>")
endif()

# Platform-independent core: data.win loading, the bytecode VM and the GameMaker runtime.
add_library(gmcore STATIC
  src/data/DataWin.cpp
  src/vm/Bytecode.cpp
  src/vm/Value.cpp
  src/vm/VM.cpp
  src/render/GL.cpp
  src/render/Renderer.cpp
  src/runtime/Runner.cpp
  src/runtime/Collision.cpp
  src/runtime/BuiltinVars.cpp
  src/runtime/Graphics.cpp
  src/runtime/TrueType.cpp
  src/runtime/Audio.cpp
  src/runtime/Filters.cpp
  src/runtime/lib/Core.cpp
  src/runtime/lib/Ds.cpp
  src/runtime/lib/InstanceLib.cpp
  src/runtime/lib/DrawLib.cpp
  src/runtime/lib/AudioInputLib.cpp
  src/runtime/lib/FileLib.cpp
  src/runtime/lib/ParticleLib.cpp
  src/runtime/lib/Gms2Lib.cpp
  src/runtime/lib/JsonLib.cpp
  src/runtime/lib/BufferLib.cpp
  src/runtime/lib/LayerLib.cpp
  src/runtime/lib/MiscLib.cpp
  src/runtime/lib/ArrayLib.cpp
  src/runtime/lib/TimeLib.cpp
  src/runtime/lib/VertexLib.cpp
  src/runtime/lib/SpriteLib.cpp
  src/runtime/lib/AnimCurveLib.cpp
  src/runtime/lib/SequenceLib.cpp
  src/runtime/lib/DateLib.cpp
  src/runtime/lib/VideoLib.cpp
  src/runtime/lib/ExtEmuLib.cpp
  src/runtime/Video.cpp
  src/runtime/lib/SkeletonStub.cpp
  src/util/Bzip2.cpp
  src/util/BuiltinFonts.cpp
  src/util/Image.cpp
  src/util/Hash.cpp
)
target_include_directories(gmcore PUBLIC src third_party)
target_compile_definitions(gmcore PUBLIC GMR_VERSION="${GMR_VERSION}")
# Local modules kept outside the repository (private/<module>/*.cpp, e.g. skeleton sprites) are
# built in when present, unless GMR_PRIVATE=0 is set (tools/release.sh does, by default).
file(GLOB GMR_PRIVATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/private/*/*.cpp")
if(GMR_PRIVATE_SOURCES AND NOT "$ENV{GMR_PRIVATE}" STREQUAL "0")
  message(STATUS "Private modules: ${GMR_PRIVATE_SOURCES}")
  target_sources(gmcore PRIVATE ${GMR_PRIVATE_SOURCES})
  target_compile_definitions(gmcore PUBLIC GMR_SKELETON_MODULE)
endif()
# The built-in fonts (src/util/BuiltinFonts.cpp) are read from here at compile time.
target_compile_definitions(gmcore PRIVATE GMR_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/liberation-fonts")
# Texture pages are decoded on worker threads (glibc before 2.34 needs libpthread named).
find_package(Threads REQUIRED)
target_link_libraries(gmcore PUBLIC Threads::Threads)
# Video playback (video_open) uses FFmpeg when it is installed; otherwise videos fail to open.
# (Not for phone builds: pkg-config there would find the build machine's libraries.)
if(NOT ANDROID AND NOT IOS)
  find_package(PkgConfig QUIET)
endif()
if(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
endif()
if(FFMPEG_FOUND)
  target_compile_definitions(gmcore PRIVATE GMR_FFMPEG)
  if(GMR_TARGET STREQUAL "handheld")
    # The PortMaster runner links the static FFmpeg built in its Docker image
    # (tools/portmaster/Dockerfile), with the libraries FFmpeg itself needs.
    # libatomic goes in statically too: not every firmware has the shared one.
    set(ffmpeg_ldflags ${FFMPEG_STATIC_LDFLAGS})
    list(REMOVE_ITEM ffmpeg_ldflags -latomic)
    target_include_directories(gmcore PRIVATE ${FFMPEG_STATIC_INCLUDE_DIRS})
    target_link_libraries(gmcore PUBLIC ${ffmpeg_ldflags} -l:libatomic.a)
  else()
    target_link_libraries(gmcore PUBLIC PkgConfig::FFMPEG)
  endif()
  message(STATUS "Video playback: FFmpeg ${FFMPEG_libavcodec_VERSION}")
else()
  message(STATUS "Video playback: disabled (FFmpeg not found)")
endif()
# bzip2 texture pages with randomised blocks are decoded by libbz2 when it is installed.
if(NOT ANDROID AND NOT IOS)
  find_package(BZip2 QUIET)
endif()
if(BZIP2_FOUND)
  target_compile_definitions(gmcore PRIVATE GMR_LIBBZ2)
  target_link_libraries(gmcore PUBLIC BZip2::BZip2)
endif()
# Third-party single-file decoders are compiled as part of the core with warnings off.
set_source_files_properties(src/runtime/Graphics.cpp src/runtime/Audio.cpp PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-but-set-variable;-Wno-unused-variable;-Wno-missing-field-initializers")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_property(SOURCE src/runtime/Audio.cpp APPEND PROPERTY COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()

# The SDL2 host's sources; one Target*.cpp in them is compiled in (see Target.h).
set(HOST_SOURCES
  src/platform/sdl/Main.cpp
  src/platform/sdl/Options.cpp
  src/platform/sdl/Files.cpp
  src/platform/sdl/SdlPlatform.cpp
  src/platform/sdl/Buttons.cpp
  src/platform/sdl/GameMenu.cpp
  src/platform/sdl/HostInput.cpp
  src/platform/sdl/DevTools.cpp
  src/platform/sdl/Ui.cpp
  src/platform/sdl/Picker.cpp
  src/platform/sdl/TouchPad.cpp
)

if(ANDROID OR IOS)
  # Phones and tablets: SDL2 built from its source release in third_party/SDL2 (docs/MOBILE.md).
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2/CMakeLists.txt)
    message(FATAL_ERROR "Unpack the SDL2 source release into third_party/SDL2 (see docs/MOBILE.md)")
  endif()
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  if(ANDROID)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)   # SDLActivity loads libSDL2.so and libmain.so
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)  # one static app on iOS
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(third_party/SDL2 SDL2 EXCLUDE_FROM_ALL)
  # Video through the system's decoders (src/platform/video/), not FFmpeg.
  target_compile_definitions(gmcore PRIVATE GMR_PLATFORM_VIDEO)
  if(ANDROID)
    add_library(main SHARED ${HOST_SOURCES} src/platform/sdl/TargetMobile.cpp src/platform/video/VideoAndroid.cpp)
    target_compile_definitions(main PRIVATE GMR_MOBILE)
    target_link_libraries(main PRIVATE gmcore SDL2::SDL2 android log mediandk)
  else()
    # The SwiftUI launcher (ios/Launcher.swift) and SDL's UIKit app around the runner.
    enable_language(Swift)
    # The licenses of the third-party software in the app (shown by the launcher).
    set(IOS_LICENSES licenses/GPL-3.0.txt licenses/SDL2.txt licenses/stb.txt licenses/liberation-fonts.txt licenses/bzip2.txt
        licenses/gamemaker-html5.txt licenses/Apache-2.0.txt)
    set_source_files_properties(${IOS_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
    add_executable(gmrunner MACOSX_BUNDLE ${HOST_SOURCES} src/platform/sdl/TargetMobile.cpp src/platform/video/VideoApple.cpp ios/Launcher.swift ios/GameMenu.swift ios/VideoReader.swift
                   ios/LaunchScreen.storyboard ${IOS_LICENSES})
    target_compile_definitions(gmrunner PRIVATE GMR_MOBILE)
    target_link_libraries(gmrunner PRIVATE gmcore SDL2::SDL2main SDL2::SDL2-static "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
    set_target_properties(gmrunner PROPERTIES
      OUTPUT_NAME "gmRunner"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${GMR_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${GMR_BUILD}"
      RESOURCE ios/LaunchScreen.storyboard
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.gmrunner.app"
      XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0"
      XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
      XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
      XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
  endif()
  return()
endif()

add_executable(gmdump tools/gmdump.cpp)
target_link_libraries(gmdump PRIVATE gmcore)

find_package(SDL2 CONFIG QUIET)
# SDL before 2.0.12 ships a config file without the SDL2::SDL2 target (Debian 10): use pkg-config.
if(NOT SDL2_FOUND OR NOT TARGET SDL2::SDL2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(SDL2_TARGET PkgConfig::SDL2)
else()
  set(SDL2_TARGET SDL2::SDL2)
endif()

# The SDL2 host. GMR_TARGET picks what it is built for: desktop (a window; the default) or
# handheld (Linux handhelds through PortMaster: fullscreen OpenGL ES, Start + Select quits; see
# tools/portmaster/). The Switch build uses Makefile.switch; iOS and Android are above.
set(GMR_TARGET desktop CACHE STRING "desktop or handheld")
add_executable(gmrunner ${HOST_SOURCES} src/platform/sdl/TargetDesktop.cpp src/platform/sdl/TargetHandheld.cpp)
target_link_libraries(gmrunner PRIVATE gmcore ${SDL2_TARGET} ${CMAKE_DL_LIBS})  # dlsym: newer SDL functions
if(GMR_TARGET STREQUAL "handheld")
  target_compile_definitions(gmrunner PRIVATE GMR_HANDHELD)
  # One binary for many firmwares: no dependency on the device's C++ runtime.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(gmrunner PRIVATE -static-libstdc++ -static-libgcc)
  endif()
elseif(NOT GMR_TARGET STREQUAL "desktop")
  message(FATAL_ERROR "GMR_TARGET must be desktop or handheld")
endif()
