cmake_minimum_required(VERSION 3.16)
project(blue_spheres C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Locate / download the prebuilt sokol-shdc shader compiler.
# We grab the single binary for the host platform from the sokol-tools-bin repo.
# ---------------------------------------------------------------------------
set(SHDC_DIR "${CMAKE_BINARY_DIR}/shdc")
file(MAKE_DIRECTORY "${SHDC_DIR}")

if(APPLE)
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(SHDC_PLATFORM "osx_arm64")
    else()
        set(SHDC_PLATFORM "osx")
    endif()
    set(SHADER_SLANG "metal_macos")
elseif(WIN32)
    set(SHDC_PLATFORM "win32")
    set(SHADER_SLANG "hlsl5")
else()
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(SHDC_PLATFORM "linux_arm64")
    else()
        set(SHDC_PLATFORM "linux")
    endif()
    set(SHADER_SLANG "glsl430")
endif()

if(WIN32)
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
    if(NOT WIN32)
        execute_process(COMMAND chmod +x "${SHDC_EXE}")
    endif()
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

# Compile the sokol implementation unit. On macOS it must be Objective-C.
if(APPLE)
    target_sources(blue_spheres PRIVATE src/sokol.m)
    set_source_files_properties(src/sokol.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
else()
    # On non-Apple, rename target unit role: compile sokol.m as C via a wrapper.
    # Simplest path: create a C copy at configure time.
    configure_file(src/sokol.m "${CMAKE_BINARY_DIR}/sokol_impl.c" COPYONLY)
    target_sources(blue_spheres PRIVATE "${CMAKE_BINARY_DIR}/sokol_impl.c")
endif()

add_dependencies(blue_spheres shaders)

target_include_directories(blue_spheres PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty"
    "${CMAKE_BINARY_DIR}/generated"
)

# ---------------------------------------------------------------------------
# Platform-specific linking.
# ---------------------------------------------------------------------------
if(APPLE)
    target_link_libraries(blue_spheres PRIVATE
        "-framework Metal"
        "-framework MetalKit"
        "-framework AppKit"
        "-framework QuartzCore"
        "-framework AudioToolbox"
    )
elseif(UNIX)
    find_package(OpenGL REQUIRED)
    target_link_libraries(blue_spheres PRIVATE
        OpenGL::GL X11 Xi Xcursor m dl pthread)
elseif(WIN32)
    target_link_libraries(blue_spheres PRIVATE d3d11 dxgi)
endif()