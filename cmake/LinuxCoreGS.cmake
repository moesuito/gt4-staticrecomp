# TASK-074: CoreGS 2.0 (native/hw_gs, TASK-027) on Linux. Included by cmake/LinuxHost.cmake when GT4_LINUX_HW_GS=ON.
#
# The same targets, sources and tests as native/hw_gs/CMakeLists.txt, which stays the Windows build file. Differences:
#   - warnings are -Wall -Wextra (the Windows file's /W4), with -Wno-missing-field-initializers for the whole tree (the
#     Vulkan structures are initialised with their sType only, as the Windows file allows for its fixtures);
#   - Vulkan comes from the system (headers; the loader is dlopened at run time), not a Vulkan SDK import library;
#   - Windows-only files are replaced: hud/hw_hud_system.cpp (GDI font, PDH counters) by hud/hw_hud_system_linux.cpp
#     (FreeType and fontconfig for the font, /proc for the readings), hw_cpu_sampler.cpp (Win32 thread sampling) by
#     hw_cpu_sampler_linux.cpp (off);
#   - not built: hw_gs_smoke (a Win32 window test tool), hw_cpu_sampler_fixture (tests the Windows sampler);
#   - the shaders use the system glslangValidator. The Windows build pins its glslangValidator.exe by hash (IN-201);
#     no Linux binary is pinned yet, so its version and hash are printed and passed on as the expected values.
# Nothing in this file is clean-room sensitive: it lists files; no PCSX2 source was read to write it.

find_package(Threads REQUIRED)
set(GT4_HW "${GT4_NATIVE}/hw_gs")
set(GT4_HW_WARNINGS ${GT4_WARNINGS} -Wno-missing-field-initializers)

find_program(GT4_LINUX_GLSLANG glslangValidator REQUIRED)
file(SHA256 "${GT4_LINUX_GLSLANG}" gt4_linux_glslang_sha256)
execute_process(COMMAND "${GT4_LINUX_GLSLANG}" --version OUTPUT_VARIABLE gt4_glslang_version ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "Glslang Version: [^\n]*" gt4_glslang_version_line "${gt4_glslang_version}")
message(STATUS "CoreGS shaders on Linux: ${GT4_LINUX_GLSLANG}, ${gt4_glslang_version_line}, sha256 "
               "${gt4_linux_glslang_sha256} (no Linux pin yet)")
set(GT4_HW_GLSLANG "${GT4_LINUX_GLSLANG}" CACHE FILEPATH "glslangValidator for the CoreGS shaders" FORCE)
set(GT4_HW_GLSLANG_SHA256 "${gt4_linux_glslang_sha256}" CACHE STRING "Hash of the Linux glslangValidator in use" FORCE)

function(gt4_hw_compile target)
    target_compile_options(${target} PRIVATE ${GT4_HW_WARNINGS})
endfunction()

add_library(gt4_hw_options STATIC "${GT4_HW}/hw_gs_options.cpp")
gt4_hw_compile(gt4_hw_options)

add_library(gt4_hw_core STATIC
    "${GT4_HW}/cpuref/cpuref_memory.cpp" "${GT4_HW}/cpuref/cpuref_draw.cpp" "${GT4_HW}/memory/hw_memory.cpp"
    "${GT4_HW}/clut/hw_clut.cpp" "${GT4_HW}/frontend/hw_gs_registers.cpp" "${GT4_HW}/frontend/hw_gif_parser.cpp"
    "${GT4_HW}/frontend/hw_transfers.cpp" "${GT4_HW}/frontend/hw_batch.cpp" "${GT4_HW}/frontend/hw_front_end.cpp"
    "${GT4_HW}/pixel/hw_gs_pixel.cpp" "${GT4_HW}/display/hw_gs_display.cpp" "${GT4_HW}/core2/core2_upload.cpp"
    "${GT4_HW}/core2/core2_depth_alias.cpp")
target_include_directories(gt4_hw_core PUBLIC "${GT4_HW}/cpuref" "${GT4_HW}/memory" "${GT4_HW}/clut"
    "${GT4_HW}/frontend" "${GT4_HW}/pixel" "${GT4_HW}/display")
gt4_hw_compile(gt4_hw_core)

add_subdirectory("${GT4_HW}/shaders" "${CMAKE_BINARY_DIR}/hw_gs/shaders")

add_library(gt4_hw_gpu STATIC "${GT4_HW}/writeback/hw_gs_writeback_gpu.cpp"
    "${GT4_HW}/writeback/hw_gs_vulkan_device_table.cpp")
target_include_directories(gt4_hw_gpu PRIVATE "${GT4_HW}")
target_link_libraries(gt4_hw_gpu PUBLIC gt4_hw_shaders)
gt4_hw_compile(gt4_hw_gpu)

add_subdirectory("${GT4_HW}/cpuref" "${CMAKE_BINARY_DIR}/hw_gs/cpuref")
add_subdirectory("${GT4_HW}/cache" "${CMAKE_BINARY_DIR}/hw_gs/cache")
add_subdirectory("${GT4_HW}/decide" "${CMAKE_BINARY_DIR}/hw_gs/decide")
add_subdirectory("${GT4_HW}/lod" "${CMAKE_BINARY_DIR}/hw_gs/lod")
add_subdirectory("${GT4_HW}/replace" "${CMAKE_BINARY_DIR}/hw_gs/replace")

add_library(gt4_hw_writeback STATIC "${GT4_HW}/writeback/hw_gs_writeback.cpp"
    "${GT4_HW}/writeback/hw_gs_present_scheduler.cpp" "${GT4_HW}/core2/core2_renderer.cpp"
    "${GT4_HW}/core2/core2_gpu.cpp" "${GT4_HW}/core2/core2_replace.cpp")
target_link_libraries(gt4_hw_writeback PUBLIC gt4_hw_core gt4_hw_gpu hw_gs_lod hw_gs_replace Threads::Threads)
target_include_directories(gt4_hw_writeback PRIVATE "${GT4_HW}")
gt4_hw_compile(gt4_hw_writeback)

add_library(gt4_hw_vulkan_loader STATIC "${GT4_HW}/hw_gs_vulkan_loader.cpp")
target_include_directories(gt4_hw_vulkan_loader PUBLIC "${GT4_HW}")
target_link_libraries(gt4_hw_vulkan_loader PUBLIC ${CMAKE_DL_LIBS})
gt4_hw_compile(gt4_hw_vulkan_loader)

add_executable(hw_writeback_tests "${GT4_HW}/writeback/tests/hw_writeback_tests.cpp")
target_include_directories(hw_writeback_tests PRIVATE "${GT4_HW}")
target_link_libraries(hw_writeback_tests PRIVATE gt4_hw_writeback gt4_hw_vulkan_loader hw_gs_cpuref)
gt4_hw_compile(hw_writeback_tests)
add_executable(hw_writeback_replay "${GT4_HW}/writeback/hw_writeback_replay.cpp")
target_include_directories(hw_writeback_replay PRIVATE "${GT4_HW}")
target_link_libraries(hw_writeback_replay PRIVATE gt4_hw_writeback gt4_hw_vulkan_loader hw_gs_cpuref)
gt4_hw_compile(hw_writeback_replay)
# The same list as native/hw_gs/CMakeLists.txt; exit code 77 (no compute-capable Vulkan 1.2 device) is Skipped.
foreach(hw_writeback_test IN ITEMS hw_flat_provoking hw_sprite_corners hw_present_scheduler
        hw_present_scheduler_hold hw_present_scheduler_rate hw_aux_frame_guest_untouched
        hw_pass_timings hw_gpu_coverage_vectors
        hw_gpu_equivalence_flat hw_gpu_equivalence_texture hw_gpu_equivalence_region
        hw_gpu_equivalence_bilinear hw_gpu_equivalence_tfx_fog hw_gpu_equivalence_depth_alpha
        hw_gpu_equivalence_scissor hw_gpu_raster_blend hw_gpu_raster_pabe hw_gpu_raster_atest
        hw_gpu_raster_date hw_gpu_raster_frame hw_gpu_gouraud_report
        hw_gpu_writeback_aliased_depth hw_readback_pending_draw hw_replay_parity_double_buffer
        hw_presentation_field_phase hw_cache_equals_writeback hw_scale_exact hw_postfx_fxaa hw_scale_display_shadow
        hw_scale_fullscreen_passes hw_scale_interlaced_present hw_core2_interlaced_present hw_core2_cpu_sprites
        hw_core2_gpu_clut hw_core2_drawn_page_test hw_core2_width_grace hw_core2_glm_memo hw_core2_patch_bits
        hw_core2_patch_blocks hw_core2_over_target_memo hw_core2_mipmaps hw_core2_present_scale
        hw_core2_forced_bilinear hw_core2_specialised hw_core2_upload_memo hw_core2_depth_clear hw_core2_scanmsk
        hw_core2_display_memory hw_core2_shader_cache hw_core2_prepare_shaders hw_core2_shader_guard
        hw_core2_whole_clears hw_core2_rop hw_core2_road_sheen hw_core2_delayed_downloads hw_core2_compute_fills
        hw_core2_self_texture hw_core2_texture_replacement hw_core2_overlap_order
        hw_scale_target_extent hw_tone_hle_exact
        hw_glm_decode hw_glm_swizzle hw_glm_clut hw_glm_unswizzle hw_glm_upload_merge hw_glm_host_upload)
    add_test(NAME ${hw_writeback_test} COMMAND hw_writeback_tests ${hw_writeback_test})
    set_tests_properties(${hw_writeback_test} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endforeach()

find_package(Freetype REQUIRED)
find_package(Fontconfig REQUIRED)
add_library(gt4_hw_hud STATIC "${GT4_HW}/hud/hw_hud.cpp" "${GT4_HW}/hud/hw_hud_system_linux.cpp")
target_link_libraries(gt4_hw_hud PUBLIC Freetype::Freetype Fontconfig::Fontconfig Threads::Threads)
gt4_hw_compile(gt4_hw_hud)
# The fixture's font checks are labelled "Windows draws the font"; on Linux they check the FreeType atlas the same way.
add_executable(hw_hud_fixture "${GT4_HW}/hud/hw_hud_fixture.cpp")
target_link_libraries(hw_hud_fixture PRIVATE gt4_hw_hud)
add_test(NAME hw_hud COMMAND hw_hud_fixture)

add_library(gt4_hw_backend STATIC "${GT4_HW}/hw_gs_backend.cpp" "${GT4_HW}/hw_gs_dispatch.cpp"
    "${GT4_HW}/hud/hw_hud_layer.cpp")
target_link_libraries(gt4_hw_backend PUBLIC gt4_hw_hud gt4_hw_options gt4_pacing gt4_hw_writeback gt4_hw_vulkan_loader)
gt4_hw_compile(gt4_hw_backend)

add_executable(hw_options_fixture "${GT4_HW}/hw_options_fixture.cpp")
target_link_libraries(hw_options_fixture PRIVATE gt4_hw_options)
add_executable(hw_core2_upload_fixture "${GT4_HW}/core2/core2_upload_fixture.cpp")
target_include_directories(hw_core2_upload_fixture PRIVATE "${GT4_HW}")
target_link_libraries(hw_core2_upload_fixture PRIVATE gt4_hw_core)
add_test(NAME hw_core2_upload_swizzle COMMAND hw_core2_upload_fixture)
add_executable(hw_core2_depth_alias_fixture "${GT4_HW}/core2/core2_depth_alias_fixture.cpp")
target_include_directories(hw_core2_depth_alias_fixture PRIVATE "${GT4_HW}")
target_link_libraries(hw_core2_depth_alias_fixture PRIVATE gt4_hw_core)
add_test(NAME hw_core2_depth_alias COMMAND hw_core2_depth_alias_fixture)
foreach(cpu_fixture IN ITEMS flare_table present_latency present_spacing)
    add_executable(hw_${cpu_fixture}_fixture "${GT4_HW}/hw_${cpu_fixture}_fixture.cpp")
    target_include_directories(hw_${cpu_fixture}_fixture PRIVATE "${GT4_HW}" "${GT4_NATIVE}")
    add_test(NAME hw_${cpu_fixture} COMMAND hw_${cpu_fixture}_fixture)
endforeach()
add_test(NAME hw_options COMMAND $<TARGET_FILE:hw_options_fixture>)
