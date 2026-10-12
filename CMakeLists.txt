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
  src/formats/scene_anim.cpp
  src/formats/audio.cpp
  src/formats/audio_spx.cpp
  src/formats/bhav.cpp
  src/formats/camera_config.cpp
  src/formats/cas_catalog.cpp
  src/formats/cas_face.cpp
  src/formats/cas_thumbnails.cpp
  src/formats/lxnr.cpp
  src/formats/cas_personality.cpp
  src/formats/character_write.cpp
  src/formats/build_catalog.cpp
  src/formats/design_catalog.cpp
  src/formats/catalog.cpp
  src/formats/cinematic.cpp
  src/formats/config_text.cpp
  src/formats/dbpf.cpp
  src/formats/effects.cpp
  src/formats/font.cpp
  src/formats/gmdc.cpp
  src/formats/image.cpp
  src/formats/jpeg.cpp
  src/formats/lighting_config.cpp
  src/formats/locale.cpp
  src/formats/hood_lot_terrain.cpp
  src/formats/hood_moves.cpp
  src/formats/lot_arrivals.cpp
  src/formats/lot_evict.cpp
  src/formats/hood_lots.cpp
  src/formats/hood_scene.cpp
  src/formats/hood_terrain.cpp
  src/formats/lot.cpp
  src/formats/lot_objects.cpp
  src/formats/lot_objects_write.cpp
  src/formats/lot_edit.cpp
  src/formats/lot_write.cpp
  src/formats/lot_model.cpp
  src/formats/lot_lightmap.cpp
  src/formats/lot_roof.cpp
  src/formats/model.cpp
  src/formats/movie.cpp
  src/formats/neighborhood.cpp
  src/formats/wants.cpp
  src/formats/neighborhood_write.cpp
  src/formats/property_set.cpp
  src/formats/qfs.cpp
  src/formats/rcol.cpp
  src/formats/reia.cpp
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
  src/vm/hood_save.cpp
  src/vm/wants.cpp
  src/vm/kill.cpp
  src/vm/rooms.cpp
  src/vm/build_edit.cpp
  src/vm/characters.cpp
  src/vm/baby_character.cpp
  src/vm/genetics.cpp
  src/vm/family_write.cpp
  src/vm/animation.cpp
  src/vm/appearance.cpp
  src/vm/dialog.cpp
  src/vm/engine_prims.cpp
  src/vm/tutorial.cpp
  src/vm/autonomy.cpp
  src/vm/autonomy_trees.cpp
  src/vm/links.cpp
  src/vm/lot.cpp
  src/vm/lot_neighborhood.cpp
  src/vm/lot_records.cpp
  src/vm/lot_resume.cpp
  src/vm/thread_state.cpp
  src/vm/lot_save.cpp
  src/vm/measure.cpp
  src/vm/message.cpp
  src/vm/motives.cpp
  src/vm/ownership.cpp
  src/vm/path_planner.cpp
  src/vm/person_prims.cpp
  src/vm/placement.cpp
  src/vm/primitives.cpp
  src/vm/queue.cpp
  src/vm/reach.cpp
  src/vm/routing.cpp
  src/vm/save_types.cpp
  src/vm/sim_motion.cpp
  src/vm/locomotion.cpp
  src/vm/slot_bones.cpp
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

# Movies (.movie): playback by the sound's clock.
add_library(os2_media src/media/movie_player.cpp)
target_include_directories(os2_media PUBLIC src)
target_compile_options(os2_media PRIVATE -Wall -Wextra)
target_link_libraries(os2_media PUBLIC os2_audio)

# The game's particle effects, simulated without Metal (render/particle_system).
add_library(os2_fx src/render/particle_system.cpp)
target_include_directories(os2_fx PUBLIC src)
target_compile_options(os2_fx PRIVATE -Wall -Wextra)
target_link_libraries(os2_fx PUBLIC os2_formats)

add_executable(s2pkg cli/s2pkg/main.cpp cli/s2pkg/describe.cpp cli/s2pkg/anim_cmds.cpp cli/s2pkg/material_cmds.cpp cli/s2pkg/audio_cmds.cpp cli/s2pkg/movie_cmds.cpp cli/s2pkg/ui_cmds.cpp cli/s2pkg/hood_cmds.cpp cli/s2pkg/hood_play_cmds.cpp cli/s2pkg/prim_cmds.cpp cli/s2pkg/pie_cmds.cpp cli/s2pkg/state_cmds.cpp cli/s2pkg/save_cmds.cpp cli/s2pkg/autonomy_cmds.cpp cli/s2pkg/wants_cmds.cpp cli/s2pkg/route_cmds.cpp cli/s2pkg/buy_cmds.cpp cli/s2pkg/build_cmds.cpp cli/s2pkg/effects_cmds.cpp cli/s2pkg/simlook_cmds.cpp cli/s2pkg/cas_cmds.cpp cli/s2pkg/tutorial_cmds.cpp src/app/game_save.cpp)
target_include_directories(s2pkg PRIVATE cli/s2pkg)
target_link_libraries(s2pkg PRIVATE os2_vm os2_audio os2_fx)

if(APPLE)
  # Shaders are compiled at runtime (the Command Line Tools have no offline
  # Metal compiler), so the source is embedded as a string.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/render/shaders.metal)
  file(READ src/render/shaders.metal OS2_SHADER_SRC)
  configure_file(src/render/shaders_src.hpp.in ${CMAKE_BINARY_DIR}/generated/shaders_src.hpp @ONLY)

  add_library(os2_render
    src/render/camera.cpp
    src/render/cas_camera.cpp
    src/render/metal_impl.cpp
    src/render/particle_layer.cpp
    src/render/renderer.cpp
    src/render/texture.cpp
    src/render/ui_layer.cpp
    src/render/movie_layer.cpp
    src/render/screen_fx_layer.cpp
  )
  target_include_directories(os2_render PUBLIC src third_party/metal-cpp
                             PRIVATE ${CMAKE_BINARY_DIR}/generated)
  target_compile_options(os2_render PRIVATE -Wall -Wextra)
  target_link_libraries(os2_render PUBLIC os2_formats os2_fx
    "-framework Foundation" "-framework Metal" "-framework QuartzCore")

  add_executable(opensims2 MACOSX_BUNDLE src/app/main.mm src/app/lot_vm.cpp src/app/jump_detector.cpp src/app/vm_objects.cpp src/app/look_at.cpp src/app/lot_lighting.cpp src/app/door_portals.cpp src/app/pie_menu.cpp src/app/dialog_window.cpp src/app/live_ui.cpp src/app/live_panels.cpp src/app/wants_panel.cpp src/app/ui_panel_util.cpp src/app/lot_audio.cpp src/app/camera_input.cpp src/app/camera_follow.cpp src/app/wall_view.cpp src/app/anim_snaps.cpp src/app/data_location.mm src/app/hood_view.cpp src/app/hood_ui.cpp src/app/hood_bins.cpp src/app/hood_mode.cpp src/app/buy_mode.cpp src/app/design_tool.cpp src/app/build_mode.cpp src/app/movie_host.cpp src/app/cinematic_camera.cpp src/app/cinematic_player.cpp src/app/lot_effects.cpp src/app/tv_broadcast.cpp)
  # The game flow: main menu, user folder, in-game save, options (app/game_flow.hpp).
  target_sources(opensims2 PRIVATE src/app/game_flow.cpp src/app/user_data.cpp src/app/game_save.cpp src/app/main_menu.cpp src/app/tutorial.cpp src/app/options_panel.cpp src/app/cas_mode.cpp src/app/cas_studio.cpp)
  # Scenes loaded off the main thread behind the loading screens (docs/loading-mac.md).
  target_sources(opensims2 PRIVATE src/app/scene_loader.cpp src/app/loading_screen.cpp src/app/head_shot.cpp)
  set_target_properties(opensims2 PROPERTIES
    OBJCXX_STANDARD 20
    MACOSX_BUNDLE_BUNDLE_NAME opensims2
    MACOSX_BUNDLE_GUI_IDENTIFIER org.opensims2.opensims2
    MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1.0
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in)
  # Minimum macOS for the Info.plist (the SDK's deployment target when set).
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET_OR_DEFAULT ${CMAKE_OSX_DEPLOYMENT_TARGET})
  else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET_OR_DEFAULT 13.0)
  endif()
  # `cmake --build build --target package-app`: a copy of the bundle without
  # game data, ad-hoc signed (runs on Apple Silicon for the local user; not
  # notarized), plus opensims2.zip, in build/package/.
  add_custom_target(package-app
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/package
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/package
    COMMAND ditto $<TARGET_BUNDLE_DIR:opensims2> ${CMAKE_BINARY_DIR}/package/opensims2.app
    COMMAND codesign --force --deep -s - ${CMAKE_BINARY_DIR}/package/opensims2.app
    COMMAND ditto -c -k --keepParent ${CMAKE_BINARY_DIR}/package/opensims2.app ${CMAKE_BINARY_DIR}/package/opensims2.zip
    DEPENDS opensims2
    COMMENT "Packaging build/package/opensims2.app (ad-hoc signed)")
  target_compile_options(opensims2 PRIVATE -fobjc-arc -Wall -Wextra)
  target_link_libraries(opensims2 PRIVATE os2_render os2_audio os2_media os2_vm
    "-framework AppKit" "-framework MetalKit")
endif()

enable_testing()
add_executable(test_qfs tests/test_qfs.cpp)
target_link_libraries(test_qfs PRIVATE os2_formats)
add_test(NAME qfs COMMAND test_qfs)

add_executable(test_dbpf tests/test_dbpf.cpp)
target_link_libraries(test_dbpf PRIVATE os2_formats)
add_test(NAME dbpf COMMAND test_dbpf)

add_executable(test_formats tests/test_formats.cpp src/app/wall_view.cpp)
target_link_libraries(test_formats PRIVATE os2_formats ZLIB::ZLIB)
add_test(NAME formats COMMAND test_formats)

add_executable(test_scene_loader tests/test_scene_loader.cpp src/app/scene_loader.cpp)
target_include_directories(test_scene_loader PRIVATE src)
add_test(NAME scene_loader COMMAND test_scene_loader)

add_executable(test_vm tests/test_vm.cpp)
target_link_libraries(test_vm PRIVATE os2_vm)
add_test(NAME vm COMMAND test_vm)

add_executable(test_hood_vm tests/test_hood_vm.cpp)
target_link_libraries(test_hood_vm PRIVATE os2_vm)
add_test(NAME hood_vm COMMAND test_hood_vm)

add_executable(test_genetics tests/test_genetics.cpp)
target_link_libraries(test_genetics PRIVATE os2_vm)
add_test(NAME genetics COMMAND test_genetics)

add_executable(test_dialog tests/test_dialog.cpp)
target_link_libraries(test_dialog PRIVATE os2_vm)
add_test(NAME dialog COMMAND test_dialog)

add_executable(test_tutorial tests/test_tutorial.cpp)
target_link_libraries(test_tutorial PRIVATE os2_vm)
add_test(NAME tutorial COMMAND test_tutorial)
# With the game's data (OS2_DATA): s2pkg tutorial --auto plays "O Básico -
# Parte Um" (Tutorial_Lot14) to its step 32 and past it (live QA 14);
# without the data it is skipped.
add_test(NAME tutorial_auto COMMAND sh -c "if [ -z \"$OS2_DATA\" ]; then echo 'furthest step shown 33 (skipped: no OS2_DATA)'; else $<TARGET_FILE:s2pkg> tutorial \"$OS2_DATA/TSData/Res/UserData/Neighborhoods/Tutorial/Lots/Tutorial_Lot14.package\" --auto --ticks 9000; fi")
set_tests_properties(tutorial_auto PROPERTIES PASS_REGULAR_EXPRESSION "furthest step shown 3[3-9]")

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

add_executable(test_build tests/test_build.cpp)
target_link_libraries(test_build PRIVATE os2_vm)
add_test(NAME build COMMAND test_build)

add_executable(test_animation_vm tests/test_animation_vm.cpp)
target_link_libraries(test_animation_vm PRIVATE os2_vm)
add_test(NAME animation_vm COMMAND test_animation_vm)

if(APPLE)
  add_executable(test_camera tests/test_camera.cpp src/app/camera_follow.cpp)
  target_include_directories(test_camera PRIVATE src)
  target_link_libraries(test_camera PRIVATE os2_render os2_vm)
  add_test(NAME camera COMMAND test_camera)

  add_executable(test_dialog_window tests/test_dialog_window.cpp src/app/pie_menu.cpp src/app/dialog_window.cpp src/app/ui_panel_util.cpp)
  target_link_libraries(test_dialog_window PRIVATE os2_render os2_vm)
  add_test(NAME dialog_window COMMAND test_dialog_window)

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
add_executable(test_ui tests/test_ui.cpp src/app/ui_panel_util.cpp)
target_link_libraries(test_ui PRIVATE os2_formats)
add_test(NAME ui COMMAND test_ui)
add_executable(test_jump_detector tests/test_jump_detector.cpp src/app/jump_detector.cpp)
target_include_directories(test_jump_detector PRIVATE src)
add_test(NAME jump_detector COMMAND test_jump_detector)
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
add_executable(test_game_flow tests/test_game_flow.cpp src/app/game_flow.cpp src/app/user_data.cpp)
target_include_directories(test_game_flow PRIVATE src)
add_test(NAME game_flow COMMAND test_game_flow)
add_executable(test_hood_terrain tests/test_hood_terrain.cpp)
target_link_libraries(test_hood_terrain PRIVATE os2_formats)
add_test(NAME hood_terrain COMMAND test_hood_terrain)

add_executable(test_hood_moves tests/test_hood_moves.cpp)
target_link_libraries(test_hood_moves PRIVATE os2_formats)
add_test(NAME hood_moves COMMAND test_hood_moves)
add_executable(test_movie tests/test_movie.cpp)
target_link_libraries(test_movie PRIVATE os2_formats)
add_test(NAME movie COMMAND test_movie)
add_executable(test_effects tests/test_effects.cpp)
target_link_libraries(test_effects PRIVATE os2_fx)
add_test(NAME effects COMMAND test_effects)

add_executable(test_wants tests/test_wants.cpp)
target_link_libraries(test_wants PRIVATE os2_vm)
add_test(NAME wants COMMAND test_wants)

add_executable(test_tv_program tests/test_tv_program.cpp)
target_include_directories(test_tv_program PRIVATE src)
add_test(NAME tv_program COMMAND test_tv_program)

# --help prints the usage and exits without opening a window (r14).
add_test(NAME cli_help COMMAND opensims2 --help)
# With the game's data (OS2_DATA): Sims drawn without jumps the VM gave no
# reason for, seats and slots where their objects are (tests/drawn_places.sh).
add_test(NAME drawn_places COMMAND sh ${CMAKE_SOURCE_DIR}/tests/drawn_places.sh $<TARGET_FILE:opensims2> ${CMAKE_BINARY_DIR}/drawn_places)
set_tests_properties(drawn_places PROPERTIES PASS_REGULAR_EXPRESSION "drawn places: ok" TIMEOUT 600)
set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "usage: opensims2")
