cmake_minimum_required(VERSION 3.20)
project(ScarlettX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Core emulator static library
add_library(scarlettx_core STATIC
    src/core/status.cpp
    src/loader/game_config.cpp
    src/loader/pe_loader.cpp
    src/loader/pe_mapper.cpp
    src/loader/stub_thunk.cpp
    src/loader/package_vfs.cpp
    src/loader/xvd_container.cpp
    src/kernel/xmemory.cpp
    src/kernel/xruntime.cpp
    src/kernel/xstorage.cpp
    src/gpu/d3d12_backend.cpp
    src/gpu/rt_bvh.cpp
    src/gpu/shader_ir.cpp
    src/runtime/emulator.cpp
)

target_include_directories(scarlettx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_compile_definitions(scarlettx_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        SCARLETTX_PLATFORM_WINDOWS=1
    )
    if(MSVC)
        target_link_libraries(scarlettx_core PUBLIC
            d3d12
            dxgi
            d3dcompiler
            ole32
            uuid
        )
    else()
        target_link_libraries(scarlettx_core PUBLIC
            d3d12
            dxgi
            d3dcompiler_47
            ole32
            uuid
        )
        add_link_options(-static-libgcc -static-libstdc++)
    endif()
endif()

# ---------------------------------------------------------------------------
# Scarlett HLE System DLL Shims
# ---------------------------------------------------------------------------
add_library(d3d12_xs SHARED src/shims/d3d12_xs_shim.cpp)
set_target_properties(d3d12_xs PROPERTIES PREFIX "" OUTPUT_NAME "d3d12_xs")
target_link_libraries(d3d12_xs PRIVATE scarlettx_core)
target_compile_definitions(d3d12_xs PRIVATE SCARLETTX_BUILDING_D3D12_XS=1)

add_library(xgameruntime SHARED src/shims/xgameruntime_shim.cpp)
set_target_properties(xgameruntime PROPERTIES PREFIX "" OUTPUT_NAME "xgameruntime")
target_link_libraries(xgameruntime PRIVATE scarlettx_core)
target_compile_definitions(xgameruntime PRIVATE SCARLETTX_BUILDING_XGAMERUNTIME=1)

add_library(XboxBvhBuilder SHARED src/shims/xbox_bvh_builder_shim.cpp)
set_target_properties(XboxBvhBuilder PROPERTIES PREFIX "" OUTPUT_NAME "XboxBvhBuilder")
target_link_libraries(XboxBvhBuilder PRIVATE scarlettx_core)
target_compile_definitions(XboxBvhBuilder PRIVATE SCARLETTX_BUILDING_XBOX_BVH=1)

add_library(dstorage SHARED src/shims/dstorage_shim.cpp)
set_target_properties(dstorage PROPERTIES PREFIX "" OUTPUT_NAME "dstorage")
target_link_libraries(dstorage PRIVATE scarlettx_core)
target_compile_definitions(dstorage PRIVATE SCARLETTX_BUILDING_DSTORAGE=1)

add_library(xaudio2_9 SHARED src/shims/xaudio2_shim.cpp)
set_target_properties(xaudio2_9 PROPERTIES PREFIX "" OUTPUT_NAME "xaudio2_9")
target_link_libraries(xaudio2_9 PRIVATE scarlettx_core)
target_compile_definitions(xaudio2_9 PRIVATE SCARLETTX_BUILDING_XAUDIO2=1)

add_library(GameInput SHARED src/shims/gameinput_shim.cpp)
set_target_properties(GameInput PROPERTIES PREFIX "" OUTPUT_NAME "GameInput")
target_link_libraries(GameInput PRIVATE scarlettx_core)
target_compile_definitions(GameInput PRIVATE SCARLETTX_BUILDING_GAMEINPUT=1)

# ---------------------------------------------------------------------------
# ScarlettX CLI Emulator Runner
# ---------------------------------------------------------------------------
add_executable(scarlettx src/main.cpp)
target_link_libraries(scarlettx PRIVATE scarlettx_core)
add_dependencies(scarlettx d3d12_xs xgameruntime XboxBvhBuilder dstorage xaudio2_9 GameInput)

# ---------------------------------------------------------------------------
# Synthetic Scarlett GDK Test Games (compile as real x86-64 PEs linking HLE DLLs)
# ---------------------------------------------------------------------------
add_executable(hello_scarlett tests/synthetic_games/hello_scarlett/main.cpp)
target_link_libraries(hello_scarlett PRIVATE d3d12_xs xgameruntime XboxBvhBuilder)
target_include_directories(hello_scarlett PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(hello_scarlett PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/hello_scarlett"
)
add_custom_command(TARGET hello_scarlett POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/hello_scarlett/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:hello_scarlett>/MicrosoftGame.Config"
)

add_executable(rt_showcase tests/synthetic_games/rt_showcase/main.cpp)
target_link_libraries(rt_showcase PRIVATE d3d12_xs xgameruntime XboxBvhBuilder)
target_include_directories(rt_showcase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(rt_showcase PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/rt_showcase"
)
add_custom_command(TARGET rt_showcase POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/rt_showcase/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:rt_showcase>/MicrosoftGame.Config"
)

# Synthetic Scarlett Game 3: iat_probe (in-process PE64 + ordinal + auto-stub test target)
if(WIN32)
    function(scarlettx_add_dummy_import_lib target_name c_src def_file)
        add_library(${target_name} SHARED ${c_src} ${def_file})
        set_target_properties(${target_name} PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dummy_import_dlls"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dummy_import_libs"
        )
        # Remove the dummy runtime DLL so it cannot be loaded by Windows LoadLibrary;
        # only its import library (.lib/.dll.a) is used at link time.
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E remove -f "$<TARGET_FILE:${target_name}>"
        )
    endfunction()

    scarlettx_add_dummy_import_lib(xordinal_hle
        tests/synthetic_games/iat_probe/xordinal_hle_stub.c
        tests/synthetic_games/iat_probe/xordinal_hle.def
    )
    scarlettx_add_dummy_import_lib(xgameoverlay_missing
        tests/synthetic_games/iat_probe/xgameoverlay_missing_stub.c
        tests/synthetic_games/iat_probe/xgameoverlay_missing.def
    )
    scarlettx_add_dummy_import_lib(xtelemetry_missing
        tests/synthetic_games/iat_probe/xtelemetry_missing_stub.c
        tests/synthetic_games/iat_probe/xtelemetry_missing.def
    )

    add_library(iat_probe SHARED
        tests/synthetic_games/iat_probe/iat_probe.cpp
    )
    target_link_libraries(iat_probe PRIVATE
        xgameruntime
        xordinal_hle
        xgameoverlay_missing
        xtelemetry_missing
    )
    set_target_properties(iat_probe PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/iat_probe"
    )
    add_custom_command(TARGET iat_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/iat_probe/MicrosoftGame.Config"
            "${CMAKE_BINARY_DIR}/packages/iat_probe/MicrosoftGame.Config"
    )
endif()

# ---------------------------------------------------------------------------
# Autonomous Test Suite
# ---------------------------------------------------------------------------
function(scarlettx_add_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE scarlettx_core)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(${name} PRIVATE
        SCARLETTX_BINARY_DIR="${CMAKE_BINARY_DIR}"
        SCARLETTX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

scarlettx_add_test(test_game_config tests/test_game_config.cpp)
scarlettx_add_test(test_pe_loader tests/test_pe_loader.cpp)
add_dependencies(test_pe_loader hello_scarlett rt_showcase)

if(WIN32)
    scarlettx_add_test(test_inproc_loader tests/test_inproc_loader.cpp)
    add_dependencies(test_inproc_loader iat_probe xgameruntime)
endif()

scarlettx_add_test(test_kernel_hle tests/test_kernel_hle.cpp)
scarlettx_add_test(test_xgameruntime_abi tests/test_xgameruntime_abi.cpp)
target_link_libraries(test_xgameruntime_abi PRIVATE xgameruntime)
scarlettx_add_test(test_d3d12_warp tests/test_d3d12_warp.cpp)
scarlettx_add_test(test_d3d12_xs_abi tests/test_d3d12_xs_abi.cpp)
target_link_libraries(test_d3d12_xs_abi PRIVATE d3d12_xs XboxBvhBuilder)
scarlettx_add_test(test_rt_pipeline tests/test_rt_pipeline.cpp)
scarlettx_add_test(test_dstorage_xvd tests/test_dstorage_xvd.cpp)
target_link_libraries(test_dstorage_xvd PRIVATE dstorage)
scarlettx_add_test(test_xaudio2 tests/test_xaudio2.cpp)
target_link_libraries(test_xaudio2 PRIVATE xaudio2_9)
scarlettx_add_test(test_gameinput tests/test_gameinput.cpp)
target_link_libraries(test_gameinput PRIVATE GameInput)

add_test(
    NAME test_boot_hello_scarlett
    COMMAND $<TARGET_FILE:scarlettx> boot "${CMAKE_BINARY_DIR}/packages/hello_scarlett" --headless --frames 2
)
add_test(
    NAME test_boot_rt_showcase
    COMMAND $<TARGET_FILE:scarlettx> boot "${CMAKE_BINARY_DIR}/packages/rt_showcase" --headless --frames 2 --rt-dump "${CMAKE_BINARY_DIR}/rt_showcase_frame.ppm"
)
