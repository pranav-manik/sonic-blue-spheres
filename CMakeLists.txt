cmake_minimum_required(VERSION 3.16)
project(blue_spheres C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Detect Emscripten (WebAssembly) build.
# When building for WASM, invoke cmake via:
#   emcmake cmake -B build_web ..
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(BUILDING_WASM TRUE)
else()
    set(BUILDING_WASM FALSE)
endif()

# ---------------------------------------------------------------------------
# Locate / download the prebuilt sokol-shdc shader compiler.
# We grab the single binary for the HOST platform from sokol-tools-bin.
# For WASM builds we still need the host binary to compile shaders.
# ---------------------------------------------------------------------------
set(SHDC_DIR "${CMAKE_BINARY_DIR}/shdc")
file(MAKE_DIRECTORY "${SHDC_DIR}")

# Always detect the host (not the cross-compile target) platform for shdc.
if(APPLE OR (BUILDING_WASM AND CMAKE_HOST_APPLE))
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(SHDC_PLATFORM "osx_arm64")
    else()
        set(SHDC_PLATFORM "osx")
    endif()
elseif(WIN32 OR (BUILDING_WASM AND CMAKE_HOST_WIN32))
    set(SHDC_PLATFORM "win32")
else()
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(SHDC_PLATFORM "linux_arm64")
    else()
        set(SHDC_PLATFORM "linux")
    endif()
endif()

# Choose the shader target language based on the build target.
if(BUILDING_WASM)
    # WebGL2 requires GLSL ES 3.0
    set(SHADER_SLANG "glsl300es")
elseif(APPLE)
    set(SHADER_SLANG "metal_macos")
elseif(WIN32)
    set(SHADER_SLANG "hlsl5")
else()
    set(SHADER_SLANG "glsl430")
endif()

if(WIN32 AND NOT BUILDING_WASM)
    set(SHDC_EXE "${SHDC_DIR}/sokol-shdc.exe")
    set(SHDC_NAME "sokol-shdc.exe")
else()
    set(SHDC_EXE "${SHDC_DIR}/sokol-shdc")
    set(SHDC_NAME "sokol-shdc")
endif()

set(SHDC_URL "https://raw.githubusercontent.com/floooh/sokol-tools-bin/master/bin/${SHDC_PLATFORM}/${SHDC_NAME}")

if(NOT EXISTS "${SHDC_EXE}")
    message(STATUS "Downloading sokol-shdc for ${SHDC_PLATFORM} ...")
    file(DOWNLOAD "${SHDC_URL}" "${SHDC_EXE}" SHOW_PROGRESS STATUS dl_status)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        message(FATAL_ERROR "Failed to download sokol-shdc: ${dl_status}\nURL: ${SHDC_URL}")
    endif()
    execute_process(COMMAND chmod +x "${SHDC_EXE}")
endif()

# ---------------------------------------------------------------------------
# Compile the GLSL shaders into C headers at build time.
# ---------------------------------------------------------------------------
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

set(SHADER_HEADERS "")
foreach(SH sphere player ball hud fade crt gem)
    set(SRC "${CMAKE_SOURCE_DIR}/shaders/${SH}.glsl")
    set(HDR "${CMAKE_BINARY_DIR}/generated/${SH}.glsl.h")
    add_custom_command(
        OUTPUT  "${HDR}"
        COMMAND "${SHDC_EXE}" --input "${SRC}" --output "${HDR}" --slang ${SHADER_SLANG}
        DEPENDS "${SRC}" "${SHDC_EXE}"
        COMMENT "Compiling shader ${SH}.glsl -> ${SH}.glsl.h (${SHADER_SLANG})"
        VERBATIM
    )
    list(APPEND SHADER_HEADERS "${HDR}")
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_HEADERS})

# ---------------------------------------------------------------------------
# The executable.
# ---------------------------------------------------------------------------
add_executable(blue_spheres
    src/main.c
)

if(BUILDING_WASM)
    # -----------------------------------------------------------------------
    # WebAssembly / Emscripten build
    # -----------------------------------------------------------------------

    # sokol implementation for WASM: clean C file with SOKOL_GLES3 defined
    # before all sokol header includes (critical for proper backend selection).
    target_sources(blue_spheres PRIVATE src/sokol_wasm.c)

    # Emscripten produces a self-contained HTML page.
    set(CMAKE_EXECUTABLE_SUFFIX ".html")

    # Core Emscripten link flags:
    #   -sUSE_WEBGL2=1        — enable WebGL2 (maps to GLES3 in sokol)
    #   -sALLOW_MEMORY_GROWTH — allow heap to grow (games often need this)
    #   -sASYNCIFY            — required by sokol_app.h for the browser event loop
    #   --shell-file          — our custom HTML shell
    target_link_options(blue_spheres PUBLIC
        -sUSE_WEBGL2=1
        -sALLOW_MEMORY_GROWTH=1
        -sASYNCIFY=1
        -sSTACK_SIZE=1048576
        --shell-file "${CMAKE_SOURCE_DIR}/web/shell.html"
    )

elseif(APPLE)
    # -----------------------------------------------------------------------
    # macOS native build
    # -----------------------------------------------------------------------
    target_sources(blue_spheres PRIVATE src/sokol.m)
    set_source_files_properties(src/sokol.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(blue_spheres PRIVATE
        "-framework Metal"
        "-framework MetalKit"
        "-framework AppKit"
        "-framework QuartzCore"
        "-framework AudioToolbox"
    )

else()
    # -----------------------------------------------------------------------
    # Linux / Windows native build
    # -----------------------------------------------------------------------
    configure_file(src/sokol.m "${CMAKE_BINARY_DIR}/sokol_impl.c" COPYONLY)
    target_sources(blue_spheres PRIVATE "${CMAKE_BINARY_DIR}/sokol_impl.c")

    if(UNIX)
        find_package(OpenGL REQUIRED)
        target_link_libraries(blue_spheres PRIVATE
            OpenGL::GL X11 Xi Xcursor m dl pthread)
    elseif(WIN32)
        target_link_libraries(blue_spheres PRIVATE d3d11 dxgi)
    endif()
endif()

add_dependencies(blue_spheres shaders)

target_include_directories(blue_spheres PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty"
    "${CMAKE_BINARY_DIR}/generated"
)