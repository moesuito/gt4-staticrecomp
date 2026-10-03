cmake_minimum_required(VERSION 3.24)
project(GT4Recomp VERSION 0.1.0 LANGUAGES CXX)

# Compression is a utility dependency. Parsing and image construction are ours.
include(FetchContent)
set(GT4_ZLIB_ARCHIVE "https://zlib.net/fossils/zlib-1.3.1.tar.gz" CACHE STRING
    "Pinned zlib archive URL or local archive path for offline builds")
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    URL "${GT4_ZLIB_ARCHIVE}"
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(zlib)

add_library(gt4recomp_core STATIC src/common/project.cpp)
target_include_directories(gt4recomp_core PUBLIC include)
target_compile_features(gt4recomp_core PUBLIC cxx_std_20)
set_target_properties(gt4recomp_core PROPERTIES CXX_EXTENSIONS OFF)

add_library(gt4recomp_decode STATIC
    src/ee/decode.cpp
    src/ee/disassemble.cpp
    src/ee/flow.cpp
    src/ee/functions.cpp
    src/ee/state.cpp
    src/ee/checkpoint.cpp
    src/ee/interpreter.cpp
    src/ee/driver.cpp
    src/ee/services.cpp
    src/ee/kernel.cpp
    src/ee/timer.cpp
    src/ee/device.cpp
    src/ee/vu_macro.cpp
)
target_include_directories(gt4recomp_decode PUBLIC include)
target_compile_features(gt4recomp_decode PUBLIC cxx_std_20)
set_target_properties(gt4recomp_decode PROPERTIES CXX_EXTENSIONS OFF)

add_library(gt4recomp_executable STATIC
    src/executable/core_image.cpp
    src/executable/analysis_elf.cpp
    src/executable/disc_image.cpp
    src/executable/gt4_volume.cpp
)
target_include_directories(gt4recomp_executable PUBLIC include)
target_include_directories(gt4recomp_executable PRIVATE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
target_compile_features(gt4recomp_executable PUBLIC cxx_std_20)
set_target_properties(gt4recomp_executable PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(gt4recomp_executable PRIVATE zlibstatic)

if(WIN32)
    add_library(gt4recomp_input STATIC tools/common/verified_core.cpp)
    target_include_directories(gt4recomp_input PUBLIC tools/common)
    target_compile_features(gt4recomp_input PUBLIC cxx_std_20)
    set_target_properties(gt4recomp_input PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_definitions(gt4recomp_input PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(gt4recomp_input PRIVATE bcrypt)
    add_executable(gt4core tools/gt4core/main.cpp)
    target_link_libraries(gt4core PRIVATE gt4recomp_executable gt4recomp_input)
    target_compile_definitions(gt4core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_executable(gt4disasm tools/gt4disasm/main.cpp)
    target_link_libraries(gt4disasm PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
    add_executable(gt4blocks tools/gt4blocks/main.cpp)
    target_link_libraries(gt4blocks PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
    add_executable(gt4cfg tools/gt4cfg/main.cpp)
    target_link_libraries(gt4cfg PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
    add_executable(gt4funcs tools/gt4funcs/main.cpp)
    target_link_libraries(gt4funcs PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
    add_executable(gt4translate tools/gt4translate/main.cpp)
    target_link_libraries(gt4translate PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
endif()

add_executable(gt4recomp tools/gt4recomp/main.cpp)
target_link_libraries(gt4recomp PRIVATE gt4recomp_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(ee_disassemble_tests tests/unit/ee_disassemble_test.cpp)
    target_link_libraries(ee_disassemble_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_disassemble COMMAND ee_disassemble_tests)
    add_executable(ee_flow_tests tests/unit/ee_flow_test.cpp)
    target_link_libraries(ee_flow_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_flow COMMAND ee_flow_tests)
    add_executable(ee_cfg_tests tests/unit/ee_cfg_test.cpp)
    target_link_libraries(ee_cfg_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_cfg COMMAND ee_cfg_tests)
    add_executable(ee_functions_tests tests/unit/ee_functions_test.cpp)
    target_link_libraries(ee_functions_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_functions COMMAND ee_functions_tests)
    add_executable(ee_state_tests tests/unit/ee_state_test.cpp)
    target_link_libraries(ee_state_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_state COMMAND ee_state_tests)
    add_executable(ee_checkpoint_tests tests/unit/ee_checkpoint_test.cpp)
    target_link_libraries(ee_checkpoint_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_checkpoint COMMAND ee_checkpoint_tests)
    add_executable(ee_device_tests tests/unit/ee_device_test.cpp)
    target_link_libraries(ee_device_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_device COMMAND ee_device_tests)
    add_executable(ee_interpreter_tests tests/unit/ee_interpreter_test.cpp)
    target_link_libraries(ee_interpreter_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_interpreter COMMAND ee_interpreter_tests)
    add_executable(ee_driver_tests tests/unit/ee_driver_test.cpp)
    target_link_libraries(ee_driver_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_driver COMMAND ee_driver_tests)
    add_executable(ee_kernel_tests tests/unit/ee_kernel_test.cpp)
    target_link_libraries(ee_kernel_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_kernel COMMAND ee_kernel_tests)
    add_executable(ee_timer_tests tests/unit/ee_timer_test.cpp)
    target_link_libraries(ee_timer_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_timer COMMAND ee_timer_tests)
    add_executable(ee_synth_tests tests/unit/ee_synth_test.cpp)
    target_link_libraries(ee_synth_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_synth COMMAND ee_synth_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synth-straight.txt")
    add_test(NAME ee_synth_branching COMMAND ee_synth_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synth-branching.txt")
    add_executable(ee_decode_tests tests/unit/ee_decode_test.cpp)
    target_link_libraries(ee_decode_tests PRIVATE gt4recomp_decode)
    add_test(NAME ee_decode COMMAND ee_decode_tests)
    add_executable(gt4recomp_tests tests/unit/project_test.cpp)
    target_link_libraries(gt4recomp_tests PRIVATE gt4recomp_core)
    add_test(NAME project_identity COMMAND gt4recomp_tests)
    add_test(NAME cli_smoke COMMAND gt4recomp)
    set_tests_properties(cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "GT4Recomp: learning laboratory")
    add_executable(executable_image_tests tests/unit/executable_image_test.cpp)
    target_link_libraries(executable_image_tests PRIVATE gt4recomp_executable zlibstatic)
    target_include_directories(executable_image_tests PRIVATE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    add_test(NAME executable_image COMMAND executable_image_tests)

    # The disc image readers: always the synthetic image, plus the pinned ISO
    # when it is present locally.
    add_executable(disc_image_tests tests/unit/disc_image_test.cpp)
    target_link_libraries(disc_image_tests PRIVATE gt4recomp_executable)
    set(GT4_LOCAL_ISO "${CMAKE_CURRENT_SOURCE_DIR}/Gran Turismo 4 (USA) (v2.00).iso")
    if(EXISTS "${GT4_LOCAL_ISO}")
        add_test(NAME disc_image COMMAND disc_image_tests "${GT4_LOCAL_ISO}")
    else()
        add_test(NAME disc_image COMMAND disc_image_tests)
    endif()

    # The GT4.VOL archive reader: always the synthetic volume, plus the
    # pinned archive through the ISO when both are present.
    add_executable(gt4_volume_tests tests/unit/gt4_volume_test.cpp)
    target_link_libraries(gt4_volume_tests PRIVATE gt4recomp_executable)
    if(EXISTS "${GT4_LOCAL_ISO}")
        add_test(NAME gt4_volume COMMAND gt4_volume_tests "${GT4_LOCAL_ISO}")
    else()
        add_test(NAME gt4_volume COMMAND gt4_volume_tests)
    endif()

    # The translation test needs the local CORE: the translated header is
    # generated into the ignored build tree and never committed. On machines
    # without the CORE the test simply does not exist.
    set(GT4_LOCAL_CORE "${CMAKE_CURRENT_SOURCE_DIR}/private/fingerprint-check/CORE.GT4")
    if(WIN32 AND EXISTS "${GT4_LOCAL_CORE}")
        set(GT4_TRANSLATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-00577878.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x577878 64 "${GT4_TRANSLATED_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_tests
            tests/unit/ee_translation_test.cpp
            "${GT4_TRANSLATED_HEADER}")
        target_include_directories(ee_translation_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation COMMAND ee_translation_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_BRANCH_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-005c11a8.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_BRANCH_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x5c11a8 300 "${GT4_TRANSLATED_BRANCH_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_branch_tests
            tests/unit/ee_translation_branch_test.cpp
            "${GT4_TRANSLATED_BRANCH_HEADER}")
        target_include_directories(ee_translation_branch_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_branch_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_branch COMMAND ee_translation_branch_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_CALLS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-0010c0c0.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_CALLS_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x10c0c0 2000 "${GT4_TRANSLATED_CALLS_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_calls_tests
            tests/unit/ee_translation_calls_test.cpp
            "${GT4_TRANSLATED_CALLS_HEADER}")
        target_include_directories(ee_translation_calls_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_calls_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_calls COMMAND ee_translation_calls_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_549378_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-00549378.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_549378_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x549378 64 "${GT4_TRANSLATED_549378_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_549378_tests
            tests/unit/ee_translation_549378_test.cpp
            "${GT4_TRANSLATED_549378_HEADER}")
        target_include_directories(ee_translation_549378_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_549378_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_549378 COMMAND ee_translation_549378_tests "${GT4_LOCAL_CORE}")

        add_executable(ee_startup_run_tests
            tests/unit/ee_startup_run_test.cpp)
        target_link_libraries(ee_startup_run_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_startup_run COMMAND ee_startup_run_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_STARTUP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-startup.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_STARTUP_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x100008 200 "${GT4_TRANSLATED_STARTUP_HEADER}" 0x1001C8
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_startup_tests
            tests/unit/ee_translation_startup_test.cpp
            "${GT4_TRANSLATED_STARTUP_HEADER}")
        target_include_directories(ee_translation_startup_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_startup_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_startup COMMAND ee_translation_startup_tests "${GT4_LOCAL_CORE}")

        # The driver as a program: the same startup module, run and checked
        # against the interpreter from the command line.
        add_executable(gt4run tools/gt4run/main.cpp "${GT4_TRANSLATED_STARTUP_HEADER}")
        target_include_directories(gt4run PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(gt4run PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME gt4run_startup COMMAND gt4run "${GT4_LOCAL_CORE}" --compare-interpreter)
        set_tests_properties(gt4run_startup PROPERTIES
            PASS_REGULAR_EXPRESSION "boundary: syscall 0x001001c8 service 0x3c")

        # Regression for the "jr ra" emission: DIntr returns in the middle of
        # its extent, followed by the taken-path block.
        set(GT4_TRANSLATED_5B72A8_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-005b72a8.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_5B72A8_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x5b72a8 64 "${GT4_TRANSLATED_5B72A8_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_5b72a8_tests
            tests/unit/ee_translation_5b72a8_test.cpp
            "${GT4_TRANSLATED_5B72A8_HEADER}")
        target_include_directories(ee_translation_5b72a8_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_5b72a8_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_5b72a8 COMMAND ee_translation_5b72a8_tests "${GT4_LOCAL_CORE}")

        # The whole game as one module under the driver. Generating it takes
        # minutes and the object is ~100 MB, so gt4boot is excluded from the
        # default build; the gt4boot_build fixture builds it on demand the
        # first time the gt4boot_services test runs.
        set(GT4_WHOLE_PROGRAM_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-whole-program.hpp")
        add_custom_command(
            OUTPUT "${GT4_WHOLE_PROGRAM_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" --functions 20000 --all 2000000
                    "${GT4_WHOLE_PROGRAM_HEADER}"
            DEPENDS gt4translate
            COMMENT "Translating the whole program (15k functions, ~150 MB)"
            VERBATIM)
        add_executable(gt4boot tools/gt4boot/main.cpp "${GT4_WHOLE_PROGRAM_HEADER}")
        target_include_directories(gt4boot PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(gt4boot PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        target_compile_options(gt4boot PRIVATE /bigobj)
        set_target_properties(gt4boot PROPERTIES EXCLUDE_FROM_ALL TRUE)
        add_test(NAME gt4boot_build
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target gt4boot)
        set_tests_properties(gt4boot_build PROPERTIES FIXTURES_SETUP gt4boot)
        # The pinned boot frontier. With the disc image present it advances
        # past the sound phase (the block cache fix of slice 42); without
        # one the boot cannot pass the disc phase, so the old frontier stays.
        if(EXISTS "${GT4_LOCAL_ISO}")
            add_test(NAME gt4boot_services
                COMMAND gt4boot "${GT4_LOCAL_CORE}" --disc "${GT4_LOCAL_ISO}"
                        --services 90000 --compare-interpreter)
            set_tests_properties(gt4boot_services PROPERTIES
                FIXTURES_REQUIRED gt4boot
                PASS_REGULAR_EXPRESSION "services handled 90000")
        else()
            add_test(NAME gt4boot_services
                COMMAND gt4boot "${GT4_LOCAL_CORE}" --services 3000 --compare-interpreter)
            set_tests_properties(gt4boot_services PROPERTIES
                FIXTURES_REQUIRED gt4boot
                PASS_REGULAR_EXPRESSION "services handled 3000")
        endif()
        # The stop-time memory view: a short run prints the requested guest
        # words, so the --dump option stays exercised.
        add_test(NAME gt4boot_dump
            COMMAND gt4boot "${GT4_LOCAL_CORE}" --services 400 --dump 0x100000 0x20)
        set_tests_properties(gt4boot_dump PROPERTIES
            FIXTURES_REQUIRED gt4boot
            PASS_REGULAR_EXPRESSION "dump 0x00100000 \\(32 bytes\\):")
        # The step budget flag: a tiny budget stops at the step limit right
        # away, so --steps stays exercised without a disc or a long run.
        add_test(NAME gt4boot_steps
            COMMAND gt4boot "${GT4_LOCAL_CORE}" --steps 1000)
        set_tests_properties(gt4boot_steps PROPERTIES
            FIXTURES_REQUIRED gt4boot
            PASS_REGULAR_EXPRESSION "boundary: step-limit")
        # Checkpoint and resume: a tiny run saves at 400 services, then a
        # second invocation resumes it for 400 more and requires the same
        # stop and state as an uninterrupted 800-service run. The file
        # holds guest RAM, so it lives in the (ignored) build directory.
        set(GT4_CHECKPOINT_FILE "${CMAKE_CURRENT_BINARY_DIR}/ckpt-test.bin")
        add_test(NAME gt4boot_checkpoint
            COMMAND gt4boot "${GT4_LOCAL_CORE}" --services 400
                    --checkpoint-at 400 "${GT4_CHECKPOINT_FILE}")
        set_tests_properties(gt4boot_checkpoint PROPERTIES
            FIXTURES_REQUIRED gt4boot
            FIXTURES_SETUP gt4boot_checkpoint
            PASS_REGULAR_EXPRESSION "checkpoint saved at 400 services")
        add_test(NAME gt4boot_resume_verify
            COMMAND gt4boot "${GT4_LOCAL_CORE}"
                    --verify-resume "${GT4_CHECKPOINT_FILE}" --services 400)
        set_tests_properties(gt4boot_resume_verify PROPERTIES
            FIXTURES_REQUIRED "gt4boot;gt4boot_checkpoint"
            PASS_REGULAR_EXPRESSION "resume states identical")

        set(GT4_TRANSLATED_572438_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-00572438.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_572438_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x572438 64 "${GT4_TRANSLATED_572438_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_572438_tests
            tests/unit/ee_translation_572438_test.cpp
            "${GT4_TRANSLATED_572438_HEADER}")
        target_include_directories(ee_translation_572438_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_572438_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_572438 COMMAND ee_translation_572438_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_THUNK_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-005b27f8.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_THUNK_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x5b27f8 64 "${GT4_TRANSLATED_THUNK_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_thunk_tests
            tests/unit/ee_translation_thunk_test.cpp
            "${GT4_TRANSLATED_THUNK_HEADER}")
        target_include_directories(ee_translation_thunk_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_thunk_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_thunk COMMAND ee_translation_thunk_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_CACHEFLUSH_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-005b0f78.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_CACHEFLUSH_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x5b0f78 128 "${GT4_TRANSLATED_CACHEFLUSH_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_cacheflush_tests
            tests/unit/ee_translation_cacheflush_test.cpp
            "${GT4_TRANSLATED_CACHEFLUSH_HEADER}")
        target_include_directories(ee_translation_cacheflush_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_cacheflush_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_cacheflush COMMAND ee_translation_cacheflush_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_579780_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-00579780.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_579780_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x579780 256 "${GT4_TRANSLATED_579780_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_579780_tests
            tests/unit/ee_translation_579780_test.cpp
            "${GT4_TRANSLATED_579780_HEADER}")
        target_include_directories(ee_translation_579780_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_579780_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_579780 COMMAND ee_translation_579780_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_58CE48_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-0058ce48.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_58CE48_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x58ce48 6000 "${GT4_TRANSLATED_58CE48_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_58ce48_tests
            tests/unit/ee_translation_58ce48_test.cpp
            "${GT4_TRANSLATED_58CE48_HEADER}")
        target_include_directories(ee_translation_58ce48_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_58ce48_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_58ce48 COMMAND ee_translation_58ce48_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_56DF58_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-0056df58.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_56DF58_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x56df58 4000 "${GT4_TRANSLATED_56DF58_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_56df58_tests
            tests/unit/ee_translation_56df58_test.cpp
            "${GT4_TRANSLATED_56DF58_HEADER}")
        target_include_directories(ee_translation_56df58_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_56df58_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_56df58 COMMAND ee_translation_56df58_tests "${GT4_LOCAL_CORE}")

        set(GT4_TRANSLATED_101C28_HEADER "${CMAKE_CURRENT_BINARY_DIR}/translated-00101c28.hpp")
        add_custom_command(
            OUTPUT "${GT4_TRANSLATED_101C28_HEADER}"
            COMMAND gt4translate "${GT4_LOCAL_CORE}" 0x101c28 256 "${GT4_TRANSLATED_101C28_HEADER}"
            DEPENDS gt4translate
            VERBATIM)
        add_executable(ee_translation_101c28_tests
            tests/unit/ee_translation_101c28_test.cpp
            "${GT4_TRANSLATED_101C28_HEADER}")
        target_include_directories(ee_translation_101c28_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(ee_translation_101c28_tests PRIVATE gt4recomp_decode gt4recomp_executable gt4recomp_input)
        add_test(NAME ee_translation_101c28 COMMAND ee_translation_101c28_tests "${GT4_LOCAL_CORE}")
    endif()
endif()
