/* sokol_wasm.c — Sokol implementation for WebAssembly/Emscripten.
   This must be compiled with GLES3 backend (WebGL2).
   All sokol backend selection happens here, before any includes. */
 
#define SOKOL_GLES3
#define SOKOL_IMPL
 
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"