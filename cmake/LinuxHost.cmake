# TASK-074 (Linux build, plan step L1: host-only part; docs/linux/PLAN.md on branch linux/main): the Linux host-only build.
#
# Builds the platform-neutral host libraries and their CTest fixtures with Clang or GCC on Linux x86-64, so the pure
# rules (pacing, display aspect, hooks tables, controls, MTVU journal, memory card writer, performance accounting...) are
# checked on Linux too. It needs no private assets: no donor runtime, no EE game library, no SDL3 package, no Python
# environment. The game executable, the Vulkan/GPU fixtures and everything that talks to Win32 (window, WinMM/XInput
# input and audio, WinHTTP achievements, the GS stream shared-memory transport) are not built here yet; they are the
# later Linux steps (TASK-074 steps L1-L4).
#
# The Windows build never reads this file: the root CMakeLists.txt includes it only when WIN32 is false and then
# returns, so the qualified Windows recipe is unchanged.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
   OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "The host-only build outside Windows targets Linux x86-64")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|GNU)$")
    message(FATAL_ERROR "The Linux host build needs Clang or GCC (found ${CMAKE_CXX_COMPILER_ID})")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Same code-generation switches as the Windows tree (root CMakeLists.txt): SSE4.1 is the CPU floor; AVX2 and FMA
# contraction stay A/B options.
option(GT4_FP_CONTRACT_OFF "Build with -ffp-contract=off (control: no fused multiply-add)" OFF)
option(GT4_X86_64_V3 "Build for x86-64-v3 (AVX2, FMA); implies GT4_FP_CONTRACT_OFF" OFF)
option(GT4_SSE41 "Build for SSE4.1 (the launcher's CPU floor)" ON)
if(GT4_FP_CONTRACT_OFF OR GT4_X86_64_V3)
    add_compile_options(-ffp-contract=off)
endif()
if(GT4_X86_64_V3)
    add_compile_options(-march=x86-64-v3)
elseif(GT4_SSE41)
    add_compile_options(-msse4.1)
endif()

set(GT4_TARGET "v200" CACHE STRING "Game target for the host hooks")
set_property(CACHE GT4_TARGET PROPERTY STRINGS v200 spec2)
if(GT4_TARGET STREQUAL "spec2")
    add_compile_definitions(GT4_TARGET_SPEC2)
elseif(NOT GT4_TARGET STREQUAL "v200")
    message(FATAL_ERROR "Unknown GT4_TARGET '${GT4_TARGET}'; expected v200 or spec2")
endif()
# The perf accounting enum carries the MTVU categories in every target (native/CMakeLists.txt, TASK-011 step 3).
add_compile_definitions(GT4_MTVU_RUNTIME)

set(GT4_NATIVE "${CMAKE_SOURCE_DIR}/native")
# Counterpart of /W4 in the Windows tree.
set(GT4_WARNINGS -Wall -Wextra)

enable_testing()

# gt4_host_target(<name>): applies the shared include path and warning level to a host target.
function(gt4_host_target name)
    target_include_directories(${name} PRIVATE "${GT4_NATIVE}")
    target_compile_options(${name} PRIVATE ${GT4_WARNINGS})
endfunction()

function(gt4_host_library name)
    list(TRANSFORM ARGN PREPEND "${GT4_NATIVE}/")
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC "${GT4_NATIVE}")
    target_compile_options(${name} PRIVATE ${GT4_WARNINGS})
endfunction()

function(gt4_host_fixture name)
    list(TRANSFORM ARGN PREPEND "${GT4_NATIVE}/")
    add_executable(${name} ${ARGN})
    gt4_host_target(${name})
endfunction()

# Libraries (same names and sources as native/CMakeLists.txt).
gt4_host_library(gt4_pacing pacing.cpp)
gt4_host_library(gt4_quickfix frame_skip.cpp window_mode.cpp race_display.cpp race_widescreen.cpp cursor_autohide.cpp
    display_aspect.cpp flare_limits.cpp camera_360.cpp fov_adjust.cpp trigger_range.cpp)
if(GT4_TARGET STREQUAL "spec2")
    gt4_host_library(gt4_quickfix_v200_fixture race_display.cpp race_widescreen.cpp display_aspect.cpp flare_limits.cpp
        camera_360.cpp fov_adjust.cpp trigger_range.cpp)
    target_compile_definitions(gt4_quickfix_v200_fixture PUBLIC GT4_TARGET_FIXTURE_V200)
    set(GT4_HOOK_FIXTURE_LIB gt4_quickfix_v200_fixture)
else()
    set(GT4_HOOK_FIXTURE_LIB gt4_quickfix)
endif()
gt4_host_library(gt4_vulkan_extensions vulkan_extensions.cpp)
gt4_host_library(gt4_usb usb_ohci.cpp usb_dfp.cpp)
gt4_host_library(gt4_memcard memcard_store.cpp)
gt4_host_library(gt4_license_guard license_guard.cpp)
gt4_host_library(gt4_pad_mapping pad_mapping.cpp pad_choice.cpp input_recording.cpp)
gt4_host_library(gt4_perf_probe perf_probe.cpp perf_clock.cpp)
gt4_host_library(gt4_mtvu_measure mtvu_measure.cpp)
gt4_host_library(gt4_mtvu_journal mtvu_journal.cpp)
# The .gscap file format and the READ_FIFO probe only; the shared-memory transport (gs_stream_ipc.cpp) is Win32.
gt4_host_library(gt4_gs_stream_files gs_stream/gs_stream_file.cpp gs_stream/gs_readback_probe.cpp)
target_include_directories(gt4_gs_stream_files PUBLIC "${GT4_NATIVE}/gs_stream")

# Fixtures and their tests (test names match the Windows tree, so results compare one to one).
gt4_host_fixture(vulkan_extensions_fixture vulkan_extensions_fixture.cpp)
target_link_libraries(vulkan_extensions_fixture PRIVATE gt4_vulkan_extensions)
add_test(NAME native_vulkan_extensions COMMAND $<TARGET_FILE:vulkan_extensions_fixture>)

gt4_host_fixture(usb_dfp_fixture usb_dfp_fixture.cpp)
target_link_libraries(usb_dfp_fixture PRIVATE gt4_usb)
add_test(NAME native_usb_dfp COMMAND $<TARGET_FILE:usb_dfp_fixture>)

gt4_host_fixture(memcard_store_fixture memcard_store_fixture.cpp)
target_link_libraries(memcard_store_fixture PRIVATE gt4_memcard)
add_test(NAME native_memcard_store COMMAND $<TARGET_FILE:memcard_store_fixture> "${CMAKE_BINARY_DIR}/memcard_store_work")

gt4_host_fixture(license_guard_fixture license_guard_fixture.cpp)
target_link_libraries(license_guard_fixture PRIVATE gt4_license_guard)
add_test(NAME native_license_guard COMMAND $<TARGET_FILE:license_guard_fixture>)

gt4_host_fixture(standalone_fixture standalone_fixture.cpp standalone.cpp)
target_link_libraries(standalone_fixture PRIVATE gt4_pad_mapping)
target_compile_definitions(standalone_fixture PRIVATE GT4_STANDALONE_SAMPLE_DIR="${GT4_NATIVE}/standalone_samples")
add_test(NAME native_standalone COMMAND $<TARGET_FILE:standalone_fixture>)

gt4_host_fixture(game_target_fixture game_target_fixture.cpp game_target_spec2_fixture.cpp)
set_source_files_properties("${GT4_NATIVE}/game_target_fixture.cpp" PROPERTIES COMPILE_DEFINITIONS GT4_TARGET_FIXTURE_V200)
add_test(NAME native_game_target COMMAND $<TARGET_FILE:game_target_fixture>)

gt4_host_fixture(spec2_iop_fixture spec2_iop_fixture.cpp)
add_test(NAME native_spec2_iop COMMAND $<TARGET_FILE:spec2_iop_fixture>)

gt4_host_fixture(spec2_hooks_fixture spec2_hooks_fixture.cpp race_widescreen.cpp race_display.cpp license_guard.cpp)
target_compile_definitions(spec2_hooks_fixture PRIVATE GT4_TARGET_SPEC2)
add_test(NAME native_spec2_hooks COMMAND $<TARGET_FILE:spec2_hooks_fixture>)

gt4_host_fixture(mtvu_measure_fixture mtvu_measure_fixture.cpp)
target_link_libraries(mtvu_measure_fixture PRIVATE gt4_mtvu_measure)
add_test(NAME native_mtvu_measure COMMAND $<TARGET_FILE:mtvu_measure_fixture>)

gt4_host_fixture(mtvu_ring_fixture mtvu_ring_fixture.cpp)
add_test(NAME native_mtvu_ring COMMAND $<TARGET_FILE:mtvu_ring_fixture>)

gt4_host_fixture(mtvu_journal_fixture mtvu_journal_fixture.cpp)
target_link_libraries(mtvu_journal_fixture PRIVATE gt4_mtvu_journal)
add_test(NAME native_mtvu_journal COMMAND $<TARGET_FILE:mtvu_journal_fixture>)

gt4_host_fixture(mtvu_perf_fixture mtvu_perf_fixture.cpp)
target_link_libraries(mtvu_perf_fixture PRIVATE gt4_perf_probe)
add_test(NAME native_mtvu_perf COMMAND $<TARGET_FILE:mtvu_perf_fixture>)

gt4_host_fixture(mtvu_vif_batch_fixture mtvu_vif_batch_fixture.cpp)
add_test(NAME native_mtvu_vif_batch COMMAND $<TARGET_FILE:mtvu_vif_batch_fixture>)

gt4_host_fixture(mtvu_switch_fixture mtvu_switch_fixture.cpp)
add_test(NAME native_mtvu_switches COMMAND $<TARGET_FILE:mtvu_switch_fixture>)

gt4_host_fixture(gs_readback_probe_fixture gs_stream/gs_readback_probe_fixture.cpp)
target_link_libraries(gs_readback_probe_fixture PRIVATE gt4_gs_stream_files)
add_test(NAME native_gs_readback_probe
    COMMAND $<TARGET_FILE:gs_readback_probe_fixture> "${CMAKE_BINARY_DIR}/gs-readback-probe-work")

gt4_host_fixture(gs_submit_ring_fixture gs_submit_ring_fixture.cpp)
add_test(NAME native_gs_submit_ring COMMAND $<TARGET_FILE:gs_submit_ring_fixture>)

gt4_host_fixture(spu2_envelope_fixture spu2_envelope_fixture.cpp)
add_test(NAME native_spu2_envelope COMMAND $<TARGET_FILE:spu2_envelope_fixture>)

gt4_host_fixture(ipu_fast_fixture ipu_fast_fixture.cpp)
add_test(NAME native_ipu_fast COMMAND $<TARGET_FILE:ipu_fast_fixture>)

gt4_host_fixture(frame_skip_fixture frame_skip_fixture.cpp frame_skip.cpp)
target_link_libraries(frame_skip_fixture PRIVATE gt4_pacing)
add_test(NAME native_frame_skip COMMAND $<TARGET_FILE:frame_skip_fixture>)

gt4_host_fixture(pacing_fixture pacing_fixture.cpp)
target_link_libraries(pacing_fixture PRIVATE gt4_pacing)
add_test(NAME native_pacing COMMAND $<TARGET_FILE:pacing_fixture>)

gt4_host_fixture(controls_fixture controls_fixture.cpp controls.cpp)
target_compile_definitions(controls_fixture PRIVATE GT4_CONTROLS_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/docs/controls")
add_test(NAME native_controls COMMAND $<TARGET_FILE:controls_fixture>)

gt4_host_fixture(input_recording_fixture input_recording_fixture.cpp)
target_link_libraries(input_recording_fixture PRIVATE gt4_pad_mapping)
add_test(NAME native_input_recording COMMAND $<TARGET_FILE:input_recording_fixture> native_input_recording.txt)

gt4_host_fixture(pad_mapping_fixture pad_mapping_fixture.cpp controls.cpp)
target_link_libraries(pad_mapping_fixture PRIVATE gt4_pad_mapping)
add_test(NAME native_pad_mapping COMMAND $<TARGET_FILE:pad_mapping_fixture>)

gt4_host_fixture(shape_observer_fixture shape_observer_fixture.cpp shape_observer.cpp)
add_test(NAME native_shape_observer COMMAND $<TARGET_FILE:shape_observer_fixture>)

gt4_host_fixture(perf_accounting_fixture perf_accounting_fixture.cpp)
add_test(NAME native_performance_accounting COMMAND $<TARGET_FILE:perf_accounting_fixture>)

gt4_host_fixture(perf_probe_fixture perf_probe_fixture.cpp)
target_link_libraries(perf_probe_fixture PRIVATE gt4_perf_probe)
add_test(NAME native_performance_probe COMMAND $<TARGET_FILE:perf_probe_fixture>)

gt4_host_fixture(fpu_chop_fixture fpu_chop_fixture.cpp)
add_test(NAME native_fpu_chop COMMAND $<TARGET_FILE:fpu_chop_fixture>)

gt4_host_fixture(quickfix_fixture quickfix_fixture.cpp)
target_link_libraries(quickfix_fixture PRIVATE gt4_quickfix)
add_test(NAME native_quickfix COMMAND $<TARGET_FILE:quickfix_fixture>)

gt4_host_fixture(cursor_autohide_fixture cursor_autohide_fixture.cpp)
target_link_libraries(cursor_autohide_fixture PRIVATE gt4_quickfix)
add_test(NAME native_cursor_autohide COMMAND $<TARGET_FILE:cursor_autohide_fixture>)

# TASK-074 L2: the controls table and its keyboard scancodes (pure), used by the Linux window host.
gt4_host_library(gt4_controls controls.cpp keyboard_scancodes.cpp)
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    gt4_host_fixture(keyboard_scancodes_fixture keyboard_scancodes_fixture.cpp)
    target_link_libraries(keyboard_scancodes_fixture PRIVATE gt4_controls SDL3::Headers)
    add_test(NAME native_keyboard_scancodes COMMAND $<TARGET_FILE:keyboard_scancodes_fixture>)
    # TASK-074 L2: the Linux frame clock and the paced audio target (no audio device is opened).
    gt4_host_fixture(linux_pacing_fixture linux/linux_pacing_fixture.cpp linux/sdl_audio.cpp frame_clock.cpp)
    target_link_libraries(linux_pacing_fixture PRIVATE gt4_pacing SDL3::SDL3)
    add_test(NAME native_linux_pacing COMMAND $<TARGET_FILE:linux_pacing_fixture>)
    set_tests_properties(native_linux_pacing PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
endif()

# The guest-hook fixtures assert the v2.00 chains (GT4_HOOK_FIXTURE_LIB, as in the Windows tree).
foreach(hook IN ITEMS race_display race_widescreen display_aspect camera_360 fov_adjust trigger_range flare_limits)
    gt4_host_fixture(${hook}_fixture ${hook}_fixture.cpp)
    target_link_libraries(${hook}_fixture PRIVATE ${GT4_HOOK_FIXTURE_LIB})
    add_test(NAME native_${hook} COMMAND $<TARGET_FILE:${hook}_fixture>)
endforeach()

foreach(v200_fixture IN ITEMS license_guard_fixture race_display_fixture race_widescreen_fixture display_aspect_fixture
                               shape_observer_fixture)
    target_compile_definitions(${v200_fixture} PRIVATE GT4_TARGET_FIXTURE_V200)
endforeach()

# Same scheduling as the Windows tree: fixtures that write files or measure time run alone.
set_tests_properties(native_mtvu_measure native_mtvu_ring native_mtvu_journal native_mtvu_perf native_mtvu_vif_batch
    native_quickfix native_race_display native_race_widescreen native_shape_observer native_performance_accounting
    native_performance_probe native_gs_submit_ring native_flare_limits native_gs_readback_probe
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# TASK-074: CoreGS 2.0 (native/hw_gs) libraries, shaders and their tests; off by default (cmake/LinuxCoreGS.cmake).
option(GT4_LINUX_HW_GS "Build CoreGS 2.0 (native/hw_gs) on Linux" OFF)
if(GT4_LINUX_HW_GS)
    include("${CMAKE_SOURCE_DIR}/cmake/LinuxCoreGS.cmake")
endif()

# Optional TASK-074 L1 targets, all off by default (the host-only build above needs none of them):
#   GT4_DONOR + GT4_LINUX_PRODUCER  -> gt4_headless_smoke: the donor runtime with its CPU rasterizer, the donor's
#                                      headless host and a recorded Linux game library.
#   GT4_DONOR + GT4_LINUX_PORT_DIR + GT4_LINUX_SHADER_DIR
#                                   -> the native Vulkan GS backend (native_vulkan.cpp, headless on Linux) with
#                                      vulkan_fixture and its CTest entries; with GT4_LINUX_PRODUCER as well,
#                                      gt4_vulkan_headless_smoke (the game drawn by the Vulkan GS backend).
# The runtime source list is RUNTIME_SOURCES of native/CMakeLists.txt. VU1 microcode runs in the donor interpreter
# (the weak register_generated_vu). No window, audio or input: L2 adds those.
set(GT4_DONOR "" CACHE PATH "Verified ps2-web-recomp checkout (private/references/ps2-web-recomp)")
set(GT4_LINUX_PRODUCER "" CACHE PATH "Linux candidate EE producer directory holding linux-library.json")
set(GT4_LINUX_PORT_DIR "" CACHE PATH "Native Vulkan port derived by scripts/prepare_external_vulkan.py")
set(GT4_LINUX_SHADER_DIR "" CACHE PATH "SPIR-V shaders built by tools/wgsl_spirv from the donor gs.wgsl")
if((GT4_LINUX_PRODUCER OR GT4_LINUX_PORT_DIR OR GT4_LINUX_SHADER_DIR) AND NOT GT4_DONOR)
    message(FATAL_ERROR "The optional Linux runtime targets need GT4_DONOR")
endif()
if(GT4_DONOR)
    if(NOT EXISTS "${GT4_DONOR}/runtime/include/ps2/system.hpp" OR NOT EXISTS "${GT4_DONOR}/host/main.cpp")
        message(FATAL_ERROR "GT4_DONOR is not a ps2-web-recomp checkout; run scripts/ee_producer.py prepare first")
    endif()
    find_package(Threads REQUIRED)
    set(gt4_donor_runtime)
    foreach(source IN ITEMS memory dispatch system kernel hw cop disc dmac vif vu_micro vu_jit vu_jit_web
                            ipu/ipu gs/gif gs/queue gs/raster iop/cpu iop/kernel iop/libs iop/hw iop/spu2 iop/sio2
                            iop/io iop/cdvd iop/sif iop/cdvdfsv)
        list(APPEND gt4_donor_runtime "${GT4_DONOR}/runtime/src/${source}.cpp")
    endforeach()
endif()

if(GT4_DONOR)
    # TASK-074 L2: the VU microcode recompiler (tools/vu_recomp) and its standalone controls, as in its own
    # CMakeLists.txt (which the Windows tree uses); they need only the donor headers.
    set(gt4_vu_src "${CMAKE_SOURCE_DIR}/tools/vu_recomp/src")
    set(gt4_vu_tests "${CMAKE_SOURCE_DIR}/tools/vu_recomp/tests")
    set(gt4_vu_donor_includes "${GT4_DONOR}/runtime/include" "${GT4_DONOR}/runtime/src")
    add_library(gt4_vu_recomp_core STATIC "${gt4_vu_src}/vurecomp.cpp" "${gt4_vu_src}/sha256.cpp")
    target_include_directories(gt4_vu_recomp_core PUBLIC "${gt4_vu_src}" ${gt4_vu_donor_includes})
    target_compile_options(gt4_vu_recomp_core PRIVATE ${GT4_WARNINGS})
    add_executable(gt4_vu_recomp "${gt4_vu_src}/vurecomp_main.cpp")
    target_link_libraries(gt4_vu_recomp PRIVATE gt4_vu_recomp_core)
    add_library(gt4_vu_fixtures STATIC "${gt4_vu_tests}/vu_fixtures.cpp")
    target_include_directories(gt4_vu_fixtures PUBLIC "${gt4_vu_tests}" ${gt4_vu_donor_includes})
    add_executable(gt4_vu_recomp_core_tests "${gt4_vu_tests}/core_tests.cpp")
    target_link_libraries(gt4_vu_recomp_core_tests PRIVATE gt4_vu_recomp_core gt4_vu_fixtures)
    add_executable(gt4_vu0_emitter_tests "${gt4_vu_tests}/vu0_emitter_tests.cpp")
    target_link_libraries(gt4_vu0_emitter_tests PRIVATE gt4_vu_recomp_core)
    add_test(NAME vu_recomp_core COMMAND $<TARGET_FILE:gt4_vu_recomp_core_tests>)
    add_test(NAME vu0_emitter COMMAND $<TARGET_FILE:gt4_vu0_emitter_tests>)
    set_tests_properties(vu_recomp_core vu0_emitter PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if(GT4_LINUX_PRODUCER)
    # The library must be the one ee_producer.py record-library verified and recorded, byte for byte.
    set(gt4_linux_library "${GT4_LINUX_PRODUCER}/game-build/libps2game.a")
    if(NOT EXISTS "${GT4_LINUX_PRODUCER}/linux-library.json" OR NOT EXISTS "${gt4_linux_library}")
        message(FATAL_ERROR "GT4_LINUX_PRODUCER has no recorded Linux library; run ee_producer.py record-library")
    endif()
    file(READ "${GT4_LINUX_PRODUCER}/linux-library.json" gt4_linux_receipt)
    string(JSON gt4_linux_expected GET "${gt4_linux_receipt}" library sha256)
    file(SHA256 "${gt4_linux_library}" gt4_linux_actual)
    if(NOT gt4_linux_actual STREQUAL gt4_linux_expected)
        message(FATAL_ERROR "libps2game.a differs from its linux-library.json record")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GT4_LINUX_PRODUCER}/linux-library.json"
        "${gt4_linux_library}")

    # gs/gs.cpp holds the GS model; without PS2_GPU it draws with the donor's CPU rasterizer.
    add_library(gt4_ps2runtime_headless STATIC ${gt4_donor_runtime} "${GT4_DONOR}/runtime/src/gs/gs.cpp")
    target_include_directories(gt4_ps2runtime_headless PUBLIC "${GT4_DONOR}/runtime/include"
        PRIVATE "${GT4_DONOR}/runtime/src" "${GT4_DONOR}/runtime/src/gs")
    target_link_libraries(gt4_ps2runtime_headless PUBLIC Threads::Threads)

    add_executable(gt4_headless_smoke "${GT4_DONOR}/host/main.cpp")
    # The game library and the runtime call each other (dispatch tables and runtime services): link them as a group.
    target_link_libraries(gt4_headless_smoke PRIVATE
        "$<LINK_GROUP:RESCAN,${gt4_linux_library},gt4_ps2runtime_headless>")
endif()

if(GT4_LINUX_PORT_DIR OR GT4_LINUX_SHADER_DIR)
    if(NOT GT4_LINUX_PORT_DIR OR NOT GT4_LINUX_SHADER_DIR)
        message(FATAL_ERROR "The Linux Vulkan backend needs both GT4_LINUX_PORT_DIR and GT4_LINUX_SHADER_DIR")
    endif()
    foreach(port_file IN ITEMS gpu.cpp gpu.hpp gs.cpp main.cpp)
        if(NOT EXISTS "${GT4_LINUX_PORT_DIR}/${port_file}")
            message(FATAL_ERROR "GT4_LINUX_PORT_DIR lacks ${port_file}; run scripts/prepare_external_vulkan.py")
        endif()
    endforeach()
    # The shaders are naga's output for the pinned donor gs.wgsl; they must be byte-identical to the Windows pins.
    file(READ "${CMAKE_SOURCE_DIR}/docs/inputs/slice114-native-base.json" gt4_shader_manifest)
    string(JSON gt4_dependency_count LENGTH "${gt4_shader_manifest}" runtime_dependencies)
    math(EXPR gt4_dependency_last "${gt4_dependency_count} - 1")
    set(gt4_shader_count 0)
    foreach(index RANGE ${gt4_dependency_last})
        string(JSON gt4_dependency_path GET "${gt4_shader_manifest}" runtime_dependencies ${index} path)
        if(gt4_dependency_path MATCHES "/shaders/([a-z_]+\\.spv)$")
            set(gt4_shader "${GT4_LINUX_SHADER_DIR}/${CMAKE_MATCH_1}")
            string(JSON gt4_shader_expected GET "${gt4_shader_manifest}" runtime_dependencies ${index} sha256)
            if(NOT EXISTS "${gt4_shader}")
                message(FATAL_ERROR "Missing shader ${gt4_shader}; run tools/wgsl_spirv")
            endif()
            file(SHA256 "${gt4_shader}" gt4_shader_actual)
            if(NOT gt4_shader_actual STREQUAL gt4_shader_expected)
                message(FATAL_ERROR "${gt4_shader} differs from docs/inputs/slice114-native-base.json")
            endif()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gt4_shader}")
            math(EXPR gt4_shader_count "${gt4_shader_count} + 1")
        endif()
    endforeach()
    if(NOT gt4_shader_count EQUAL 9)
        message(FATAL_ERROR "Expected 9 pinned shaders, found ${gt4_shader_count}")
    endif()

    gt4_host_library(gt4_install_paths install_paths.cpp)

    # TASK-074 L2 performance: the release profile's VU path. GT4_LINUX_RUNTIME_CAPSULE is a capsule from
    # scripts/prepare_perf_runtime.py --mtvu (the VU1 worker thread, with the VU0/VU1 AOT registries); its seven sources
    # must equal the slice178 pins. GT4_LINUX_VU1_GENERATED_DIR / GT4_LINUX_VU0_GENERATED_DIR are tools/vu_recomp output
    # (production command lines in docs/linux/L2-PERFORMANCE.md) whose tables must equal the slice408 (1.3.0-rc14) pins.
    # All three together or none; without them the runtime is the donor's (VU interpreter, no worker).
    set(GT4_LINUX_RUNTIME_CAPSULE "" CACHE PATH "prepare_perf_runtime.py --mtvu capsule")
    set(GT4_LINUX_VU1_GENERATED_DIR "" CACHE PATH "gt4_vu_recomp VU1 output (vu1_generated.cpp)")
    set(GT4_LINUX_VU0_GENERATED_DIR "" CACHE PATH "gt4_vu_recomp VU0 output (vu0_generated.cpp)")
    set(gt4_linux_aot OFF)
    if(GT4_LINUX_RUNTIME_CAPSULE OR GT4_LINUX_VU1_GENERATED_DIR OR GT4_LINUX_VU0_GENERATED_DIR)
        if(NOT GT4_LINUX_RUNTIME_CAPSULE OR NOT GT4_LINUX_VU1_GENERATED_DIR OR NOT GT4_LINUX_VU0_GENERATED_DIR)
            message(FATAL_ERROR "The Linux VU AOT runtime needs GT4_LINUX_RUNTIME_CAPSULE, GT4_LINUX_VU1_GENERATED_DIR "
                                "and GT4_LINUX_VU0_GENERATED_DIR together")
        endif()
        set(gt4_capsule_sources system.cpp vif.cpp vu_micro.cpp gif.cpp memory.cpp hw.cpp dmac.cpp)
        file(GLOB gt4_capsule_files RELATIVE "${GT4_LINUX_RUNTIME_CAPSULE}" "${GT4_LINUX_RUNTIME_CAPSULE}/*")
        list(SORT gt4_capsule_files)
        set(gt4_capsule_expected runtime.json ${gt4_capsule_sources})
        list(SORT gt4_capsule_expected)
        if(NOT gt4_capsule_files STREQUAL gt4_capsule_expected)
            message(FATAL_ERROR "GT4_LINUX_RUNTIME_CAPSULE must hold exactly the --mtvu sources and runtime.json")
        endif()
        # Three pinned profiles: prepare_perf_runtime.py --mtvu (slice178), --gs-stream (slice191: the same plus the GS
        # stream hooks in gif.cpp and hw.cpp, which CoreGS needs) and --gs-stream --frame-cost (slice262: the release's
        # source capsule, with the TASK-084 frame-cost hooks). The capsule must equal one of them in every source.
        set(gt4_capsule_profile "")
        foreach(candidate IN ITEMS slice178-main-runtime slice191-gs-stream-runtime slice262-frame-cost-runtime)
            file(READ "${CMAKE_SOURCE_DIR}/docs/inputs/${candidate}.json" gt4_capsule_manifest)
            set(gt4_capsule_matches ON)
            foreach(source IN LISTS gt4_capsule_sources)
                string(JSON gt4_capsule_expected_hash GET "${gt4_capsule_manifest}" source_sha256 "${source}")
                file(SHA256 "${GT4_LINUX_RUNTIME_CAPSULE}/${source}" gt4_capsule_actual_hash)
                if(NOT gt4_capsule_actual_hash STREQUAL gt4_capsule_expected_hash)
                    set(gt4_capsule_matches OFF)
                endif()
            endforeach()
            if(gt4_capsule_matches)
                set(gt4_capsule_profile "${candidate}")
                break()
            endif()
        endforeach()
        if(NOT gt4_capsule_profile)
            message(FATAL_ERROR "GT4_LINUX_RUNTIME_CAPSULE differs from docs/inputs/slice178-main-runtime.json and "
                                "slice191-gs-stream-runtime.json; derive it with prepare_perf_runtime.py --mtvu or --gs-stream")
        endif()
        message(STATUS "Linux runtime capsule: ${gt4_capsule_profile} (every source equals its pin)")
        set(gt4_capsule_gs_stream OFF)
        if(gt4_capsule_profile MATCHES "^slice(191-gs-stream|262-frame-cost)-runtime$")
            set(gt4_capsule_gs_stream ON)
        endif()
        if(gt4_capsule_gs_stream AND NOT GT4_LINUX_HW_GS)
            message(FATAL_ERROR "The --gs-stream capsule feeds CoreGS: configure with -DGT4_LINUX_HW_GS=ON")
        endif()
        foreach(source IN LISTS gt4_capsule_sources)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GT4_LINUX_RUNTIME_CAPSULE}/${source}")
            # The capsule copy replaces the donor file (gif.cpp lives in gs/ in the donor).
            set(gt4_donor_path "${GT4_DONOR}/runtime/src/${source}")
            if(source STREQUAL "gif.cpp")
                set(gt4_donor_path "${GT4_DONOR}/runtime/src/gs/gif.cpp")
            endif()
            list(REMOVE_ITEM gt4_donor_runtime "${gt4_donor_path}")
            list(APPEND gt4_donor_runtime "${GT4_LINUX_RUNTIME_CAPSULE}/${source}")
        endforeach()
        # The pinned vu_micro.cpp (scripts/vu1_aot_overlay.inc, vu0_aot_overlay.inc) has two lambdas that return both
        # std::strtoull (unsigned long long) and std::uint64_t: one type with MSVC, two types on Linux (unsigned long), so
        # clang and GCC refuse them. The pinned file is checked above; the build compiles a copy with exactly those two
        # returns cast to std::uint64_t (the same value). The lasting fix belongs in the two overlays, with new pins.
        file(READ "${GT4_LINUX_RUNTIME_CAPSULE}/vu_micro.cpp" gt4_vu_micro)
        set(gt4_report_return "if (period) return std::strtoull(period, nullptr, 10);")
        string(REPLACE "${gt4_report_return}" "" gt4_vu_micro_without "${gt4_vu_micro}")
        string(LENGTH "${gt4_vu_micro}" gt4_length_with)
        string(LENGTH "${gt4_vu_micro_without}" gt4_length_without)
        string(LENGTH "${gt4_report_return}" gt4_length_return)
        math(EXPR gt4_return_count "(${gt4_length_with} - ${gt4_length_without}) / ${gt4_length_return}")
        if(NOT gt4_return_count EQUAL 2)
            message(FATAL_ERROR "Expected the two report_every returns in the capsule vu_micro.cpp, found ${gt4_return_count}")
        endif()
        string(REPLACE "${gt4_report_return}"
            "if (period) return static_cast<std::uint64_t>(std::strtoull(period, nullptr, 10));" gt4_vu_micro "${gt4_vu_micro}")
        file(WRITE "${CMAKE_BINARY_DIR}/linux_capsule/vu_micro.cpp" "${gt4_vu_micro}")
        list(REMOVE_ITEM gt4_donor_runtime "${GT4_LINUX_RUNTIME_CAPSULE}/vu_micro.cpp")
        list(APPEND gt4_donor_runtime "${CMAKE_BINARY_DIR}/linux_capsule/vu_micro.cpp")
        file(READ "${CMAKE_SOURCE_DIR}/docs/inputs/slice408-pcsx2.json" gt4_release_manifest)
        string(JSON gt4_vu1_expected GET "${gt4_release_manifest}" vu1_aot generated_source_sha256)
        string(JSON gt4_vu0_expected GET "${gt4_release_manifest}" vu0_aot table_sha256)
        file(SHA256 "${GT4_LINUX_VU1_GENERATED_DIR}/vu1_generated.cpp" gt4_vu1_actual)
        file(SHA256 "${GT4_LINUX_VU0_GENERATED_DIR}/vu0_generated.cpp" gt4_vu0_actual)
        if(NOT gt4_vu1_actual STREQUAL gt4_vu1_expected OR NOT gt4_vu0_actual STREQUAL gt4_vu0_expected)
            message(FATAL_ERROR "The VU1/VU0 tables differ from docs/inputs/slice408-pcsx2.json")
        endif()
        file(GLOB gt4_vu1_parts "${GT4_LINUX_VU1_GENERATED_DIR}/vu1_generated_*.cpp")
        list(SORT gt4_vu1_parts)
        set(gt4_vu_generated "${GT4_LINUX_VU1_GENERATED_DIR}/vu1_generated.cpp" ${gt4_vu1_parts}
            "${GT4_LINUX_VU0_GENERATED_DIR}/vu0_generated.cpp")
        set_source_files_properties(${gt4_vu_generated} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
        list(APPEND gt4_donor_runtime ${gt4_vu_generated} "${CMAKE_SOURCE_DIR}/tools/vu_recomp/src/sha256.cpp"
            "${GT4_NATIVE}/mtvu_bridge.cpp")
        gt4_host_library(gt4_mtvu_runtime mtvu_runtime.cpp)
        target_link_libraries(gt4_mtvu_runtime PUBLIC gt4_mtvu_journal gt4_mtvu_measure gt4_perf_probe gt4_quickfix)
        # The perf accounting layout must match in every unit (native/CMakeLists.txt does the same).
        target_compile_definitions(gt4_perf_probe PUBLIC GT4_PERF_VU_SPLIT GT4_VU1_AOT GT4_VU0_AOT)
        set(gt4_linux_aot ON)
    endif()

    # TASK-017/TASK-080: the license guard capsule of the release (prepare_license_guard.py --display-aspect
    # --frame-counters, with the license guard overlay of the slice262 capsule, git bc6d3d04): the license demo guard, the
    # race camera aspect hook and the HUD canvas hook (dispatch.cpp), the halt dump (kernel.cpp, with the frame
    # counters) and the FSV trace (cdvdfsv.cpp). Its three sources must equal the slice408 (1.3.0-rc14) pins.
    set(GT4_LINUX_LICENSE_GUARD "" CACHE PATH "prepare_license_guard.py capsule equal to the release's slice262 guard")
    if(GT4_LINUX_LICENSE_GUARD)
        file(GLOB gt4_guard_files RELATIVE "${GT4_LINUX_LICENSE_GUARD}" "${GT4_LINUX_LICENSE_GUARD}/*")
        list(SORT gt4_guard_files)
        if(NOT gt4_guard_files STREQUAL "cdvdfsv.cpp;dispatch.cpp;kernel.cpp;runtime.json")
            message(FATAL_ERROR "GT4_LINUX_LICENSE_GUARD must hold exactly cdvdfsv.cpp, dispatch.cpp, kernel.cpp and runtime.json")
        endif()
        file(READ "${CMAKE_SOURCE_DIR}/docs/inputs/slice408-pcsx2.json" gt4_guard_manifest)
        foreach(source IN ITEMS cdvdfsv.cpp dispatch.cpp kernel.cpp)
            string(JSON gt4_guard_expected GET "${gt4_guard_manifest}" license_guard_source_sha256 "${source}")
            file(SHA256 "${GT4_LINUX_LICENSE_GUARD}/${source}" gt4_guard_actual)
            if(NOT gt4_guard_actual STREQUAL gt4_guard_expected)
                message(FATAL_ERROR "License guard ${source} differs from docs/inputs/slice408-pcsx2.json")
            endif()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GT4_LINUX_LICENSE_GUARD}/${source}")
        endforeach()
        list(REMOVE_ITEM gt4_donor_runtime "${GT4_DONOR}/runtime/src/dispatch.cpp" "${GT4_DONOR}/runtime/src/kernel.cpp"
             "${GT4_DONOR}/runtime/src/iop/cdvdfsv.cpp")
        list(APPEND gt4_donor_runtime "${GT4_LINUX_LICENSE_GUARD}/dispatch.cpp" "${GT4_LINUX_LICENSE_GUARD}/kernel.cpp"
             "${GT4_LINUX_LICENSE_GUARD}/cdvdfsv.cpp")
        set_source_files_properties("${GT4_LINUX_LICENSE_GUARD}/cdvdfsv.cpp" PROPERTIES
            INCLUDE_DIRECTORIES "${GT4_DONOR}/runtime/src/iop")
        message(STATUS "Linux license guard capsule: equals the slice408 pins")
    endif()

    add_library(gt4_ps2runtime_vulkan STATIC ${gt4_donor_runtime} "${GT4_LINUX_PORT_DIR}/gs.cpp"
        "${GT4_LINUX_PORT_DIR}/gpu.cpp" "${GT4_NATIVE}/native_vulkan.cpp")
    # TASK-134 fast headers, on by default as in native/CMakeLists.txt: the donor's runtime headers with exact faster forms
    # of fp::add, fp::mul, the VU0 macro operations (native/fp4.hpp) and vfix. The release VU0 table includes ps2/fp4.hpp
    # from this folder. native_vu_fast_ops compares each form with the donor's.
    option(GT4_FAST_HEADERS "Build the runtime with the exact faster donor headers" ON)
    if(GT4_FAST_HEADERS)
        set(GT4_PYTHON "${CMAKE_SOURCE_DIR}/private/tooling-venv/bin/python" CACHE FILEPATH "Python for the derivation scripts")
        set(gt4_fast_headers "${CMAKE_BINARY_DIR}/fast_headers")
        execute_process(COMMAND "${GT4_PYTHON}" -I "${CMAKE_SOURCE_DIR}/scripts/prepare_fast_headers.py" "${GT4_DONOR}"
                        "${gt4_fast_headers}" RESULT_VARIABLE gt4_fast_result)
        if(NOT gt4_fast_result EQUAL 0)
            message(FATAL_ERROR "scripts/prepare_fast_headers.py failed")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/scripts/prepare_fast_headers.py"
            "${GT4_NATIVE}/fp4.hpp")
        target_include_directories(gt4_ps2runtime_vulkan BEFORE PUBLIC "${gt4_fast_headers}")
        add_executable(vu_fast_ops_fixture "${GT4_NATIVE}/vu_fast_ops_fixture.cpp")
        target_include_directories(vu_fast_ops_fixture PRIVATE "${gt4_fast_headers}" "${CMAKE_SOURCE_DIR}/tools/vu_recomp/src"
            "${GT4_DONOR}/runtime/include" "${GT4_DONOR}/runtime/src" "${GT4_NATIVE}")
        add_test(NAME native_vu_fast_ops COMMAND $<TARGET_FILE:vu_fast_ops_fixture>)
    elseif(gt4_linux_aot)
        message(FATAL_ERROR "The release VU0 table needs GT4_FAST_HEADERS (it includes ps2/fp4.hpp)")
    endif()
    if(GT4_LINUX_LICENSE_GUARD)
        target_link_libraries(gt4_ps2runtime_vulkan PUBLIC gt4_license_guard gt4_quickfix)
    endif()

    # TASK-074 game patches: the game library from the release's translated units (the slice217 aspect units with the
    # hook sites of scripts/camera360_units.py), built with the fast headers as the release builds it. The folder must
    # equal the slice408 output inventory pin, else the build stops.
    set(GT4_LINUX_GAME_UNITS "" CACHE PATH "camera360_units.py output equal to the release's units (slice408 pin)")
    if(GT4_LINUX_GAME_UNITS)
        if(NOT GT4_FAST_HEADERS)
            message(FATAL_ERROR "The release units are built with the fast headers (GT4_FAST_HEADERS=ON)")
        endif()
        execute_process(COMMAND "${GT4_PYTHON}" -c [=[
import json, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import camera360_units as units
digest = units.inventory_digest(units.inventory(Path(sys.argv[2])))
pin = json.load(open(sys.argv[3]))['camera360_units']['output_inventory_sha256']
print(digest)
sys.exit(0 if digest == pin else 1)
]=] "${CMAKE_SOURCE_DIR}/scripts" "${GT4_LINUX_GAME_UNITS}" "${CMAKE_SOURCE_DIR}/docs/inputs/slice408-pcsx2.json"
            RESULT_VARIABLE gt4_units_result OUTPUT_VARIABLE gt4_units_digest OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT gt4_units_result EQUAL 0)
            message(FATAL_ERROR "GT4_LINUX_GAME_UNITS inventory ${gt4_units_digest} differs from the slice408 pin")
        endif()
        message(STATUS "Linux game units: release inventory ${gt4_units_digest}")
        file(GLOB gt4_game_unit_sources "${GT4_LINUX_GAME_UNITS}/*.cpp")
        add_library(gt4_game_units STATIC ${gt4_game_unit_sources})
        target_include_directories(gt4_game_units BEFORE PRIVATE "${gt4_fast_headers}")
        target_include_directories(gt4_game_units PRIVATE "${GT4_LINUX_GAME_UNITS}" "${GT4_DONOR}/runtime/include")
        target_compile_options(gt4_game_units PRIVATE -Wno-unused-label -Wno-unused-variable -fPIE)
        set(gt4_linux_library gt4_game_units)
    endif()

    # The host functions the release units call (camera_360_hook.cpp, fov_adjust_hook.cpp, display_aspect_hook.cpp: they
    # read guest registers and RAM, so they belong to the game executable, as in gt4vulkan on Windows). Linked with every
    # game executable; with the baseline library nothing calls them.
    add_library(gt4_patch_hooks STATIC "${GT4_NATIVE}/camera_360_hook.cpp" "${GT4_NATIVE}/fov_adjust_hook.cpp"
        "${GT4_NATIVE}/display_aspect_hook.cpp")
    target_link_libraries(gt4_patch_hooks PUBLIC gt4_ps2runtime_vulkan gt4_quickfix)
    target_compile_options(gt4_patch_hooks PRIVATE ${GT4_WARNINGS})

    if(gt4_linux_aot)
        target_include_directories(gt4_ps2runtime_vulkan PUBLIC "${CMAKE_SOURCE_DIR}/tools/vu_recomp/src")
        target_compile_definitions(gt4_ps2runtime_vulkan PUBLIC GT4_PERF_PROBE)
        target_link_libraries(gt4_ps2runtime_vulkan PUBLIC gt4_mtvu_runtime)
        if(gt4_capsule_gs_stream)
            # TASK-074 CoreGS: the GS stream seam with GT4_RENDERER=hw taken in process by CoreGS (native/hw_gs). The
            # GS process transport compiles but refuses on Linux (gs_stream_ipc.cpp); the port must come from
            # prepare_external_vulkan.py --gs-stream-hooks --linux-surfaces.
            file(STRINGS "${GT4_LINUX_PORT_DIR}/gs.cpp" gt4_port_stream_hook REGEX "attach_linux_window")
            if(NOT gt4_port_stream_hook)
                message(FATAL_ERROR "The --gs-stream capsule needs a port derived with --gs-stream-hooks --linux-surfaces")
            endif()
            gt4_host_library(gt4_gs_stream gs_stream/gs_stream_ipc.cpp gs_stream/gs_stream_file.cpp
                gs_stream/gs_readback_probe.cpp)
            target_include_directories(gt4_gs_stream PUBLIC "${GT4_NATIVE}/gs_stream")
            add_library(gt4_hw_stream_sink STATIC "${GT4_HW}/hw_gs_stream_sink.cpp" "${GT4_HW}/hw_gs_prepare.cpp"
                "${GT4_HW}/hw_cpu_sampler_linux.cpp")
            target_include_directories(gt4_hw_stream_sink PRIVATE "${GT4_NATIVE}")
            target_link_libraries(gt4_hw_stream_sink PUBLIC gt4_hw_backend gt4_quickfix)
            gt4_hw_compile(gt4_hw_stream_sink)
            gt4_host_library(gt4_gs_stream_client gs_stream/gs_stream_client.cpp)
            target_link_libraries(gt4_gs_stream_client PUBLIC gt4_gs_stream gt4_perf_probe gt4_install_paths gt4_quickfix
                gt4_hw_stream_sink)
            target_compile_definitions(gt4_gs_stream_client PRIVATE GT4_GS_STREAM_HW INTERFACE GT4_GS_STREAM_CLIENT)
            target_link_libraries(gt4_ps2runtime_vulkan PUBLIC gt4_gs_stream_client)
        endif()

        # The AOT controls of tools/vu_recomp (registered as in the root CMakeLists.txt): the emitter's fixture corpus
        # against the donor interpreter, and the release VU1/VU0 tables in the linked runtime against the interpreter.
        set(gt4_vu_fixture_root "${CMAKE_BINARY_DIR}/vu_recomp")
        add_executable(gt4_vu_fixture_writer "${gt4_vu_tests}/fixture_writer.cpp")
        target_link_libraries(gt4_vu_fixture_writer PRIVATE gt4_vu_fixtures)
        add_custom_command(
            OUTPUT "${gt4_vu_fixture_root}/generated/vu1_generated.cpp" "${gt4_vu_fixture_root}/generated/manifest.json"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${gt4_vu_fixture_root}/corpus" "${gt4_vu_fixture_root}/generated"
            COMMAND $<TARGET_FILE:gt4_vu_fixture_writer> "${gt4_vu_fixture_root}/corpus"
            COMMAND $<TARGET_FILE:gt4_vu_recomp> "${gt4_vu_fixture_root}/corpus" "${gt4_vu_fixture_root}/generated"
            DEPENDS gt4_vu_fixture_writer gt4_vu_recomp VERBATIM)
        add_executable(gt4_vu_differential_tests "${gt4_vu_tests}/differential_tests.cpp" "${gt4_vu_tests}/gif_capture.cpp"
            "${gt4_vu_fixture_root}/generated/vu1_generated.cpp")
        set_source_files_properties("${gt4_vu_fixture_root}/generated/vu1_generated.cpp" PROPERTIES
            COMPILE_OPTIONS "-Wno-unused-label")
        add_executable(gt4_vu1_aot_registry_test "${gt4_vu_tests}/vu1_aot_registry_test.cpp" "${gt4_vu_tests}/gif_capture.cpp")
        add_executable(gt4_vu0_differential_tests "${gt4_vu_tests}/vu0_differential_tests.cpp" "${gt4_vu_tests}/gif_capture.cpp")
        add_executable(gt4_vu0_aot_registry_test "${gt4_vu_tests}/vu0_aot_registry_test.cpp" "${gt4_vu_tests}/gif_capture.cpp")
        foreach(vu_test IN ITEMS gt4_vu_differential_tests gt4_vu1_aot_registry_test gt4_vu0_differential_tests
                                 gt4_vu0_aot_registry_test)
            target_include_directories(${vu_test} PRIVATE "${gt4_vu_src}" "${gt4_vu_tests}")
            target_link_libraries(${vu_test} PRIVATE gt4_vu_fixtures gt4_ps2runtime_vulkan)
        endforeach()
        set(GT4_LINUX_VU0_IMAGE "" CACHE FILEPATH "Pinned VU0 image (scripts/vu0_image.py build) for the VU0 AOT controls")
        add_test(NAME vu_recomp_differential COMMAND $<TARGET_FILE:gt4_vu_differential_tests>)
        add_test(NAME vu1_aot_registry COMMAND $<TARGET_FILE:gt4_vu1_aot_registry_test>)
        set_tests_properties(vu_recomp_differential vu1_aot_registry PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" ENVIRONMENT "GT4_VU_CORPUS_DIR=")
        if(GT4_LINUX_VU0_IMAGE)
            add_test(NAME vu0_aot_registry COMMAND $<TARGET_FILE:gt4_vu0_aot_registry_test> "${GT4_LINUX_VU0_IMAGE}")
            add_test(NAME vu0_aot_differential COMMAND $<TARGET_FILE:gt4_vu0_differential_tests> "${GT4_LINUX_VU0_IMAGE}")
            set_tests_properties(vu0_aot_registry vu0_aot_differential PROPERTIES RUN_SERIAL TRUE TIMEOUT 3600
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        endif()
    endif()
    target_include_directories(gt4_ps2runtime_vulkan PUBLIC "${GT4_DONOR}/runtime/include" "${GT4_DONOR}/runtime/src"
        "${GT4_DONOR}/runtime/src/gs" "${GT4_NATIVE}" "${GT4_LINUX_PORT_DIR}")
    target_compile_definitions(gt4_ps2runtime_vulkan PUBLIC PS2_GPU GT4_VULKAN_SHADER_DIR="${GT4_LINUX_SHADER_DIR}")
    target_link_libraries(gt4_ps2runtime_vulkan PUBLIC gt4_pacing gt4_quickfix gt4_install_paths gt4_vulkan_extensions
        gt4_usb gt4_perf_probe Threads::Threads ${CMAKE_DL_LIBS})

    add_executable(vulkan_fixture "${GT4_NATIVE}/fixture.cpp")
    target_link_libraries(vulkan_fixture PRIVATE gt4_ps2runtime_vulkan)
    # Same checks as the Windows native_vulkan_pixels entries, minus presentation (headless until L2). The default
    # entry runs on the loader's choice of GPU; the lavapipe entry forces Mesa's software driver when it is installed.
    add_test(NAME native_vulkan_pixels COMMAND $<TARGET_FILE:vulkan_fixture>)
    add_test(NAME native_vulkan_pixels_sync COMMAND $<TARGET_FILE:vulkan_fixture>)
    set_tests_properties(native_vulkan_pixels_sync PROPERTIES ENVIRONMENT "GT4_GS_SYNC=1")
    set(gt4_tests_vulkan native_vulkan_pixels native_vulkan_pixels_sync)
    find_file(GT4_LAVAPIPE_ICD lvp_icd.x86_64.json PATHS /usr/share/vulkan/icd.d /etc/vulkan/icd.d NO_DEFAULT_PATH)
    if(GT4_LAVAPIPE_ICD)
        add_test(NAME native_vulkan_pixels_lavapipe COMMAND $<TARGET_FILE:vulkan_fixture>)
        set_tests_properties(native_vulkan_pixels_lavapipe PROPERTIES ENVIRONMENT "VK_DRIVER_FILES=${GT4_LAVAPIPE_ICD}")
        list(APPEND gt4_tests_vulkan native_vulkan_pixels_lavapipe)
    endif()
    set_tests_properties(${gt4_tests_vulkan} PROPERTIES RUN_SERIAL TRUE TIMEOUT 300
        PASS_REGULAR_EXPRESSION "PASS: real Vulkan" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    if(GT4_LINUX_PRODUCER)
        # TASK-074 L2: the game in an SDL3 window (native/linux/sdl_window.cpp). It needs a port derived with
        # scripts/prepare_external_vulkan.py --linux-surfaces, whose attach_surface takes X11 and Wayland handles.
        file(STRINGS "${GT4_LINUX_PORT_DIR}/gpu.cpp" gt4_linux_surface_attach REGEX "g_vulkan\\.attach_x11")
        if(NOT SDL3_FOUND)
            message(STATUS "gt4_linux_window not built: SDL3 (CMake package) not found")
        elseif(NOT gt4_linux_surface_attach)
            message(STATUS "gt4_linux_window not built: derive GT4_LINUX_PORT_DIR with --linux-surfaces")
        else()
            # The port's main.cpp runs as gt4_donor_main behind native/linux/linux_main.cpp (as game_main.cpp on Windows),
            # with the game patches' host functions the release units call.
            # The port's main.cpp is compiled once more on its own, renamed (source properties are per directory and
            # the headless runners keep it as their main).
            add_library(gt4_port_donor_main OBJECT "${GT4_LINUX_PORT_DIR}/main.cpp")
            target_compile_definitions(gt4_port_donor_main PRIVATE PS2_WINDOW main=gt4_donor_main)
            target_link_libraries(gt4_port_donor_main PRIVATE gt4_ps2runtime_vulkan)
            add_executable(gt4_linux_window $<TARGET_OBJECTS:gt4_port_donor_main> "${GT4_NATIVE}/linux/linux_main.cpp"
                "${GT4_NATIVE}/linux/sdl_window.cpp" "${GT4_NATIVE}/linux/sdl_audio.cpp" "${GT4_NATIVE}/sdl_gamepad.cpp"
                "${GT4_NATIVE}/frame_clock.cpp")
            target_compile_definitions(gt4_linux_window PRIVATE PS2_WINDOW)
            set_source_files_properties("${GT4_LINUX_PORT_DIR}/main.cpp" PROPERTIES
                COMPILE_OPTIONS "-include;${GT4_NATIVE}/host_compat.hpp")
            target_compile_options(gt4_linux_window PRIVATE ${GT4_WARNINGS})
            target_link_libraries(gt4_linux_window PRIVATE gt4_controls gt4_pad_mapping SDL3::SDL3
                "$<LINK_GROUP:RESCAN,${gt4_linux_library},gt4_patch_hooks,gt4_ps2runtime_vulkan>")
        endif()

        # The port's main.cpp is the donor host with PS2_GS=gpu forced (no silent CPU raster fallback); its
        # _putenv_s call comes from native/host_compat.hpp.
        add_executable(gt4_vulkan_headless_smoke "${GT4_LINUX_PORT_DIR}/main.cpp")
        target_compile_options(gt4_vulkan_headless_smoke PRIVATE -include "${GT4_NATIVE}/host_compat.hpp")
        target_link_libraries(gt4_vulkan_headless_smoke PRIVATE
            "$<LINK_GROUP:RESCAN,${gt4_linux_library},gt4_patch_hooks,gt4_ps2runtime_vulkan>")
    endif()
endif()
