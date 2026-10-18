# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(dsperate main.cpp config.cpp display.cpp audio.cpp input.cpp pad_faces.cpp menu.cpp settings.cpp lid.cpp mic_alsa.cpp loader_cart.cpp dmaheap.cpp
  display_drm.cpp display_disp.cpp display_fbdev.cpp)
# Required on older glibc toolchains where pthread symbols are not in libc.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(dsperate PRIVATE dsperate_core dsperate_frontend_video ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS} Threads::Threads)
# video.hires3d: the hi-res 3D (gl_hires.cpp, a library of its own so the
# tests can drive it) and the GLES present (gl_present.cpp). Without GLES
# headers both compile as stubs that decline.
find_path(DSPERATE_GLES_INCLUDE GLES3/gl31.h)
find_library(DSPERATE_GLES_LIB GLESv2)
add_library(dsperate_gles STATIC gl_hires.cpp)
target_link_libraries(dsperate_gles PUBLIC dsperate_core)
target_include_directories(dsperate_gles PUBLIC ${SDL2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(DSPERATE_GLES_INCLUDE AND DSPERATE_GLES_LIB)
  set(DSPERATE_GLES ON PARENT_SCOPE)
  target_compile_definitions(dsperate_gles PUBLIC DSPERATE_GLES=1)
  target_include_directories(dsperate_gles PUBLIC ${DSPERATE_GLES_INCLUDE})
  target_link_libraries(dsperate_gles PUBLIC ${DSPERATE_GLES_LIB} ${SDL2_LIBRARIES})
else()
  target_compile_definitions(dsperate_gles PUBLIC DSPERATE_GLES=0)
endif()
target_sources(dsperate PRIVATE gl_present.cpp)
target_link_libraries(dsperate PRIVATE dsperate_gles)
# GCC < 9 requires a separate library for C++17 std::filesystem.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(dsperate PRIVATE stdc++fs)
endif()
# Restamped on every build; its own translation unit so a commit recompiles nothing else.
set(_ds_version_cpp ${CMAKE_CURRENT_BINARY_DIR}/version.cpp)
add_custom_target(dsperate_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${_ds_version_cpp} -DDSPERATE_VERSION=${DSPERATE_VERSION} -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS ${_ds_version_cpp} VERBATIM)
target_sources(dsperate PRIVATE ${_ds_version_cpp})
add_dependencies(dsperate dsperate_version)
if(DSPERATE_CHEEVOS)
  target_link_libraries(dsperate PRIVATE dsperate_cheevos)
  target_compile_definitions(dsperate PRIVATE DSPERATE_CHEEVOS=1)
endif()
target_include_directories(dsperate PRIVATE ${SDL2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
if(DSPERATE_WAYLAND)
  target_sources(dsperate PRIVATE display_wl.cpp wl_dyn.cpp wl/wayland-protocol.c wl/linux-dmabuf-v1.c wl/xdg-shell.c)
  target_compile_definitions(dsperate PRIVATE DSPERATE_WAYLAND=1)
  # Vendored protocol code marshals through dlopen'd pointers; force-include the macro layer, no linked wayland.
  set_source_files_properties(wl/wayland-protocol.c wl/linux-dmabuf-v1.c wl/xdg-shell.c PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/wl_dyn.h;-I${CMAKE_CURRENT_SOURCE_DIR}/wl")
else()
  target_sources(dsperate PRIVATE display_wl_stub.cpp)
endif()
if(DSPERATE_NET)
  target_link_libraries(dsperate PRIVATE dsperate_net)
  target_compile_definitions(dsperate PRIVATE DSPERATE_NET=1)
endif()
target_sources(dsperate PRIVATE gpu_present.cpp video/present_vk_import.cpp video/present_rga.cpp)
if(DSPERATE_VULKAN AND DSPERATE_VULKAN_INCLUDE)
  target_include_directories(dsperate PRIVATE ${DSPERATE_VULKAN_INCLUDE})
  set_property(SOURCE gpu_present.cpp APPEND PROPERTY COMPILE_DEFINITIONS
               DSPERATE_PRESENT_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
  set_property(SOURCE gpu_present.cpp APPEND PROPERTY OBJECT_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.spv")
endif()
