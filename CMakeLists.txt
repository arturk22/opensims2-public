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

# Speex 1.2.1 (BSD, third_party/speex): decoder for the SPX1 voice resources.
enable_language(C)
file(GLOB OS2_SPEEX_SRC third_party/speex/libspeex/*.c)
add_library(os2_speex STATIC ${OS2_SPEEX_SRC})
target_include_directories(os2_speex PUBLIC third_party/speex/include)
target_compile_definitions(os2_speex PRIVATE FLOATING_POINT VAR_ARRAYS EXPORT=)
target_compile_options(os2_speex PRIVATE -w)

add_library(os2_formats
  src/formats/anim.cpp
  src/formats/anim_player.cpp
  src/formats/audio.cpp
  src/formats/audio_spx.cpp
  src/formats/bhav.cpp
  src/formats/camera_config.cpp
  src/formats/config_text.cpp
  src/formats/dbpf.cpp
  src/formats/font.cpp
  src/formats/gmdc.cpp
  src/formats/image.cpp
  src/formats/jpeg.cpp
  src/formats/lighting_config.cpp
  src/formats/locale.cpp
  src/formats/lot.cpp
  src/formats/lot_objects.cpp
  src/formats/lot_model.cpp
  src/formats/lot_roof.cpp
  src/formats/model.cpp
  src/formats/neighborhood.cpp
  src/formats/property_set.cpp
  src/formats/qfs.cpp
  src/formats/rcol.cpp
  src/formats/resindex.cpp
  src/formats/scenegraph.cpp
  src/formats/sim.cpp
  src/formats/sim_actor.cpp
  src/formats/sim_character.cpp
  src/formats/simantics.cpp
  src/formats/skeleton.cpp
  src/formats/skinning.cpp
  src/formats/strings.cpp
  src/formats/txmt.cpp
  src/formats/ui_image.cpp
  src/formats/ui_resources.cpp
  src/formats/ui_screen.cpp
  src/formats/ui_script.cpp
)
target_include_directories(os2_formats PUBLIC src)
target_compile_options(os2_formats PRIVATE -Wall -Wextra)
find_package(ZLIB REQUIRED)
target_link_libraries(os2_formats PRIVATE ZLIB::ZLIB os2_speex)

add_library(os2_vm
  src/vm/hood_bridge.cpp
  src/vm/animation.cpp
  src/vm/appearance.cpp
  src/vm/dialog.cpp
  src/vm/autonomy.cpp
  src/vm/lot.cpp
  src/vm/lot_resume.cpp
  src/vm/measure.cpp
  src/vm/motives.cpp
  src/vm/primitives.cpp
  src/vm/queue.cpp
  src/vm/reach.cpp
  src/vm/routing.cpp
  src/vm/sim_motion.cpp
  src/vm/sound.cpp
  src/vm/timer.cpp
  src/vm/turn.cpp
  src/vm/view.cpp
  src/vm/vm.cpp
  src/vm/world.cpp
)
target_include_directories(os2_vm PUBLIC src)
target_compile_options(os2_vm PRIVATE -Wall -Wextra)
target_link_libraries(os2_vm PUBLIC os2_formats)

# Sound: decoding to PCM (MP3 through AudioToolbox), name lookup and playback.
add_library(os2_audio src/audio/decode.cpp src/audio/sound_bank.cpp src/audio/mixer.cpp src/audio/sound_system.cpp)
target_include_directories(os2_audio PUBLIC src)
target_compile_options(os2_audio PRIVATE -Wall -Wextra)
target_link_libraries(os2_audio PUBLIC os2_formats)
if(APPLE)
  target_sources(os2_audio PRIVATE src/audio/player.cpp src/audio/play_sound.cpp src/audio/mixer_output.cpp)
  target_link_libraries(os2_audio PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
endif()

add_executable(s2pkg cli/s2pkg/main.cpp cli/s2pkg/describe.cpp cli/s2pkg/anim_cmds.cpp cli/s2pkg/material_cmds.cpp cli/s2pkg/audio_cmds.cpp cli/s2pkg/ui_cmds.cpp cli/s2pkg/hood_cmds.cpp cli/s2pkg/prim_cmds.cpp cli/s2pkg/pie_cmds.cpp cli/s2pkg/state_cmds.cpp cli/s2pkg/autonomy_cmds.cpp)
target_include_directories(s2pkg PRIVATE cli/s2pkg)
target_link_libraries(s2pkg PRIVATE os2_vm os2_audio)

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
    src/render/ui_layer.cpp
  )
  target_include_directories(os2_render PUBLIC src third_party/metal-cpp
                             PRIVATE ${CMAKE_BINARY_DIR}/generated)
  target_compile_options(os2_render PRIVATE -Wall -Wextra)
  target_link_libraries(os2_render PUBLIC os2_formats
    "-framework Foundation" "-framework Metal" "-framework QuartzCore")

  add_executable(opensims2 MACOSX_BUNDLE src/app/main.mm src/app/lot_vm.cpp src/app/vm_objects.cpp src/app/look_at.cpp src/app/pie_menu.cpp src/app/lot_audio.cpp src/app/camera_input.cpp)
  set_target_properties(opensims2 PROPERTIES
    OBJCXX_STANDARD 20
    MACOSX_BUNDLE_BUNDLE_NAME opensims2
    MACOSX_BUNDLE_GUI_IDENTIFIER org.opensims2.app)
  target_compile_options(opensims2 PRIVATE -fobjc-arc -Wall -Wextra)
  target_link_libraries(opensims2 PRIVATE os2_render os2_audio os2_vm
    "-framework AppKit" "-framework MetalKit")
endif()

enable_testing()
add_executable(test_qfs tests/test_qfs.cpp)
target_link_libraries(test_qfs PRIVATE os2_formats)
add_test(NAME qfs COMMAND test_qfs)

add_executable(test_dbpf tests/test_dbpf.cpp)
target_link_libraries(test_dbpf PRIVATE os2_formats)
add_test(NAME dbpf COMMAND test_dbpf)

add_executable(test_formats tests/test_formats.cpp)
target_link_libraries(test_formats PRIVATE os2_formats ZLIB::ZLIB)
add_test(NAME formats COMMAND test_formats)

add_executable(test_vm tests/test_vm.cpp)
target_link_libraries(test_vm PRIVATE os2_vm)
add_test(NAME vm COMMAND test_vm)

add_executable(test_hood_vm tests/test_hood_vm.cpp)
target_link_libraries(test_hood_vm PRIVATE os2_vm)
add_test(NAME hood_vm COMMAND test_hood_vm)

add_executable(test_dialog tests/test_dialog.cpp)
target_link_libraries(test_dialog PRIVATE os2_vm)
add_test(NAME dialog COMMAND test_dialog)

add_executable(test_queue tests/test_queue.cpp)
target_link_libraries(test_queue PRIVATE os2_vm)
add_test(NAME queue COMMAND test_queue)

add_executable(test_autonomy tests/test_autonomy.cpp)
target_link_libraries(test_autonomy PRIVATE os2_vm)
add_test(NAME autonomy COMMAND test_autonomy)

add_executable(test_reach tests/test_reach.cpp)
target_link_libraries(test_reach PRIVATE os2_vm)
add_test(NAME reach COMMAND test_reach)

add_executable(test_routing tests/test_routing.cpp)
target_link_libraries(test_routing PRIVATE os2_vm)
add_test(NAME routing COMMAND test_routing)

add_executable(test_animation_vm tests/test_animation_vm.cpp)
target_link_libraries(test_animation_vm PRIVATE os2_vm)
add_test(NAME animation_vm COMMAND test_animation_vm)

if(APPLE)
  add_executable(test_camera tests/test_camera.cpp)
  target_link_libraries(test_camera PRIVATE os2_render)
  add_test(NAME camera COMMAND test_camera)

  add_executable(test_render tests/test_render.cpp)
  target_link_libraries(test_render PRIVATE os2_render)
  add_test(NAME render COMMAND test_render)
endif()

add_executable(test_anim tests/test_anim.cpp)
target_link_libraries(test_anim PRIVATE os2_formats)
add_test(NAME anim COMMAND test_anim)

add_executable(test_audio tests/test_audio.cpp)
target_link_libraries(test_audio PRIVATE os2_audio)
target_include_directories(test_audio PRIVATE third_party/speex/include) # the test encodes with libspeex
add_test(NAME audio COMMAND test_audio)
add_executable(test_ui tests/test_ui.cpp)
target_link_libraries(test_ui PRIVATE os2_formats)
add_test(NAME ui COMMAND test_ui)
add_executable(test_locale tests/test_locale.cpp)
target_link_libraries(test_locale PRIVATE os2_formats)
add_test(NAME locale COMMAND test_locale)
add_executable(test_neighborhood tests/test_neighborhood.cpp)
target_link_libraries(test_neighborhood PRIVATE os2_formats)
add_test(NAME neighborhood COMMAND test_neighborhood)
add_executable(test_sound tests/test_sound.cpp)
target_link_libraries(test_sound PRIVATE os2_audio os2_vm)
add_test(NAME sound COMMAND test_sound)
add_executable(test_camera_input tests/test_camera_input.cpp src/app/camera_input.cpp)
target_include_directories(test_camera_input PRIVATE src)
add_test(NAME camera_input COMMAND test_camera_input)
