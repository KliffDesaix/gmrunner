# The Switch game list (Launcher.cpp) and Borealis, built into two static libraries that
# Makefile.switch links into gmrunner.nro. Run through tools/switch/build-launcher.sh.
cmake_minimum_required(VERSION 3.13)
set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)
project(gmrlauncher C CXX)
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(${BOREALIS_LIBRARY} borealis)
add_library(gmrlauncher STATIC Launcher.cpp)
target_link_libraries(gmrlauncher PUBLIC borealis)

# Desktop preview (cmake -DPLATFORM_DESKTOP=ON -DUSE_SDL2=ON): the same list in a window, to see
# changes without a console: build/gmrlauncher-preview <games folder>.
if(PLATFORM_DESKTOP)
  add_executable(gmrlauncher-preview preview.cpp)
  target_link_libraries(gmrlauncher-preview PRIVATE gmrlauncher)
  target_include_directories(gmrlauncher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)  # util/Png.h
  if(APPLE)
    target_link_libraries(gmrlauncher-preview PRIVATE "-framework CoreWLAN")  # Borealis's Wi-Fi indicator
  endif()
endif()
