//------------------------------------------------------------------------------
//  sokol.m / sokol.c -- the single translation unit that compiles the sokol
//  implementations. On macOS this MUST be compiled as Objective-C (.m) because
//  the Metal and AppKit backends are written in Objective-C. CMake handles that.
//------------------------------------------------------------------------------
#define SOKOL_IMPL

#if defined(__APPLE__)
    #define SOKOL_METAL
#elif defined(_WIN32)
    #define SOKOL_D3D11
#else
    #define SOKOL_GLCORE
#endif

#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
