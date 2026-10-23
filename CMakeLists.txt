cmake_minimum_required(VERSION 3.20)
project(MetalStream C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build; an empty CMAKE_BUILD_TYPE means -O0.
# deploy.sh strips the DLLs and keeps the debug info in build/*.dll.debug.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_SHARED_LIBRARY_PREFIX "")

# Static link runtime to eliminate MinGW DLL dependencies
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Target 1: sl.interposer.dll
# ---------------------------------------------------------------------------
add_library(sl.interposer SHARED
    src/common/logger.cpp
    src/common/config.cpp
    src/interposer/dllmain.cpp
    src/interposer/dxgi_proxy.cpp
    src/interposer/d3d12_proxy.cpp
    src/interposer/framegen.cpp
    src/interposer/nvngx_router.cpp
    src/interposer/sl_interposer.cpp
)

target_compile_definitions(sl.interposer PRIVATE
    SL_INTERPOSER=1
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(sl.interposer PRIVATE -Wno-deprecated-declarations)

target_link_libraries(sl.interposer PRIVATE
    kernel32
    user32
)

set_target_properties(sl.interposer PROPERTIES
    OUTPUT_NAME "sl.interposer"
    PREFIX ""
    SUFFIX ".dll"
)

# ---------------------------------------------------------------------------
# Target 2: sl.dlss.dll
# ---------------------------------------------------------------------------
add_library(sl.dlss SHARED
    src/common/logger.cpp
    src/common/config.cpp
    src/dlss/sl_dlss.cpp
)

target_compile_definitions(sl.dlss PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(sl.dlss PRIVATE -Wno-deprecated-declarations)

target_link_libraries(sl.dlss PRIVATE
    kernel32
    user32
)

set_target_properties(sl.dlss PROPERTIES
    OUTPUT_NAME "sl.dlss"
    PREFIX ""
    SUFFIX ".dll"
)

# ---------------------------------------------------------------------------
# Test: test_streamline.exe
# ---------------------------------------------------------------------------
add_executable(test_streamline
    tests/test_streamline.cpp
)
target_compile_definitions(test_streamline PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_streamline PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_streamline PRIVATE
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_streamline_e2e_eval.exe
# ---------------------------------------------------------------------------
add_executable(test_streamline_e2e_eval
    tests/test_streamline_e2e_eval.cpp
)
target_compile_definitions(test_streamline_e2e_eval PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_streamline_e2e_eval PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_streamline_e2e_eval PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_phase1_stability.exe
# ---------------------------------------------------------------------------
add_executable(test_phase1_stability
    tests/test_phase1_stability.cpp
)
target_compile_definitions(test_phase1_stability PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_phase1_stability PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_phase1_stability PRIVATE
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_phase2_defense.exe
# ---------------------------------------------------------------------------
add_executable(test_phase2_defense
    tests/test_phase2_defense.cpp
)
target_compile_definitions(test_phase2_defense PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_phase2_defense PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_phase2_defense PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_phase3_drs.exe
# ---------------------------------------------------------------------------
add_executable(test_phase3_drs
    tests/test_phase3_drs.cpp
)
target_compile_definitions(test_phase3_drs PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_phase3_drs PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_phase3_drs PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_phase4_viewport.exe
# ---------------------------------------------------------------------------
add_executable(test_phase4_viewport
    tests/test_phase4_viewport.cpp
)
target_compile_definitions(test_phase4_viewport PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_phase4_viewport PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_phase4_viewport PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_phase5_barriers_bus.exe
# ---------------------------------------------------------------------------
add_executable(test_phase5_barriers_bus
    tests/test_phase5_barriers_bus.cpp
)
target_compile_definitions(test_phase5_barriers_bus PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_phase5_barriers_bus PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_phase5_barriers_bus PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_part1_audit.exe
# ---------------------------------------------------------------------------
add_executable(test_part1_audit
    tests/test_part1_audit.cpp
)
target_compile_definitions(test_part1_audit PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_part1_audit PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_part1_audit PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_part2_contracts.exe
# ---------------------------------------------------------------------------
add_executable(test_part2_contracts
    tests/test_part2_contracts.cpp
)
target_compile_definitions(test_part2_contracts PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_part2_contracts PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_part2_contracts PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_part3_viewport_lifecycle.exe
# ---------------------------------------------------------------------------
add_executable(test_part3_viewport_lifecycle
    tests/test_part3_viewport_lifecycle.cpp
)
target_compile_definitions(test_part3_viewport_lifecycle PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_part3_viewport_lifecycle PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_part3_viewport_lifecycle PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_part4_concurrency.exe
# ---------------------------------------------------------------------------
add_executable(test_part4_concurrency
    tests/test_part4_concurrency.cpp
)
target_compile_definitions(test_part4_concurrency PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_part4_concurrency PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_part4_concurrency PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_part5_reflex_dlssg.exe
# ---------------------------------------------------------------------------
add_executable(test_part5_reflex_dlssg
    tests/test_part5_reflex_dlssg.cpp
)
target_compile_definitions(test_part5_reflex_dlssg PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_part5_reflex_dlssg PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_part5_reflex_dlssg PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit2_batch1_float_logging.exe
# ---------------------------------------------------------------------------
add_executable(test_audit2_batch1_float_logging
    tests/test_audit2_batch1_float_logging.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit2_batch1_float_logging PRIVATE
    include
    src
)
target_compile_definitions(test_audit2_batch1_float_logging PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit2_batch1_float_logging PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit2_batch1_float_logging PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit2_batch2_perf_quality.exe
# ---------------------------------------------------------------------------
add_executable(test_audit2_batch2_perf_quality
    tests/test_audit2_batch2_perf_quality.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit2_batch2_perf_quality PRIVATE
    include
    src
)
target_compile_definitions(test_audit2_batch2_perf_quality PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit2_batch2_perf_quality PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit2_batch2_perf_quality PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit2_batch3_render_pipeline.exe
# ---------------------------------------------------------------------------
add_executable(test_audit2_batch3_render_pipeline
    tests/test_audit2_batch3_render_pipeline.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit2_batch3_render_pipeline PRIVATE
    include
    src
)
target_compile_definitions(test_audit2_batch3_render_pipeline PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit2_batch3_render_pipeline PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit2_batch3_render_pipeline PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit3_batch1_lifecycle_safety.exe
# ---------------------------------------------------------------------------
add_executable(test_audit3_batch1_lifecycle_safety
    tests/test_audit3_batch1_lifecycle_safety.cpp
)
target_include_directories(test_audit3_batch1_lifecycle_safety PRIVATE
    include
    src
)
target_compile_definitions(test_audit3_batch1_lifecycle_safety PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit3_batch1_lifecycle_safety PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit3_batch1_lifecycle_safety PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit3_batch2_spec_concurrency.exe
# ---------------------------------------------------------------------------
add_executable(test_audit3_batch2_spec_concurrency
    tests/test_audit3_batch2_spec_concurrency.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit3_batch2_spec_concurrency PRIVATE
    include
    src
)
target_compile_definitions(test_audit3_batch2_spec_concurrency PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit3_batch2_spec_concurrency PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit3_batch2_spec_concurrency PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit4_full_spec_and_concurrency.exe
# ---------------------------------------------------------------------------
add_executable(test_audit4_full_spec_and_concurrency
    tests/test_audit4_full_spec_and_concurrency.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit4_full_spec_and_concurrency PRIVATE
    include
    src
)
target_compile_definitions(test_audit4_full_spec_and_concurrency PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit4_full_spec_and_concurrency PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit4_full_spec_and_concurrency PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit5_full_round5_verification.exe
# ---------------------------------------------------------------------------
add_executable(test_audit5_full_round5_verification
    tests/test_audit5_full_round5_verification.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit5_full_round5_verification PRIVATE
    include
    src
)
target_compile_definitions(test_audit5_full_round5_verification PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit5_full_round5_verification PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit5_full_round5_verification PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit6_round6_contract_verification.exe
# ---------------------------------------------------------------------------
add_executable(test_audit6_round6_contract_verification
    tests/test_audit6_round6_contract_verification.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit6_round6_contract_verification PRIVATE
    include
    src
)
target_compile_definitions(test_audit6_round6_contract_verification PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit6_round6_contract_verification PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit6_round6_contract_verification PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Test: test_audit7_round7_contract_verification.exe
# ---------------------------------------------------------------------------
add_executable(test_audit7_round7_contract_verification
    tests/test_audit7_round7_contract_verification.cpp
    src/common/logger.cpp
    src/common/config.cpp
)
target_include_directories(test_audit7_round7_contract_verification PRIVATE
    include
    src
)
target_compile_definitions(test_audit7_round7_contract_verification PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(test_audit7_round7_contract_verification PRIVATE -Wno-deprecated-declarations)
target_link_libraries(test_audit7_round7_contract_verification PRIVATE
    d3d12
    dxgi
    kernel32
    user32
)

# ---------------------------------------------------------------------------
# Behavior test: mock nvngx.dll + fake D3D12 objects (no GPU needed, runs under Wine)
# Everything lands in build/mock/ so the mock never shadows a real nvngx.dll.
# ---------------------------------------------------------------------------
set(METALSTREAM_MOCK_DIR ${CMAKE_BINARY_DIR}/mock)

add_library(mock_nvngx SHARED
    tests/mock/mock_nvngx.cpp
)
target_compile_definitions(mock_nvngx PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mock_nvngx PRIVATE -Wno-deprecated-declarations)
set_target_properties(mock_nvngx PROPERTIES
    OUTPUT_NAME "nvngx"
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY ${METALSTREAM_MOCK_DIR}
)

add_executable(test_mock_pipeline
    tests/test_mock_pipeline.cpp
    tests/mock/fake_d3d12.c
)
target_include_directories(test_mock_pipeline PRIVATE src tests)
target_compile_definitions(test_mock_pipeline PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(test_mock_pipeline PRIVATE -Wno-deprecated-declarations)
set_target_properties(test_mock_pipeline PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${METALSTREAM_MOCK_DIR})
add_dependencies(test_mock_pipeline mock_nvngx sl.interposer)
add_custom_command(TARGET test_mock_pipeline POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sl.interposer> ${METALSTREAM_MOCK_DIR}/
)

# ---------------------------------------------------------------------------
# GPTk real-GPU tests (frame generation). Not run by ctest: they need Wine + GPTk's
# D3DMetal; run them with scripts/run_gptk_tests.sh. Everything lands in build/gptk/,
# which also gets an empty nvngx_dlssg.dll: D3DMetal only enables NGX FrameGeneration
# when a file with that name exists under the executable's directory.
# ---------------------------------------------------------------------------
set(METALSTREAM_GPTK_DIR ${CMAKE_BINARY_DIR}/gptk)

foreach(_gptk_test test_gptk_fg_probe test_gptk_framegen_swapchain test_gptk_framegen_lifecycle)
    add_executable(${_gptk_test} tests/${_gptk_test}.cpp)
    target_compile_definitions(${_gptk_test} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${_gptk_test} PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(${_gptk_test} PRIVATE d3d12 dxgi kernel32 user32)
    set_target_properties(${_gptk_test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${METALSTREAM_GPTK_DIR})
    add_custom_command(TARGET ${_gptk_test} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch ${METALSTREAM_GPTK_DIR}/nvngx_dlssg.dll
    )
endforeach()
add_dependencies(test_gptk_framegen_swapchain sl.interposer)
add_dependencies(test_gptk_framegen_lifecycle sl.interposer)
add_custom_command(TARGET test_gptk_framegen_swapchain POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sl.interposer> ${METALSTREAM_GPTK_DIR}/
)

# Tests rely on assert(); keep it active even in optimized (NDEBUG) builds
get_property(_metalstream_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _metalstream_targets)
    if(_target MATCHES "^test_")
        target_compile_options(${_target} PRIVATE -UNDEBUG)
    endif()
endforeach()

# ctest runs only the mock behavior test; the other test_* executables need a real D3D12 device
set(METALSTREAM_WINE "" CACHE FILEPATH "Wine binary used by ctest to run Windows test executables")
if(METALSTREAM_WINE)
    enable_testing()
    add_test(NAME mock_pipeline
        COMMAND ${METALSTREAM_WINE} $<TARGET_FILE:test_mock_pipeline>
        WORKING_DIRECTORY ${METALSTREAM_MOCK_DIR}
    )
    set_tests_properties(mock_pipeline PROPERTIES
        ENVIRONMENT "WINEPREFIX=${CMAKE_BINARY_DIR}/wineprefix-test;WINEDLLOVERRIDES=nvngx=n;WINEDEBUG=-all"
        TIMEOUT 300
    )
endif()
