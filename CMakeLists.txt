cmake_minimum_required(VERSION 3.24)
project(opensims2 LANGUAGES CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(os2_formats
  src/formats/bhav.cpp
  src/formats/camera_config.cpp
  src/formats/config_text.cpp
  src/formats/dbpf.cpp
  src/formats/gmdc.cpp
  src/formats/image.cpp
  src/formats/lighting_config.cpp
  src/formats/model.cpp
  src/formats/qfs.cpp
  src/formats/rcol.cpp
  src/formats/resindex.cpp
  src/formats/scenegraph.cpp
  src/formats/simantics.cpp
  src/formats/strings.cpp
  src/formats/txmt.cpp
)
target_include_directories(os2_formats PUBLIC src)
target_compile_options(os2_formats PRIVATE -Wall -Wextra)
find_package(ZLIB REQUIRED)
target_link_libraries(os2_formats PRIVATE ZLIB::ZLIB)

add_executable(s2pkg cli/s2pkg/main.cpp cli/s2pkg/describe.cpp)
target_include_directories(s2pkg PRIVATE cli/s2pkg)
target_link_libraries(s2pkg PRIVATE os2_formats)

if(APPLE)
  # Shaders are compiled at runtime (the Command Line Tools have no offline
  # Metal compiler), so the source is embedded as a string.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/render/shaders.metal)
  file(READ src/render/shaders.metal OS2_SHADER_SRC)
  configure_file(src/render/shaders_src.hpp.in ${CMAKE_BINARY_DIR}/generated/shaders_src.hpp @ONLY)

  add_library(os2_render
    src/render/camera.cpp
    src/render/metal_impl.cpp
    src/render/renderer.cpp
    src/render/texture.cpp
  )
  target_include_directories(os2_render PUBLIC src third_party/metal-cpp
                             PRIVATE ${CMAKE_BINARY_DIR}/generated)
  target_compile_options(os2_render PRIVATE -Wall -Wextra)
  target_link_libraries(os2_render PUBLIC os2_formats
    "-framework Foundation" "-framework Metal" "-framework QuartzCore")

  add_executable(opensims2 MACOSX_BUNDLE src/app/main.mm)
  set_target_properties(opensims2 PROPERTIES
    OBJCXX_STANDARD 20
    MACOSX_BUNDLE_BUNDLE_NAME opensims2
    MACOSX_BUNDLE_GUI_IDENTIFIER org.opensims2.app)
  target_compile_options(opensims2 PRIVATE -fobjc-arc -Wall -Wextra)
  target_link_libraries(opensims2 PRIVATE os2_render os2_formats
    "-framework AppKit" "-framework MetalKit")
endif()

enable_testing()
add_executable(test_qfs tests/test_qfs.cpp)
target_link_libraries(test_qfs PRIVATE os2_formats)
add_test(NAME qfs COMMAND test_qfs)

add_executable(test_formats tests/test_formats.cpp)
target_link_libraries(test_formats PRIVATE os2_formats ZLIB::ZLIB)
add_test(NAME formats COMMAND test_formats)

if(APPLE)
  add_executable(test_camera tests/test_camera.cpp)
  target_link_libraries(test_camera PRIVATE os2_render)
  add_test(NAME camera COMMAND test_camera)

  add_executable(test_render tests/test_render.cpp)
  target_link_libraries(test_render PRIVATE os2_render)
  add_test(NAME render COMMAND test_render)
endif()
