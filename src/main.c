//------------------------------------------------------------------------------
//  main.c -- Blue Spheres, step 2: grid-locked auto-run movement.
//
//  Movement model (matches the original game):
//    * Sonic is ALWAYS running forward along a grid line (an edge between
//      squares). He never moves freely.
//    * Left / Right do NOT strafe. They queue a 90-degree turn.
//    * The queued turn only fires when you reach the next NODE (the corner of
//      the upcoming square). At that node you snap exactly to the corner and
//      pivot 90 degrees, then keep running.
//    * Only the most recent Left/Right press before the node counts.
//    * The camera always faces "forward", so turning rotates the world under
//      you. We pass the player's facing angle to the shader to do that.
//
//  YELLOW SPHERE (launchpad):
//    * Launches Sonic exactly 6 tiles forward in an arc.
//    * No sphere collision during flight (flies over red spheres safely).
//    * Always launches in the current forward direction.
//    * On landing, touch_node fires once for the landing tile.
//    * Arc peaks ~1.8x normal jump height at the midpoint.
//
//  DEBUG OVERLAY (F1 to toggle):
//    * Shows level, position, direction, angle, frac, blue count.
//    * Remove DBG_* block, build_debug_texture, dbg state fields,
//      init block, frame block, and F1 handler when done debugging.
//------------------------------------------------------------------------------
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "sphere.glsl.h"
#include "player.glsl.h"
#include "ball.glsl.h"
#include "hud.glsl.h"
#include "fade.glsl.h"
#include "crt.glsl.h"
#include "gem.glsl.h"
#include "hud_atlas.h"   // 4x5 bitmap digit atlas

#include "sonic_tex.h"   // embedded sprite sheet (RGBA pixel data)
#include "ring_tex.h"
#include "levels.h"

// minimal mat4 type so the generated @ctype resolves (shader uses no mat4)
typedef struct { float m[16]; } hmm_mat4;

// the four facing directions as integer grid steps, ordered so that
// turning right = (dir+1)%4 and turning left = (dir+3)%4.
//   0 = +Y (north)   1 = +X (east)   2 = -Y (south)   3 = -X (west)
static const int DIR_DX[4] = {  0,  1,  0, -1 };
static const int DIR_DY[4] = {  1,  0, -1,  0 };

// --- spheres -----------------------------------------------------------------
typedef enum {
    SPH_BLUE   = 0,
    SPH_RED    = 1,
    SPH_RING   = 2,
    SPH_STAR   = 3,
    SPH_YELLOW = 4   // launchpad: launches Sonic 6 tiles forward
} sphere_type;

typedef struct {
    int x, y;
    sphere_type type;
    bool active;
} sphere_t;

#define MAX_LEVEL_SPHERES 900
#define MAX_VISIBLE_SPHERES 256
#define VISIBLE_RANGE 8
#define GRID_SIZE 32

// Yellow sphere launch distance (tiles), matches S3&K original
#define YELLOW_LAUNCH_TILES 6.0f

// Run animation: frames 2-12 (sonic2.png-sonic12.png, 0-based indices 1-11),
// held for varying tick counts to smooth the cycle. 120 ticks/s.
static const int RUN_FRAMES[]      = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
static const int RUN_FRAME_TICKS[] = { 5, 3, 4, 3, 4, 5, 4, 3,  4,  3,  4 };
#define RUN_CYCLE_LEN 11

static int gwrap(int v) {
    int r = v % GRID_SIZE;
    return r < 0 ? r + GRID_SIZE : r;
}

static float gwrap_deltaf(float from, int to) {
    float d = (float)to - from;
    while (d >  GRID_SIZE / 2.0f) d -= (float)GRID_SIZE;
    while (d < -GRID_SIZE / 2.0f) d += (float)GRID_SIZE;
    return d;
}

#define G_GR     12.5f
#define G_GCx    0.0f
#define G_GCy    0.0f
#define G_GCz   -12.5f
#define G_PIVOTx 0.0f
#define G_PIVOTy 1.8f
#define BALL_RADIUS_C 0.20f

#define JUMP_DISTANCE   2.0f
#define JUMP_HEIGHT     0.5f

#define BASE_SPEED     4.0f
#define SPEEDUP_PERIOD 30.0f
#define ACCEL 28.125f   // S3&K: $200/frame velocity ramp @60fps

static bool project_ball(const float center[3], float aspect,
                         float* cx, float* cy, float* hx, float* hy, float* depth) {
    float camx = 0.0f, camy = 1.6f, camz = 1.6f;
    float tgx = 0.0f, tgy = 6.0f, tgz = -7.0f;
    float fx = tgx - camx, fy = tgy - camy, fz = tgz - camz;
    float fl = sqrtf(fx*fx + fy*fy + fz*fz); fx/=fl; fy/=fl; fz/=fl;
    float rx = fy*0.0f - fz*1.0f, ry = fz*0.0f - fx*0.0f, rz = fx*1.0f - fy*0.0f;
    float rl = sqrtf(rx*rx + ry*ry + rz*rz); rx/=rl; ry/=rl; rz/=rl;
    float ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;
    float tox = center[0]-camx, toy = center[1]-camy, toz = center[2]-camz;
    float d = tox*fx + toy*fy + toz*fz;
    if (d <= 0.05f) return false;
    *depth = d;
    const float FOCAL = 1.0f;
    float sx = (tox*rx + toy*ry + toz*rz) / d / FOCAL;
    float sy = (tox*ux + toy*uy + toz*uz) / d / FOCAL;
    float ar = (BALL_RADIUS_C / d) / FOCAL;
    *cx = sx / aspect; *cy = sy; *hx = ar / aspect; *hy = ar;
    return true;
}

static void ball_center(float wx, float wy, float pos_x, float pos_y, float rot,
                        float out[3]) {
    float rpx = wx - pos_x, rpy = wy - pos_y;
    float cr = cosf(rot), sr = sinf(rot);
    float gpx = rpx * cr - rpy * sr + G_PIVOTx;
    float gpy = rpx * sr + rpy * cr + G_PIVOTy;
    float dirx = gpx - G_GCx, diry = gpy - G_GCy, dirz = 0.0f - G_GCz;
    float dlen = sqrtf(dirx*dirx + diry*diry + dirz*dirz);
    float t = G_GR / dlen;
    float sx = G_GCx + dirx * t, sy = G_GCy + diry * t, sz = G_GCz + dirz * t;
    float nx = (sx - G_GCx) / G_GR, ny = (sy - G_GCy) / G_GR, nz = (sz - G_GCz) / G_GR;
    out[0] = sx + nx * BALL_RADIUS_C * 0.3f;
    out[1] = sy + ny * BALL_RADIUS_C * 0.3f;
    out[2] = sz + nz * BALL_RADIUS_C * 0.3f;
}

static void ball_center_and_normal(float wx, float wy, float pos_x, float pos_y, float rot,
                                   float out[3], float nout[3]) {
    float rpx = wx - pos_x, rpy = wy - pos_y;
    float cr = cosf(rot), sr = sinf(rot);
    float gpx = rpx * cr - rpy * sr + G_PIVOTx;
    float gpy = rpx * sr + rpy * cr + G_PIVOTy;
    float dirx = gpx - G_GCx, diry = gpy - G_GCy, dirz = 0.0f - G_GCz;
    float dlen = sqrtf(dirx*dirx + diry*diry + dirz*dirz);
    float t = G_GR / dlen;
    float sx = G_GCx + dirx * t, sy = G_GCy + diry * t, sz = G_GCz + dirz * t;
    float nx = (sx - G_GCx) / G_GR, ny = (sy - G_GCy) / G_GR, nz = (sz - G_GCz) / G_GR;
    out[0] = sx + nx * BALL_RADIUS_C * 0.3f;
    out[1] = sy + ny * BALL_RADIUS_C * 0.3f;
    out[2] = sz + nz * BALL_RADIUS_C * 0.3f;
    nout[0] = nx; nout[1] = ny; nout[2] = nz;
}

// ---------------------------------------------------------------------------
// DEBUG OVERLAY -- remove this entire block when done debugging
// ---------------------------------------------------------------------------
#define DBG_TEX_W  512
#define DBG_TEX_H   48
#define DBG_CHAR_W   4
#define DBG_CHAR_H   6
#define DBG_SCALE    2

// 4x6 bitmap font: space, 0-9, A-Z, ( ) , . / = - >
static const uint8_t DBG_FONT[][6] = {
/*' '*/{ 0x0,0x0,0x0,0x0,0x0,0x0 },
/*'0'*/{ 0x6,0x9,0x9,0x9,0x9,0x6 },
/*'1'*/{ 0x2,0x6,0x2,0x2,0x2,0x7 },
/*'2'*/{ 0x6,0x9,0x1,0x2,0x4,0xF },
/*'3'*/{ 0xE,0x1,0x6,0x1,0x1,0xE },
/*'4'*/{ 0x1,0x3,0x5,0xF,0x1,0x1 },
/*'5'*/{ 0xF,0x8,0xE,0x1,0x1,0xE },
/*'6'*/{ 0x6,0x8,0xE,0x9,0x9,0x6 },
/*'7'*/{ 0xF,0x1,0x2,0x2,0x4,0x4 },
/*'8'*/{ 0x6,0x9,0x6,0x9,0x9,0x6 },
/*'9'*/{ 0x6,0x9,0x9,0x7,0x1,0x6 },
/*'A'*/{ 0x6,0x9,0xF,0x9,0x9,0x9 },
/*'B'*/{ 0xE,0x9,0xE,0x9,0x9,0xE },
/*'C'*/{ 0x6,0x9,0x8,0x8,0x9,0x6 },
/*'D'*/{ 0xE,0x9,0x9,0x9,0x9,0xE },
/*'E'*/{ 0xF,0x8,0xE,0x8,0x8,0xF },
/*'F'*/{ 0xF,0x8,0xE,0x8,0x8,0x8 },
/*'G'*/{ 0x6,0x9,0x8,0xB,0x9,0x6 },
/*'H'*/{ 0x9,0x9,0xF,0x9,0x9,0x9 },
/*'I'*/{ 0x7,0x2,0x2,0x2,0x2,0x7 },
/*'J'*/{ 0x1,0x1,0x1,0x1,0x9,0x6 },
/*'K'*/{ 0x9,0xA,0xC,0xA,0x9,0x9 },
/*'L'*/{ 0x8,0x8,0x8,0x8,0x8,0xF },
/*'M'*/{ 0x9,0xF,0xF,0x9,0x9,0x9 },
/*'N'*/{ 0x9,0xD,0xB,0x9,0x9,0x9 },
/*'O'*/{ 0x6,0x9,0x9,0x9,0x9,0x6 },
/*'P'*/{ 0xE,0x9,0x9,0xE,0x8,0x8 },
/*'Q'*/{ 0x6,0x9,0x9,0xB,0x9,0x7 },
/*'R'*/{ 0xE,0x9,0x9,0xE,0xA,0x9 },
/*'S'*/{ 0x6,0x9,0x4,0x2,0x9,0x6 },
/*'T'*/{ 0x7,0x2,0x2,0x2,0x2,0x2 },
/*'U'*/{ 0x9,0x9,0x9,0x9,0x9,0x6 },
/*'V'*/{ 0x9,0x9,0x9,0x9,0x6,0x6 },
/*'W'*/{ 0x9,0x9,0x9,0xF,0xF,0x9 },
/*'X'*/{ 0x9,0x9,0x6,0x6,0x9,0x9 },
/*'Y'*/{ 0x9,0x9,0x6,0x2,0x2,0x2 },
/*'Z'*/{ 0xF,0x1,0x2,0x4,0x8,0xF },
/*'('*/{ 0x2,0x4,0x4,0x4,0x4,0x2 },
/*')'*/{ 0x4,0x2,0x2,0x2,0x2,0x4 },
/*','*/{ 0x0,0x0,0x0,0x0,0x2,0x4 },
/*'.'*/{ 0x0,0x0,0x0,0x0,0x0,0x6 },
/*'/'*/{ 0x1,0x1,0x2,0x4,0x8,0x8 },
/*'='*/{ 0x0,0xF,0x0,0xF,0x0,0x0 },
/*'-'*/{ 0x0,0x0,0xF,0x0,0x0,0x0 },
/*'>'*/{ 0x8,0x4,0x2,0x2,0x4,0x8 },
};

static int dbg_char_idx(char c) {
    if (c == ' ') return 0;
    if (c >= '0' && c <= '9') return 1  + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 11 + (c - 'a');
    if (c == '(') return 37; if (c == ')') return 38;
    if (c == ',') return 39; if (c == '.') return 40;
    if (c == '/') return 41; if (c == '=') return 42;
    if (c == '-') return 43; if (c == '>') return 44;
    return 0;
}

static void dbg_draw_str(uint8_t* tex, int col, int row_off, const char* s) {
    int max_cols = DBG_TEX_W / (DBG_CHAR_W * DBG_SCALE);
    for (int ci = 0; s[ci] && ci < max_cols; ci++) {
        int idx = dbg_char_idx(s[ci]);
        for (int gy = 0; gy < DBG_CHAR_H; gy++) {
            uint8_t bits = DBG_FONT[idx][gy];
            for (int gx = 0; gx < DBG_CHAR_W; gx++) {
                if (!((bits >> (DBG_CHAR_W - 1 - gx)) & 1)) continue;
                for (int sy = 0; sy < DBG_SCALE; sy++)
                    for (int sx = 0; sx < DBG_SCALE; sx++) {
                        int px = (col + ci) * DBG_CHAR_W * DBG_SCALE + gx * DBG_SCALE + sx;
                        int py = row_off + gy * DBG_SCALE + sy;
                        if (px < DBG_TEX_W && py < DBG_TEX_H) {
                            uint8_t* p = tex + (py * DBG_TEX_W + px) * 4;
                            p[0] = 255; p[1] = 255; p[2] = 0; p[3] = 255;
                        }
                    }
            }
        }
    }
}

static void build_debug_texture(uint8_t* tex, const char* line1, const char* line2) {
    memset(tex, 0, DBG_TEX_W * DBG_TEX_H * 4);
    // semi-transparent dark background strip
    for (int py = 0; py < DBG_TEX_H; py++)
        for (int px = 0; px < DBG_TEX_W; px++) {
            uint8_t* p = tex + (py * DBG_TEX_W + px) * 4;
            p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 180;
        }
    int line_h = DBG_CHAR_H * DBG_SCALE + 2;
    dbg_draw_str(tex, 0, 1,        line1);
    dbg_draw_str(tex, 0, line_h,   line2);
}
// ---------------------------------------------------------------------------
// END DEBUG OVERLAY BLOCK
// ---------------------------------------------------------------------------

static struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_pass_action pass_action;

    sg_pipeline player_pip;
    sg_bindings player_bind;
    sg_image    player_tex;
    sg_sampler  player_smp;
    float player_phase;
    int   player_frame;
    int   run_cycle_idx;
    int   run_tick;

    sg_image    ring_tex;
    sg_bindings ring_bind;
    sg_pipeline ring_pip;

    sg_pipeline ball_pip;
    sg_bindings ball_bind;
    float ring_spin;

    sg_pipeline hud_pip;
    sg_bindings hud_bind;
    sg_image    hud_tex;
    sg_sampler  hud_smp;

    sg_pipeline fade_pip;
    sg_bindings fade_bind;

    // PERFECT notification (Mania-style split slide)
    int         perfect_phase;
    float       perfect_timer;
    sg_image    perfect_img;
    sg_bindings perfect_bind;
    bool        perfect_phase_triggered;

    // Congratulations screen
    bool        congrats;
    float       congrats_timer;
    float       emerald_spin;
    float       wbk_timer;
    sg_image    congrats_img;
    sg_bindings congrats_bind;
    sg_pipeline gem_pip;
    sg_bindings gem_bind;

    // CRT post-process
    sg_image        crt_img;
    sg_view         crt_att_view;
    sg_view         crt_depth_att_view;
    sg_pipeline     crt_pip;
    sg_bindings     crt_bind;
    sg_pass_action  crt_pass_action;
    bool            crt_enabled;

    // --- debug overlay (remove when done) ---
    sg_image    dbg_img;
    sg_bindings dbg_bind;
    bool        dbg_show;

    int   node_x, node_y;
    float frac;
    int   dir;
    int   pending_turn;
    float speed;
    float stage_time;
    float cur_speed;
    int  held_turn;             // -1 left held, +1 right held, 0 none
    bool left_down, right_down;
    bool turn_lock;             // set after a turn fires, cleared mid-edge

    int   move_sign;
    float bounce_dist;
    float backward_travel;
    bool  forward_queued;
    int   last_star_x, last_star_y;

    int   no_pivot_x, no_pivot_y; // bumper node just bounced off: no turning there
    bool  no_pivot;
    bool  turn_air_queued;        // pending_turn was queued while airborne

    float vis_angle;
    float target_angle;
    float turn_speed;
    bool  turning;

    double accum;

    bool  jumping;
    float jump_total;
    float jump_remaining;
    float height;
    bool  jump_queued;
    float land_offcenter;

    // Yellow launchpad state
    bool  launching;
    float launch_remaining;
    float launch_total;

    sphere_t spheres[MAX_LEVEL_SPHERES];
    int      sphere_count;
    int      blue_remaining;
    int      rings;
    int      rings_remaining;
    int      max_rings;
    int      current_level;
    bool     game_over;
    bool     game_over_spinning;
    float    game_over_spin_speed;
    float    game_over_timer;
    float    fade_in_timer;
    bool     won;
    float    win_lift;
    bool     started;
    bool     paused;
    int      last_sphere_x, last_sphere_y;
    int      grace_x, grace_y;
    bool     jump_down;

    uint64_t last_time;
} state;

static void reset_game(int level) {
    if (level < 0) level = 0;
    if (level >= NUM_LEVELS) level = NUM_LEVELS - 1;
    state.current_level = level;
    const level_desc_t* lv = &LEVELS[level];
    state.node_x = lv->start_x; state.node_y = lv->start_y;
    state.frac = 0.0f; state.dir = lv->start_dir;
    state.pending_turn = 0; state.speed = 4.0f;
    state.stage_time = 0.0f;
    state.move_sign = 1; state.bounce_dist = 1.0f; state.backward_travel = 0.0f;
    state.forward_queued = false;
    state.last_star_x = -1; state.last_star_y = -1;
    state.last_sphere_x = lv->start_x; state.last_sphere_y = lv->start_y;
    state.sphere_count = 0; state.blue_remaining = 0;
    state.rings = 0; state.game_over = false; state.won = false; state.win_lift = 0.0f;
    state.game_over_spinning = false;
    state.game_over_spin_speed = 0.0f;
    state.game_over_timer = 0.0f;
    state.fade_in_timer = 0.0f;
    state.max_rings = lv->max_rings;
    state.rings_remaining = lv->max_rings;
    state.started = false;
    state.paused = false;
    for (int i = 0; i < lv->count && i < MAX_LEVEL_SPHERES; i++) {
        sphere_t* s = &state.spheres[state.sphere_count];
        s->x = gwrap(lv->layout[i][0]); s->y = gwrap(lv->layout[i][1]);
        s->type = (sphere_type)lv->layout[i][2]; s->active = true;
        state.sphere_count++;
        if (s->type == SPH_BLUE) state.blue_remaining++;
    }
    state.vis_angle = lv->start_angle; state.target_angle = lv->start_angle;
    state.turn_speed = 1.5707963f / (14.0f / 60.0f);
    state.cur_speed = 0.0f;
    state.turn_lock = false;
    state.turning = false; state.accum = 0.0;
    state.jumping = false; state.jump_total = 0.0f;
    state.jump_remaining = 0.0f; state.height = 0.0f;
    state.jump_queued = false;
    state.launching = false;
    state.launch_remaining = 0.0f;
    state.launch_total = 0.0f;
    state.player_phase = 0.0f; state.player_frame = 0;
    state.run_cycle_idx = 0;
    state.run_tick = 0;
    state.ring_spin = 0.0f;
    state.perfect_phase = 0;
    state.perfect_timer = 0.0f;
    state.perfect_phase_triggered = false;
    state.congrats = false;
    state.congrats_timer = 0.0f;
    state.emerald_spin = 0.0f;
    state.wbk_timer = 0.0f;
    state.no_pivot = false; state.no_pivot_x = -1; state.no_pivot_y = -1;
    state.turn_air_queued = false;
    state.grace_x = -1; state.grace_y = -1;
    state.land_offcenter = 0.0f;
}

// ---------------------------------------------------------------------------
//  PERFECT notification texture (128×20 RGBA8)
// ---------------------------------------------------------------------------
#define PERF_TEX_W  128
#define PERF_TEX_H  20
#define PERF_SPLIT  56
#define PERF_END    98

static const int perf_slots[7] = { 0, 14, 28, 42, 56, 70, 84 };

static const uint8_t perf_glyphs[7][7][5] = {
    {{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,0}}, // P
    {{1,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,1}}, // E
    {{1,0,0,0,1},{1,0,0,1,0},{1,0,1,0,0},{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,0}}, // R
    {{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,1}}, // F
    {{1,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,1}}, // E
    {{0,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{0,1,1,1,1}}, // C
    {{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{1,1,1,1,1}}, // T
};

static void build_perfect_texture(uint8_t* tex) {
    memset(tex, 0, PERF_TEX_W * PERF_TEX_H * 4);
    for (int letter = 0; letter < 7; letter++) {
        int cx = perf_slots[letter] + 1;
        int cy = 2;
        for (int gy = 0; gy < 7; gy++) {
            uint8_t v = (uint8_t)(0xFF - gy * 14);
            for (int gx = 0; gx < 5; gx++) {
                if (!perf_glyphs[letter][gy][gx]) continue;
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        int px = cx + gx*2 + sx, py = cy + 1 + gy*2 + sy;
                        if (px < PERF_TEX_W && py < PERF_TEX_H) {
                            uint8_t* p = tex + (py*PERF_TEX_W+px)*4;
                            p[0]=v; p[1]=v; p[2]=v; p[3]=0xFF;
                        }
                    }
                }
            }
        }
    }
    static uint8_t src[PERF_TEX_W * PERF_TEX_H * 4];
    memcpy(src, tex, sizeof(src));
    for (int y = 0; y < PERF_TEX_H; y++) {
        for (int x = 0; x < PERF_TEX_W; x++) {
            if (src[(y*PERF_TEX_W+x)*4+3]) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++)
                for (int dx = -1; dx <= 1 && !near; dx++) {
                    if (!dx && !dy) continue;
                    int nx2 = x+dx, ny2 = y+dy;
                    if (nx2>=0 && nx2<PERF_TEX_W && ny2>=0 && ny2<PERF_TEX_H)
                        if (src[(ny2*PERF_TEX_W+nx2)*4+3]) near = true;
                }
            if (near) {
                uint8_t* p = tex + (y*PERF_TEX_W+x)*4;
                p[0]=0x28; p[1]=0x28; p[2]=0x28; p[3]=0xFF;
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  CONGRATULATIONS screen texture (256×24 RGBA8)
// ---------------------------------------------------------------------------
#define CONG_GLYPH_W  7
#define CONG_GLYPH_H  9
#define CONG_SLOT_W   16
#define CONG_TEX_W   256
#define CONG_TEX_H    24
#define CONG_LEN      15
#define CONG_PX       240

static const int cong_seq[CONG_LEN] = {0,1,2,3,4,5,6,7,8,5,6,9,1,2,10};

static const uint8_t cong_glyphs[11][9][7] = {
    {{0,0,1,1,1,0,0},{0,1,0,0,0,1,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{0,1,0,0,0,1,0},{0,0,1,1,1,0,0}},
    {{0,0,1,1,1,0,0},{0,1,0,0,0,1,0},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{0,1,0,0,0,1,0},{0,0,1,1,1,0,0}},
    {{1,0,0,0,0,0,1},{1,0,0,0,0,1,1},{1,0,0,0,0,1,1},{1,0,0,0,1,0,1},{1,0,0,1,0,0,1},{1,0,1,0,0,0,1},{1,1,0,0,0,0,1},{1,1,0,0,0,0,1},{1,0,0,0,0,0,1}},
    {{0,0,1,1,1,0,0},{0,1,0,0,0,1,0},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,1,1,1,1},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{0,1,0,0,0,1,0},{0,0,1,1,1,0,0}},
    {{1,0,0,0,0,1,0},{1,0,0,0,1,0,0},{1,0,0,1,0,0,0},{1,0,1,0,0,0,0},{1,1,1,1,1,0,0},{1,0,0,0,0,1,0},{1,0,0,0,0,0,1},{1,0,0,0,0,1,0},{1,1,1,1,1,0,0}},
    {{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,1,1,1,1,1,1},{0,1,0,0,0,1,0},{0,1,0,0,0,1,0},{0,0,1,0,1,0,0},{0,0,1,0,1,0,0},{0,0,0,1,0,0,0}},
    {{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{1,1,1,1,1,1,1}},
    {{0,0,1,1,1,0,0},{0,1,0,0,0,1,0},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1},{1,0,0,0,0,0,1}},
    {{1,1,1,1,1,1,1},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0},{1,0,0,0,0,0,0}},
    {{1,1,1,1,1,1,1},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{0,0,0,1,0,0,0},{1,1,1,1,1,1,1}},
    {{0,0,1,1,1,0,0},{0,1,0,0,0,1,0},{0,0,0,0,0,0,1},{0,0,0,0,0,1,0},{0,0,1,1,1,0,0},{0,1,0,0,0,0,0},{1,0,0,0,0,0,0},{0,1,0,0,0,1,0},{0,0,1,1,1,0,0}},
};

static const int cong_slots[CONG_LEN] = {
    0,16,32,48,64,80,96,112,128,144,160,176,192,208,224
};

static void build_congrats_texture(uint8_t* tex) {
    memset(tex, 0, CONG_TEX_W * CONG_TEX_H * 4);
    for (int li = 0; li < CONG_LEN; li++) {
        int gl = cong_seq[li];
        int cx = cong_slots[li] + 2, cy = 3;
        for (int gy = 0; gy < CONG_GLYPH_H; gy++)
            for (int gx = 0; gx < CONG_GLYPH_W; gx++) {
                if (!cong_glyphs[gl][gy][gx]) continue;
                for (int sy = 0; sy < 2; sy++)
                    for (int sx = 0; sx < 2; sx++) {
                        int px = cx+gx*2+sx, py = cy+1+gy*2+sy;
                        if (px < CONG_TEX_W && py < CONG_TEX_H) {
                            uint8_t* p = tex+(py*CONG_TEX_W+px)*4;
                            if (p[3] == 0) { p[0]=0x00; p[1]=0x08; p[2]=0x10; p[3]=0xCC; }
                        }
                    }
            }
    }
    static const uint8_t GRAD[9] = {255, 245, 228, 205, 178, 150, 125, 105, 90};
    for (int li = 0; li < CONG_LEN; li++) {
        int gl = cong_seq[li];
        int cx = cong_slots[li] + 1, cy = 2;
        for (int gy = 0; gy < CONG_GLYPH_H; gy++)
            for (int gx = 0; gx < CONG_GLYPH_W; gx++) {
                if (!cong_glyphs[gl][gy][gx]) continue;
                for (int sy = 0; sy < 2; sy++)
                    for (int sx = 0; sx < 2; sx++) {
                        int px = cx+gx*2+sx, py = cy+1+gy*2+sy;
                        if (px < CONG_TEX_W && py < CONG_TEX_H) {
                            uint8_t* p = tex+(py*CONG_TEX_W+px)*4;
                            p[0]=GRAD[gy]; p[1]=GRAD[gy]; p[2]=GRAD[gy]; p[3]=0xFF;
                        }
                    }
            }
    }
    static uint8_t src[CONG_TEX_W * CONG_TEX_H * 4];
    memcpy(src, tex, sizeof(src));
    for (int y = 0; y < CONG_TEX_H; y++)
        for (int x = 0; x < CONG_TEX_W; x++) {
            if (src[(y*CONG_TEX_W+x)*4+3] == 0xFF) continue;
            bool near = false;
            for (int dy2=-1; dy2<=1 && !near; dy2++)
                for (int dx2=-1; dx2<=1 && !near; dx2++) {
                    if (!dx2 && !dy2) continue;
                    int nx2=x+dx2, ny2=y+dy2;
                    if (nx2>=0&&nx2<CONG_TEX_W&&ny2>=0&&ny2<CONG_TEX_H)
                        if (src[(ny2*CONG_TEX_W+nx2)*4+3]==0xFF) near=true;
                }
            if (near) {
                uint8_t* p = tex+(y*CONG_TEX_W+x)*4;
                p[0]=0x28; p[1]=0x28; p[2]=0x28; p[3]=0xFF;
            }
        }
    for (int li = 0; li < CONG_LEN; li++) {
        int gl = cong_seq[li];
        int cx = cong_slots[li] + 1, cy = 2;
        for (int gx = 0; gx < CONG_GLYPH_W; gx++) {
            for (int gy = 0; gy < CONG_GLYPH_H; gy++) {
                if (!cong_glyphs[gl][gy][gx]) continue;
                for (int sx = 0; sx < 2; sx++) {
                    int px = cx+gx*2+sx, py = cy+1+gy*2;
                    if (px < CONG_TEX_W && py < CONG_TEX_H) {
                        uint8_t* p = tex+(py*CONG_TEX_W+px)*4;
                        if (p[3]==0xFF) { p[0]=255; p[1]=255; p[2]=255; }
                    }
                }
                break;
            }
        }
    }
}

static void init(void) {
    sg_setup(&(sg_desc){ .environment = sglue_environment(), .logger.func = slog_func });
    stm_setup(); state.last_time = stm_now();
    reset_game(0);

    const float verts[] = { -1,-1, 3,-1, -1,3 };
    state.bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(verts), .label = "fullscreen-tri" });
    state.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(sphere_shader_desc(sg_query_backend())),
        .layout.attrs[ATTR_sphere_position].format = SG_VERTEXFORMAT_FLOAT2,
        .label = "sphere-pipeline" });
    state.pass_action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                        .clear_value = { 0.55f, 0.80f, 1.0f, 1.0f } } };

    const float quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };
    state.player_bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad), .label = "player-quad" });
    state.player_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(player_shader_desc(sg_query_backend())),
        .layout.attrs[ATTR_player_corner].format = SG_VERTEXFORMAT_FLOAT2,
        .colors[0].blend = { .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ZERO },
        .label = "player-pipeline" });

    state.player_tex = sg_make_image(&(sg_image_desc){
        .width = SONIC_TEX_WIDTH, .height = SONIC_TEX_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { .ptr = sonic_tex_data, .size = sizeof(sonic_tex_data) },
        .label = "sonic-sprite-tex" });
    state.player_smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "sonic-sprite-smp" });
    sg_view player_view = sg_make_view(&(sg_view_desc){
        .texture.image = state.player_tex, .label = "sonic-sprite-view" });
    state.player_bind.views[VIEW_sprite_tex] = player_view;
    state.player_bind.samplers[SMP_sprite_smp] = state.player_smp;

    state.ring_tex = sg_make_image(&(sg_image_desc){
        .width = RING_TEX_WIDTH, .height = RING_TEX_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { .ptr = ring_tex_data, .size = sizeof(ring_tex_data) },
        .label = "ring-sprite-tex" });
    sg_view ring_view = sg_make_view(&(sg_view_desc){
        .texture.image = state.ring_tex, .label = "ring-sprite-view" });
    state.ring_bind.vertex_buffers[0] = state.player_bind.vertex_buffers[0]; // reuse quad
    state.ring_bind.views[VIEW_sprite_tex] = ring_view;
    state.ring_bind.samplers[SMP_sprite_smp] = state.player_smp;             // reuse sampler
    state.ring_pip = state.player_pip;                                       // reuse player pipeline

    state.ball_bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad), .label = "ball-quad" });
    state.ball_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(ball_shader_desc(sg_query_backend())),
        .layout.attrs[ATTR_ball_corner].format = SG_VERTEXFORMAT_FLOAT2,
        .colors[0].blend = { .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ZERO },
        .label = "ball-pipeline" });

    state.hud_tex = sg_make_image(&(sg_image_desc){
        .width = HUD_ATLAS_W, .height = HUD_ATLAS_H,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { .ptr = hud_atlas_data, .size = sizeof(hud_atlas_data) },
        .label = "hud-atlas" });
    state.hud_smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "hud-smp" });
    sg_view hud_view = sg_make_view(&(sg_view_desc){
        .texture.image = state.hud_tex, .label = "hud-view" });
    state.hud_bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad), .label = "hud-quad" });
    state.hud_bind.views[VIEW_hud_tex] = hud_view;
    state.hud_bind.samplers[SMP_hud_smp] = state.hud_smp;
    state.hud_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(hud_shader_desc(sg_query_backend())),
        .layout.attrs[ATTR_hud_corner].format = SG_VERTEXFORMAT_FLOAT2,
        .colors[0].blend = { .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ZERO },
        .label = "hud-pipeline" });

    state.fade_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(fade_shader_desc(sg_query_backend())),
        .layout.attrs[ATTR_fade_position].format = SG_VERTEXFORMAT_FLOAT2,
        .colors[0].blend = { .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ZERO },
        .label = "fade-pipeline" });
    state.fade_bind.vertex_buffers[0] = state.bind.vertex_buffers[0];

    // --- CRT post-process ---------------------------------------------------
    int crt_w = sapp_width(), crt_h = sapp_height();
    state.crt_img = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true,
        .width = crt_w, .height = crt_h,
        .sample_count = 1,
        .label = "crt-offscreen-color" });
    state.crt_att_view = sg_make_view(&(sg_view_desc){
        .color_attachment.image = state.crt_img,
        .label = "crt-color-att-view" });
    sg_pixel_format depth_fmt = sg_query_desc().environment.defaults.depth_format;
    sg_image crt_depth_img = sg_make_image(&(sg_image_desc){
        .usage.depth_stencil_attachment = true,
        .width = crt_w, .height = crt_h,
        .pixel_format = depth_fmt,
        .sample_count = 1,
        .label = "crt-offscreen-depth" });
    state.crt_depth_att_view = sg_make_view(&(sg_view_desc){
        .depth_stencil_attachment.image = crt_depth_img,
        .label = "crt-depth-att-view" });
    sg_view crt_tex_view = sg_make_view(&(sg_view_desc){
        .texture.image = state.crt_img,
        .label = "crt-tex-view" });
    state.crt_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(crt_shader_desc(sg_query_backend())),
        .layout.attrs[ATTR_crt_position].format = SG_VERTEXFORMAT_FLOAT2,
        .label = "crt-pipeline" });
    state.crt_bind.vertex_buffers[0] = state.bind.vertex_buffers[0];
    state.crt_bind.views[VIEW_screen_tex]    = crt_tex_view;
    state.crt_bind.samplers[SMP_screen_smp] = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "crt-smp" });
    state.crt_pass_action = (sg_pass_action){
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                       .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } } };

    // --- PERFECT notification -----------------------------------------------
    static uint8_t perfect_pixels[PERF_TEX_W * PERF_TEX_H * 4];
    build_perfect_texture(perfect_pixels);
    state.perfect_img = sg_make_image(&(sg_image_desc){
        .width = PERF_TEX_W, .height = PERF_TEX_H,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { .ptr = perfect_pixels, .size = sizeof(perfect_pixels) },
        .label = "perfect-tex" });
    sg_view perfect_view = sg_make_view(&(sg_view_desc){
        .texture.image = state.perfect_img, .label = "perfect-view" });
    state.perfect_bind = state.hud_bind;
    state.perfect_bind.views[VIEW_hud_tex] = perfect_view;

    // --- CONGRATULATIONS screen texture -------------------------------------
    static uint8_t cong_pixels[CONG_TEX_W * CONG_TEX_H * 4];
    build_congrats_texture(cong_pixels);
    state.congrats_img = sg_make_image(&(sg_image_desc){
        .width = CONG_TEX_W, .height = CONG_TEX_H,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { .ptr = cong_pixels, .size = sizeof(cong_pixels) },
        .label = "congrats-tex" });
    sg_view congrats_view = sg_make_view(&(sg_view_desc){
        .texture.image = state.congrats_img, .label = "congrats-view" });
    state.congrats_bind = state.hud_bind;
    state.congrats_bind.views[VIEW_hud_tex] = congrats_view;

    // --- Spinning 3D Chaos Emerald ------------------------------------------
    state.gem_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(gem_shader_desc(sg_query_backend())),
        .layout.attrs[ATTR_gem_corner].format = SG_VERTEXFORMAT_FLOAT2,
        .colors[0].blend = { .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ZERO },
        .label = "gem-pipeline" });
    state.gem_bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad), .label = "gem-quad" });

    // --- debug overlay init (remove when done) ------------------------------
    state.dbg_img = sg_make_image(&(sg_image_desc){
        .width  = DBG_TEX_W,
        .height = DBG_TEX_H,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage.stream_update = true,
        .label = "dbg-overlay" });
    sg_view dbg_view = sg_make_view(&(sg_view_desc){
        .texture.image = state.dbg_img, .label = "dbg-view" });
    state.dbg_bind = state.hud_bind;
    state.dbg_bind.views[VIEW_hud_tex] = dbg_view;
    state.dbg_show = false;  // off by default; F1 toggles

    // enable crt by default
    state.crt_enabled = true;
}

static int sphere_at(int x, int y) {
    int wx = gwrap(x), wy = gwrap(y);
    for (int i = 0; i < state.sphere_count; i++) {
        sphere_t* s = &state.spheres[i];
        if (s->active && s->x == wx && s->y == wy) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Ring conversion: Sonic Mania-style chain-trace algorithm
// ---------------------------------------------------------------------------
static unsigned char chain_cluster[GRID_SIZE * GRID_SIZE];
static int g_conv_x[MAX_LEVEL_SPHERES], g_conv_y[MAX_LEVEL_SPHERES], g_conv_n;

static bool is_enclosed_by_cluster(int bx, int by) {
    const int dx4[4] = {1, -1, 0, 0};
    const int dy4[4] = {0, 0, 1, -1};
    for (int d = 0; d < 4; d++) {
        bool bounded = false;
        int cx = bx, cy = by;
        for (int step = 0; step < GRID_SIZE; step++) {
            cx = gwrap(cx + dx4[d]);
            cy = gwrap(cy + dy4[d]);
            int li = cy * GRID_SIZE + cx;
            if (chain_cluster[li]) { bounded = true; break; }
            int idx = sphere_at(cx, cy);
            if (idx < 0) break;
            if (state.spheres[idx].type == SPH_RED) break;
        }
        if (!bounded) return false;
    }
    return true;
}

static void convert_enclosed_to_rings(void) {
    for (int i = 0; i < GRID_SIZE * GRID_SIZE; i++) chain_cluster[i] = 0;

    int stackx[GRID_SIZE * GRID_SIZE], stacky[GRID_SIZE * GRID_SIZE], sp = 0;
    int lsx = state.last_sphere_x, lsy = state.last_sphere_y;
    chain_cluster[lsy * GRID_SIZE + lsx] = 1;
    stackx[sp] = lsx; stacky[sp] = lsy; sp++;

    const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    while (sp > 0) {
        sp--;
        int cx = stackx[sp], cy = stacky[sp];
        for (int d = 0; d < 4; d++) {
            int nx = gwrap(cx + dx4[d]), ny = gwrap(cy + dy4[d]);
            int li = ny * GRID_SIZE + nx;
            if (chain_cluster[li]) continue;
            int idx = sphere_at(nx, ny);
            if (idx >= 0 && state.spheres[idx].type == SPH_RED) {
                chain_cluster[li] = 1;
                stackx[sp] = nx; stacky[sp] = ny; sp++;
            }
        }
    }

    {
        int nbx[4], nby[4], nn = 0;
        for (int d = 0; d < 4; d++) {
            int nx = gwrap(lsx + dx4[d]), ny = gwrap(lsy + dy4[d]);
            if (chain_cluster[ny * GRID_SIZE + nx])
                { nbx[nn] = nx; nby[nn] = ny; nn++; }
        }
        if (nn < 2) return;

        static unsigned char loop_visited[GRID_SIZE * GRID_SIZE];
        for (int i = 0; i < GRID_SIZE * GRID_SIZE; i++) loop_visited[i] = 0;
        sp = 0;
        loop_visited[nby[0] * GRID_SIZE + nbx[0]] = 1;
        stackx[sp] = nbx[0]; stacky[sp] = nby[0]; sp++;
        while (sp > 0) {
            sp--;
            int cx = stackx[sp], cy = stacky[sp];
            for (int d = 0; d < 4; d++) {
                int nx = gwrap(cx + dx4[d]), ny = gwrap(cy + dy4[d]);
                int li = ny * GRID_SIZE + nx;
                if (nx == lsx && ny == lsy) continue;
                if (loop_visited[li] || !chain_cluster[li]) continue;
                loop_visited[li] = 1;
                stackx[sp] = nx; stacky[sp] = ny; sp++;
            }
        }
        bool has_loop = false;
        for (int n = 1; n < nn; n++) {
            if (loop_visited[nby[n] * GRID_SIZE + nbx[n]]) {
                has_loop = true; break;
            }
        }
        if (!has_loop) return;
    }

    g_conv_n = 0;
    for (int i = 0; i < state.sphere_count; i++) {
        sphere_t* s = &state.spheres[i];
        if (!s->active || s->type != SPH_BLUE) continue;
        if (is_enclosed_by_cluster(s->x, s->y)) {
            s->type = SPH_RING;
            if (state.blue_remaining > 0) state.blue_remaining--;
            if (g_conv_n < MAX_LEVEL_SPHERES) {
                g_conv_x[g_conv_n] = s->x;
                g_conv_y[g_conv_n] = s->y;
                g_conv_n++;
            }
        }
    }

    if (g_conv_n > 0) {
        const int wx8[8]={1,-1,0,0,1,1,-1,-1}, wy8[8]={0,0,1,-1,1,-1,1,-1};
        int head = 0;
        while (head < g_conv_n) {
            int bx = g_conv_x[head], by = g_conv_y[head];
            head++;
            for (int d = 0; d < 8; d++) {
                int rx = gwrap(bx + wx8[d]), ry = gwrap(by + wy8[d]);
                int li = ry * GRID_SIZE + rx;
                if (!chain_cluster[li]) continue;
                int ri = sphere_at(rx, ry);
                if (ri >= 0 && state.spheres[ri].type == SPH_RED) {
                    state.spheres[ri].type = SPH_RING;
                }
            }
        }
    }
}

static bool touch_node(int nx, int ny) {
    if (state.won) return false;
    if (state.jumping || state.launching) return false;   // airborne: no collision (S3&K)
    int idx = sphere_at(nx, ny);
    if (idx < 0) return false;
    sphere_t* s = &state.spheres[idx];
    if (s->type == SPH_RED) {
        if (state.bounce_dist < 0.5f) return false;        // post-bounce grace (any dir)
        if (nx == state.grace_x && ny == state.grace_y)
            return false;                                  // still on the cell we just converted
        float off = state.land_offcenter > 0.0f ? state.land_offcenter
                  : ((state.frac > 0.5f) ? (1.0f - state.frac) : state.frac);
        if (off > 0.125f) return false;                    // not centered: thread it (S3&K $E0 gate)
        state.game_over = true;
        state.game_over_spinning = true;
        state.game_over_spin_speed = 4.0f;
        state.game_over_timer = 0.0f;
        state.fade_in_timer = 0.0f;
    } else if (s->type == SPH_BLUE) {
        state.last_sphere_x = nx;
        state.last_sphere_y = ny;
        s->type = SPH_RED;
        state.grace_x = nx; state.grace_y = ny;            // immunity while still on this cell
        if (state.blue_remaining > 0) state.blue_remaining--;
        convert_enclosed_to_rings();
        if (s->type == SPH_RING) {
            s->active = false; state.rings++;
            if (state.rings_remaining > 0) state.rings_remaining--;
        }
    } else if (s->type == SPH_RING) {
        s->active = false; state.rings++;
        if (state.rings_remaining > 0) state.rings_remaining--;
    } else if (s->type == SPH_STAR) {
        if (nx == state.last_star_x && ny == state.last_star_y) {
            state.last_star_x = -1; state.last_star_y = -1;
            return false;
        }
        state.last_star_x = nx; state.last_star_y = ny;
        state.move_sign = -state.move_sign;
        state.bounce_dist = 0.0f;
        state.backward_travel = 0.0f;
        state.forward_queued = false;
        {
            int tx = gwrap(nx + DIR_DX[state.dir] * state.move_sign);
            int ty = gwrap(ny + DIR_DY[state.dir] * state.move_sign);
            int ti = sphere_at(tx, ty);
            bool trapped  = (ti >= 0 && state.spheres[ti].type == SPH_STAR);
            bool air_turn = (state.pending_turn != 0 && state.turn_air_queued);
            state.no_pivot   = !(trapped || air_turn);
            state.no_pivot_x = nx;
            state.no_pivot_y = ny;
        }
        if (!state.game_over && state.blue_remaining == 0) state.won = true;
        return true;
    } else if (s->type == SPH_YELLOW) {
        state.launching        = true;
        state.launch_total     = YELLOW_LAUNCH_TILES;
        state.launch_remaining = YELLOW_LAUNCH_TILES;
        state.move_sign        = 1;
        state.forward_queued   = false;
        state.backward_travel  = 0.0f;
        state.bounce_dist      = 1.0f;
        state.jumping          = false;
        state.height           = 0.0f;
        return false;
    }
    if (!state.game_over && state.blue_remaining == 0) state.won = true;
    return false;
}

// Execute a queued 90-degree turn if we're at a node boundary and allowed to.
// Returns true if the turn fired. Pivoting on the bumper we just bounced off
// is forbidden (no_pivot) unless touch_node cleared it via the trapped /
// air-queued exceptions. turn_lock enforces one turn per node: set here,
// released only after traveling to mid-edge (see the poll in frame()).
static bool try_execute_turn(void) {
    if (state.jumping || state.pending_turn == 0) return false;
    if (!(state.frac < 1e-4f || state.frac > 1.0f - 1e-4f)) return false;
    int tnx = state.node_x, tny = state.node_y;
    if (state.frac > 0.5f) {
        tnx = gwrap(tnx + DIR_DX[state.dir]);
        tny = gwrap(tny + DIR_DY[state.dir]);
    }
    if (state.no_pivot && tnx == state.no_pivot_x && tny == state.no_pivot_y)
        return false;                       // can't pivot on that bumper
    state.node_x = tnx; state.node_y = tny; state.frac = 0.0f;
    if (state.pending_turn == -1) {
        state.dir = (state.dir + 3) & 3;
        state.target_angle -= 1.5707963f;
    } else {
        state.dir = (state.dir + 1) & 3;
        state.target_angle += 1.5707963f;
    }
    state.pending_turn = 0;
    state.turning = true;
    state.turn_lock = true;                 // one turn per node (S3&K)
    state.turn_air_queued = false;
    state.no_pivot = false;
    return true;
}

static void advance(float dist) {
    if (try_execute_turn()) return;

    while (dist > 0.0f) {
        float room = state.move_sign > 0 ? (1.0f - state.frac) : state.frac;
        if (dist < room) {
            state.frac += (float)state.move_sign * dist;
            state.bounce_dist = fminf(1.0f, state.bounce_dist + dist);
            if (state.move_sign < 0) state.backward_travel += dist;
            dist = 0.0f;
        } else {
            if (state.move_sign > 0) {
                state.node_x = gwrap(state.node_x + DIR_DX[state.dir]);
                state.node_y = gwrap(state.node_y + DIR_DY[state.dir]);
                state.frac = 0.0f;
                dist -= room;
                state.bounce_dist = fminf(1.0f, state.bounce_dist + room);
                if (!(state.node_x == state.grace_x && state.node_y == state.grace_y)) {
                    state.grace_x = -1; state.grace_y = -1;   // left the converted cell
                }
                if (touch_node(state.node_x, state.node_y))
                    return;
            } else {
                state.frac = 0.0f;
                dist -= room;
                state.bounce_dist = fminf(1.0f, state.bounce_dist + room);
                state.backward_travel += room;
                if (!(state.node_x == state.grace_x && state.node_y == state.grace_y)) {
                    state.grace_x = -1; state.grace_y = -1;
                }
                if (touch_node(state.node_x, state.node_y))
                    return;
                state.node_x = gwrap(state.node_x - DIR_DX[state.dir]);
                state.node_y = gwrap(state.node_y - DIR_DY[state.dir]);
                state.frac = 1.0f;
            }
            if (state.game_over) return;
            if (try_execute_turn()) return;
        }
    }
}

static void frame(void) {
    double dt = stm_sec(stm_laptime(&state.last_time));
    if (dt > 0.25) dt = 0.25;
    state.accum += dt;
    const double FIXED_DT = 1.0 / 120.0;

    while (state.accum >= FIXED_DT) {
        state.accum -= FIXED_DT;
        if (!state.started || state.paused) {
            state.jump_queued = false;
            state.fade_in_timer += (float)FIXED_DT;
            state.stage_time += (float)FIXED_DT;
            if (!state.started) {       // animate in-place turns pre-start
                float diff = state.target_angle - state.vis_angle;
                float maxstep = state.turn_speed * (float)FIXED_DT;
                if (diff >  maxstep) diff =  maxstep;
                if (diff < -maxstep) diff = -maxstep;
                state.vis_angle += diff;
                if (state.turning && fabsf(state.target_angle - state.vis_angle) < 1e-4f) {
                    state.vis_angle = state.target_angle;
                    state.turning = false;
                }
            }
            continue;
        }

        if (state.congrats) {
            state.fade_in_timer  += (float)FIXED_DT;
            state.congrats_timer += (float)FIXED_DT;
            state.emerald_spin   += 7.0f * (float)FIXED_DT;
            if (state.emerald_spin > 6.2831853f) state.emerald_spin -= 6.2831853f;
            continue;
        }

        if (state.max_rings > 0 && state.rings_remaining == 0 &&
            !state.perfect_phase_triggered) {
            state.perfect_phase = 1;
            state.perfect_timer = 0.0f;
            state.perfect_phase_triggered = true;
        }

        if (state.perfect_phase > 0) {
            state.perfect_timer += (float)FIXED_DT;
            const float SLIDE = 0.33f, HOLD = 2.5f;
            if      (state.perfect_timer < SLIDE)          state.perfect_phase = 1;
            else if (state.perfect_timer < SLIDE + HOLD)   state.perfect_phase = 2;
            else if (state.perfect_timer < 2*SLIDE + HOLD) state.perfect_phase = 3;
            else                                            state.perfect_phase = 0;
        }

        if (state.won) {
            state.win_lift += (1.0f + state.win_lift * 1.5f) * (float)FIXED_DT;
            state.ring_spin += 12.566f * (float)FIXED_DT;        // keep rings spinning
            if (state.ring_spin > 6.2831853f) state.ring_spin -= 6.2831853f;
            float step = state.speed * (float)FIXED_DT;
            advance(step);
            state.run_tick++;
            if (state.run_tick >= RUN_FRAME_TICKS[state.run_cycle_idx]) {
                state.run_tick = 0;
                state.run_cycle_idx = (state.run_cycle_idx + 1) % RUN_CYCLE_LEN;
            }
            state.player_frame = RUN_FRAMES[state.run_cycle_idx];
            if (state.win_lift >= 12.0f) {
                if (state.current_level + 1 >= NUM_LEVELS) {
                    state.wbk_timer += (float)FIXED_DT;
                    if (state.wbk_timer >= 0.7f) {
                        state.congrats = true;
                        state.fade_in_timer = 0.0f;
                    }
                } else {
                    reset_game(state.current_level + 1);
                    state.fade_in_timer = 0.0f;
                }
            }
            state.jump_queued = false;
            continue;
        }

        if (state.game_over) {
            if (state.game_over_spinning) {
                state.game_over_timer += (float)FIXED_DT;
                state.game_over_spin_speed += 6.2831853f * (float)FIXED_DT;
                state.target_angle += state.game_over_spin_speed * (float)FIXED_DT;
                state.vis_angle = state.target_angle;
                state.run_tick++;
                if (state.run_tick >= RUN_FRAME_TICKS[state.run_cycle_idx]) {
                    state.run_tick = 0;
                    state.run_cycle_idx = (state.run_cycle_idx + 1) % RUN_CYCLE_LEN;
                }
                state.player_frame = RUN_FRAMES[state.run_cycle_idx];
                if (state.game_over_timer >= 2.0f) {
                    reset_game(state.current_level);
                    state.fade_in_timer = 0.0f;
                }
            }
            state.jump_queued = false;
            continue;
        }

        state.fade_in_timer += (float)FIXED_DT;
        state.stage_time += (float)FIXED_DT;
        state.ring_spin += 12.566f * (float)FIXED_DT;
        if (state.ring_spin > 6.2831853f) state.ring_spin -= 6.2831853f;

        int tier = (int)(state.stage_time / SPEEDUP_PERIOD);
        if (tier > 4) tier = 4;
        state.speed = BASE_SPEED * (1.0f + 0.25f * (float)tier);

        // --- acceleration ramp (S3&K: velocity moves $200/frame toward rate);
        //     pressing up while rebounding decelerates to 0, then flips forward
        bool want_flip = (state.move_sign < 0 && state.forward_queued && !state.jumping);
        float target = want_flip ? 0.0f : state.speed;
        float dv = ACCEL * (float)FIXED_DT;
        if      (state.cur_speed < target - dv) state.cur_speed += dv;
        else if (state.cur_speed > target + dv) state.cur_speed -= dv;
        else                                    state.cur_speed = target;
        if (want_flip && state.cur_speed <= 0.0f) {
            state.move_sign = 1;
            state.forward_queued = false;
            state.backward_travel = 0.0f;
        }
        float step = state.cur_speed * (float)FIXED_DT;

        // --- S3&K input is level-based: a held direction re-arms at every node
        if (state.frac > 0.25f && state.frac < 0.75f) state.turn_lock = false;
        if (state.held_turn != 0 && !state.turn_lock &&
            state.pending_turn == 0 && !state.turning) {
            state.pending_turn = state.held_turn;
            state.turn_air_queued = state.jumping || state.launching;
        }

        // Yellow launchpad arc
        if (state.launching) {
            state.launch_remaining -= step;
            float t = 1.0f - (state.launch_remaining / state.launch_total);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            state.height = JUMP_HEIGHT * 1.8f * (1.0f - (2.0f*t - 1.0f)*(2.0f*t - 1.0f));
            float adv = step;
            while (adv > 0.0f && !state.game_over) {
                float room = 1.0f - state.frac;
                if (adv < room) { state.frac += adv; adv = 0.0f; }
                else {
                    state.node_x = gwrap(state.node_x + DIR_DX[state.dir]);
                    state.node_y = gwrap(state.node_y + DIR_DY[state.dir]);
                    state.frac = 0.0f; adv -= room;
                }
            }
            if (state.launch_remaining <= 0.0f) {
                state.launching = false; state.height = 0.0f;
                float d = (state.frac > 0.5f) ? (1.0f - state.frac) : state.frac;
                int lx = state.node_x, ly = state.node_y;
                if (state.frac > 0.5f) { lx = gwrap(lx + DIR_DX[state.dir]); ly = gwrap(ly + DIR_DY[state.dir]); }
                state.land_offcenter = d;
                touch_node(lx, ly);
                state.land_offcenter = 0.0f;
            }
            float lt = 1.0f - (state.launch_remaining / state.launch_total);
            lt = lt < 0.0f ? 0.0f : (lt > 1.0f ? 1.0f : lt);
            state.player_frame = SONIC_RUN_FRAMES + (int)(lt * (SONIC_JUMP_FRAMES - 1));
            state.jump_queued = false;
            continue;
        }

        if (state.jump_queued && !state.jumping && !state.launching) {
            state.jumping = true;
            state.jump_total = JUMP_DISTANCE;
            state.jump_remaining = JUMP_DISTANCE;
            state.pending_turn = 0;
            state.turn_air_queued = false;
            state.jump_queued = false;
        }

        bool was_jumping = state.jumping;
        if (state.jumping) {
            state.jump_remaining -= step;
            float dj = (state.jump_total - state.jump_remaining) / state.jump_total;
            dj = dj < 0.0f ? 0.0f : (dj > 1.0f ? 1.0f : dj);
            float arc = 1.0f - (2.0f*dj - 1.0f)*(2.0f*dj - 1.0f);
            state.height = arc * JUMP_HEIGHT;
            if (state.jump_remaining <= 0.0f) {
                state.jumping = false;
                state.height = 0.0f;
                float d = (state.frac > 0.5f) ? (1.0f - state.frac) : state.frac; // dist to nearest node
                int lx = state.node_x, ly = state.node_y;
                if (state.frac > 0.5f) { lx = gwrap(lx + DIR_DX[state.dir]); ly = gwrap(ly + DIR_DY[state.dir]); }
                state.land_offcenter = d;          // new state field, read by touch_node
                touch_node(lx, ly);
                state.land_offcenter = 0.0f;
            }
        }
        if (!state.turning || was_jumping) advance(step);

        float diff = state.target_angle - state.vis_angle;
        float maxstep = state.turn_speed * (float)FIXED_DT;
        if (diff >  maxstep) diff =  maxstep;
        if (diff < -maxstep) diff = -maxstep;
        state.vis_angle += diff;
        if (state.turning && fabsf(state.target_angle - state.vis_angle) < 1e-4f) {
            state.vis_angle = state.target_angle; state.turning = false;
        }

        if (!state.jumping) {
            state.run_tick++;
            if (state.run_tick >= RUN_FRAME_TICKS[state.run_cycle_idx]) {
                state.run_tick = 0;
                state.run_cycle_idx = (state.run_cycle_idx + 1) % RUN_CYCLE_LEN;
            }
            state.player_frame = RUN_FRAMES[state.run_cycle_idx];
        }
        if (state.jumping) {
            #define JUMP_FRAME_TICKS 4   // 120 ticks/s -> ~10 frames per spin cycle
            state.run_tick++;
            if (state.run_tick >= JUMP_FRAME_TICKS) {
                state.run_tick = 0;
                state.player_frame++;
                if (state.player_frame <  SONIC_RUN_FRAMES ||
                    state.player_frame >= SONIC_RUN_FRAMES + SONIC_JUMP_FRAMES)
                    state.player_frame = SONIC_RUN_FRAMES;
            }
        }
    }

    float pos_x = (float)state.node_x + (float)DIR_DX[state.dir] * state.frac;
    float pos_y = (float)state.node_y + (float)DIR_DY[state.dir] * state.frac;
    fs_params_t fsp = {
        .aspect = sapp_widthf() / sapp_heightf(),
        .scroll = { pos_x, pos_y }, .tile_size = 1.0f, .rot = state.vis_angle };
    float aspect = sapp_widthf() / sapp_heightf();
    int ring_frame = (int)(state.ring_spin / 6.2831853f * RING_TEX_FRAMES) % RING_TEX_FRAMES;
    if (ring_frame < 0) ring_frame += RING_TEX_FRAMES;

    struct bd { float cx, cy, hx, hy, depth, r, g, b, star, spin, wdx, wdy;
                float tc[3], ta[3]; };
    struct bd draws[MAX_VISIBLE_SPHERES];
    int ndraw = 0;
    float ring_cx[MAX_VISIBLE_SPHERES], ring_cy[MAX_VISIBLE_SPHERES];
    float ring_hx[MAX_VISIBLE_SPHERES], ring_hy[MAX_VISIBLE_SPHERES];
    float ring_dep[MAX_VISIBLE_SPHERES];
    int   ring_n = 0;
    for (int i = 0; i < state.sphere_count && ndraw < MAX_VISIBLE_SPHERES; i++) {
        sphere_t* s = &state.spheres[i];
        if (!s->active) continue;
        if (s->type == SPH_RING) {
            // project like a sphere, store for the sprite pass
            float dxr = gwrap_deltaf(pos_x, s->x), dyr = gwrap_deltaf(pos_y, s->y);
            float d2r = dxr*dxr + dyr*dyr;
            if (d2r > (float)(VISIBLE_RANGE*VISIBLE_RANGE)) continue;
            float sxr = pos_x + dxr, syr = pos_y + dyr;
            float cr[3], nr[3];
            ball_center_and_normal(sxr, syr, pos_x, pos_y, state.vis_angle, cr, nr);
            if (state.won && state.win_lift > 0.0f) {
                cr[0] += nr[0] * state.win_lift;
                cr[1] += nr[1] * state.win_lift;
                cr[2] += nr[2] * state.win_lift;
            }
            float rcx, rcy, rhx, rhy, rdepth;
            if (!project_ball(cr, aspect, &rcx, &rcy, &rhx, &rhy, &rdepth)) continue;
            float distr = sqrtf(d2r);
            float scl = 1.0f - fmaxf(0.0f, (distr - 4.0f) / (float)(VISIBLE_RANGE - 4));
            scl = scl * scl;
            if (ring_n < MAX_VISIBLE_SPHERES) {
                ring_cx[ring_n] = rcx; ring_cy[ring_n] = rcy;
                ring_hx[ring_n] = rhx * scl * .75f;   // size tweak
                ring_hy[ring_n] = rhy * scl * .75f;
                ring_dep[ring_n] = rdepth;
                ring_n++;
            }
            continue;
        }
        float dx = gwrap_deltaf(pos_x, s->x), dy = gwrap_deltaf(pos_y, s->y);
        float dist2 = dx*dx + dy*dy;
        if (dist2 > (float)(VISIBLE_RANGE*VISIBLE_RANGE)) continue;
        float sx = pos_x + dx, sy = pos_y + dy;
        float center[3], normal[3];
        ball_center_and_normal(sx, sy, pos_x, pos_y, state.vis_angle, center, normal);
        if (state.won && state.win_lift > 0.0f) {
            center[0] += normal[0] * state.win_lift;
            center[1] += normal[1] * state.win_lift;
            center[2] += normal[2] * state.win_lift;
        }
        float cx, cy, hx, hy, depth;
        if (!project_ball(center, aspect, &cx, &cy, &hx, &hy, &depth)) continue;
        float dist = sqrtf(dist2);
        float scale = 1.0f - fmaxf(0.0f, (dist - 4.0f) / (float)(VISIBLE_RANGE - 4));
        scale = scale * scale;
        hx *= scale; hy *= scale;
        float bhx = hx, bhy = hy;
        if (s->type == SPH_RING) { bhx *= 1.0f; bhy *= 1.0f; }
        if (bhx < 1e-4f) continue;
        struct bd* d = &draws[ndraw++];
        d->cx=cx; d->cy=cy; d->hx=bhx; d->hy=bhy; d->depth=depth;
        d->star=0.0f; d->spin=0.0f; d->wdx=dx; d->wdy=dy;
        d->tc[0]=center[0]; d->tc[1]=center[1]; d->tc[2]=center[2];
        d->ta[0]=normal[0]; d->ta[1]=normal[1]; d->ta[2]=normal[2];
        if      (s->type == SPH_RED)    { d->r=0.95f; d->g=0.15f; d->b=0.15f; }
        else if (s->type == SPH_RING)   { d->r=1.00f; d->g=0.84f; d->b=0.10f; d->star=2.0f; d->spin=state.ring_spin; }
        else if (s->type == SPH_STAR)   { d->r=0.92f; d->g=0.92f; d->b=0.95f; d->star=1.0f; }
        else if (s->type == SPH_YELLOW) { d->r=1.00f; d->g=0.85f; d->b=0.00f; }
        else                            { d->r=0.15f; d->g=0.35f; d->b=0.95f; }
    }
    for (int a=0;a<ndraw;a++) for (int b=a+1;b<ndraw;b++)
        if (draws[b].depth > draws[a].depth) { struct bd tmp=draws[a]; draws[a]=draws[b]; draws[b]=tmp; }

    // -------------------------------------------------------------------------
    // CONGRATULATIONS screen
    // -------------------------------------------------------------------------
    if (state.congrats) {
        sg_pass_action black = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                           .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } } };
        sg_pass cp = state.crt_enabled
            ? (sg_pass){ .action = black,
                         .attachments.colors[0]    = state.crt_att_view,
                         .attachments.depth_stencil = state.crt_depth_att_view }
            : (sg_pass){ .action = black, .swapchain = sglue_swapchain() };
        sg_begin_pass(&cp);

        {
            const float gw = 0.48f, gh = 0.44f;
            sg_apply_pipeline(state.gem_pip);
            sg_apply_bindings(&state.gem_bind);
            gem_vs_t gvs = { .center = { 0.0f, -0.02f }, .halfsize = { gw, gh } };
            gem_fs_t gfs = { .spin = state.emerald_spin };
            sg_apply_uniforms(UB_gem_vs, &SG_RANGE(gvs));
            sg_apply_uniforms(UB_gem_fs, &SG_RANGE(gfs));
            sg_draw(0, 6, 1);
        }

        const float cw     = (float)CONG_PX * 3.0f / sapp_widthf();
        const float ch     = (float)CONG_TEX_H * 3.0f / sapp_heightf();
        const float cy_pos = 1.0f - 0.05f - ch;
        sg_apply_pipeline(state.hud_pip);
        sg_apply_bindings(&state.congrats_bind);
        hud_vs_t tv = { .pos  = { -cw * 0.5f, cy_pos },
                        .size = { cw, ch },
                        .uv0  = { 0.0f, 0.0f },
                        .uv1  = { (float)CONG_PX / CONG_TEX_W, 1.0f } };
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(tv));
        sg_draw(0, 6, 1);

        if (state.fade_in_timer < 1.0f) {
            float alpha = 1.0f - state.fade_in_timer;
            fade_fs_t ffs = { .alpha = alpha, .r = 0.0f, .g = 0.0f, .b = 0.0f };
            sg_apply_pipeline(state.fade_pip);
            sg_apply_bindings(&state.fade_bind);
            sg_apply_uniforms(UB_fade_fs, &SG_RANGE(ffs));
            sg_draw(0, 3, 1);
        }

        sg_end_pass();
        if (state.crt_enabled) {
            sg_begin_pass(&(sg_pass){ .action = state.crt_pass_action,
                                      .swapchain = sglue_swapchain() });
            sg_apply_pipeline(state.crt_pip);
            sg_apply_bindings(&state.crt_bind);
            crt_params_t cps = { .screen_h = (float)sapp_height() };
            sg_apply_uniforms(UB_crt_params, &SG_RANGE(cps));
            sg_draw(0, 3, 1);
            sg_end_pass();
        }
        sg_commit();
        return;
    }

    sg_pass game_pass = state.crt_enabled
        ? (sg_pass){ .action = state.pass_action,
                     .attachments.colors[0]    = state.crt_att_view,
                     .attachments.depth_stencil = state.crt_depth_att_view }
        : (sg_pass){ .action = state.pass_action, .swapchain = sglue_swapchain() };
    sg_begin_pass(&game_pass);
    sg_apply_pipeline(state.pip); sg_apply_bindings(&state.bind);
    sg_apply_uniforms(UB_fs_params, &SG_RANGE(fsp)); sg_draw(0, 3, 1);

    sg_apply_pipeline(state.ball_pip); sg_apply_bindings(&state.ball_bind);
    for (int i = 0; i < ndraw; i++) {
        ball_vs_t bvs = { .center   = { draws[i].cx, draws[i].cy },
                          .halfsize = { draws[i].hx, draws[i].hy } };
        ball_fs_t bfs = { .color = { draws[i].r, draws[i].g, draws[i].b, draws[i].star },
                          .spin = draws[i].spin, .tilt = 0.0f,
                          .tc = { draws[i].tc[0], draws[i].tc[1], draws[i].tc[2], aspect },
                          .ta = { draws[i].ta[0], draws[i].ta[1], draws[i].ta[2], 0.0f } };
        sg_apply_uniforms(UB_ball_vs, &SG_RANGE(bvs));
        sg_apply_uniforms(UB_ball_fs, &SG_RANGE(bfs));
        sg_draw(0, 6, 1);
    }

    sg_apply_pipeline(state.ring_pip);
    sg_apply_bindings(&state.ring_bind);
    for (int i = 0; i < ring_n; i++) {
        player_vs_t rvs = { .center = { ring_cx[i], ring_cy[i] },
                            .halfsize = { ring_hx[i], ring_hy[i] } };
        player_fs_t rfs = { .frame = (float)ring_frame,
                            .total_frames = (float)RING_TEX_FRAMES };
        sg_apply_uniforms(UB_player_vs, &SG_RANGE(rvs));
        sg_apply_uniforms(UB_player_fs, &SG_RANGE(rfs));
        sg_draw(0, 6, 1);
    }

    float jump_rise = state.height * 1.2f;
    float sprite_aspect = (float)SONIC_FRAME_W / (float)SONIC_TEX_HEIGHT;
    float sprite_h = 0.22f;
    float sprite_w = sprite_h * sprite_aspect / aspect;
    player_vs_t pvs = { .center   = { 0.0f, -0.30f + jump_rise },
                        .halfsize = { sprite_w, sprite_h } };
    player_fs_t pfs = { .frame = (float)state.player_frame,
                        .total_frames = (float)SONIC_TEX_FRAMES };
    sg_apply_pipeline(state.player_pip); sg_apply_bindings(&state.player_bind);
    sg_apply_uniforms(UB_player_vs, &SG_RANGE(pvs));
    sg_apply_uniforms(UB_player_fs, &SG_RANGE(pfs));
    sg_draw(0, 6, 1);

    sg_apply_pipeline(state.hud_pip);
    sg_apply_bindings(&state.hud_bind);

    float sw = sapp_widthf(), sh = sapp_heightf();
    float gpx = 2.0f / sw, gpy = 2.0f / sh;
    float scale = 3.0f;
    float gw = HUD_GLYPH_W * gpx * scale;
    float gh = HUD_GLYPH_H * gpy * scale;
    float pad = 4.0f * gpx * scale;
    float margin_x = 0.04f, margin_y = 0.04f;
    float top = 1.0f - margin_y - gh;

    #define DRAW_GLYPH(glyph_idx, clip_x, clip_y) do { \
        float _gu  = (float)(glyph_idx) * (float)HUD_GLYPH_W / (float)HUD_ATLAS_W; \
        float _guw = (float)HUD_GLYPH_W / (float)HUD_ATLAS_W; \
        hud_vs_t _h = { .pos  = { (clip_x), (clip_y) }, .size = { gw, gh }, \
                        .uv0  = { _gu, 1.0f }, .uv1  = { _gu+_guw, 0.0f } }; \
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(_h)); sg_draw(0, 6, 1); \
    } while(0)

    #define DRAW_DIGITS(num, start_x, out_x) do { \
        int _n = (num) < 999 ? (num) : 999; float _x = (start_x); \
        if (_n >= 100) { DRAW_GLYPH(_n/100,     _x, top); _x += gw + gpx; } \
        if (_n >= 10)  { DRAW_GLYPH((_n/10)%10, _x, top); _x += gw + gpx; } \
        DRAW_GLYPH(_n%10, _x, top); _x += gw + gpx; (out_x) = _x; \
    } while(0)

    float icon_gap = gw * 0.5f;
    { float x = -1.0f + margin_x + pad, end_x;
      DRAW_DIGITS(state.blue_remaining, x, end_x);
      DRAW_GLYPH(HUD_GLYPH_SPHERE, end_x + icon_gap, top); }
    { int r = state.rings_remaining;
      int nd = r >= 100 ? 3 : (r >= 10 ? 2 : 1);
      float total_w = gw + icon_gap + nd * (gw + gpx);
      float x = 1.0f - margin_x - total_w, end_x;
      DRAW_GLYPH(HUD_GLYPH_RING, x, top); x += gw + icon_gap;
      DRAW_DIGITS(r, x, end_x); }

    {
        float alpha = 0.0f, fr = 0.0f, fg = 0.0f, fb = 0.0f;
        if (state.game_over && state.game_over_spinning)
            alpha = fminf(state.game_over_timer / 2.0f, 1.0f);
        else if (state.won && state.win_lift > 3.0f && state.wbk_timer <= 0.0f)
            { alpha = fminf((state.win_lift - 3.0f) / 6.0f, 1.0f); fr = fg = fb = 1.0f; }
        else if (state.won && state.wbk_timer > 0.0f) {
            float t = fminf(state.wbk_timer / 0.7f, 1.0f);
            alpha = 1.0f; fr = fg = fb = 1.0f - t;
        }
        else if (state.fade_in_timer < 1.0f)
            alpha = 1.0f - state.fade_in_timer;
        if (alpha > 0.001f) {
            fade_fs_t ffs = { .alpha = alpha, .r = fr, .g = fg, .b = fb };
            sg_apply_pipeline(state.fade_pip);
            sg_apply_bindings(&state.fade_bind);
            sg_apply_uniforms(UB_fade_fs, &SG_RANGE(ffs));
            sg_draw(0, 3, 1);
        }
    }

    if (state.perfect_phase > 0) {
        const float SLIDE = 0.33f, HOLD = 2.5f;
        float t = state.perfect_timer;
        float offset;
        if      (state.perfect_phase == 1) offset = 1.05f * (1.0f - t / SLIDE);
        else if (state.perfect_phase == 3) offset = 1.05f * ((t - SLIDE - HOLD) / SLIDE);
        else                               offset = 0.0f;
        offset = fmaxf(0.0f, fminf(offset, 1.05f));
        const float pw = 168.0f / 400.0f, ew = 126.0f / 400.0f;
        const float ht =  60.0f / 300.0f, by = 0.0f;
        const float su = (float)PERF_SPLIT / PERF_TEX_W;
        const float eu = (float)PERF_END   / PERF_TEX_W;
        sg_apply_pipeline(state.hud_pip);
        sg_apply_bindings(&state.perfect_bind);
        hud_vs_t lv = { .pos={-offset-pw,by}, .size={pw,ht}, .uv0={0.0f,0.0f}, .uv1={su,1.0f} };
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(lv)); sg_draw(0, 6, 1);
        hud_vs_t rv = { .pos={offset,by}, .size={ew,ht}, .uv0={su,0.0f}, .uv1={eu,1.0f} };
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(rv)); sg_draw(0, 6, 1);
    }

    // --- debug overlay (remove when done) -----------------------------------
    if (state.dbg_show) {
        static uint8_t dbg_pixels[DBG_TEX_W * DBG_TEX_H * 4];
        static const char* dir_names[4] = { "N+Y", "E+X", "S-Y", "W-X" };
        char line1[128], line2[128];

        // Line 1: LVL=N  POS=(XX,YY)  DIR=E+X  BLUE=NNN
        snprintf(line1, sizeof(line1), "LVL=%d POS=(%d,%d) DIR=%s BLUE=%d",
                 state.current_level + 1,
                 state.node_x, state.node_y,
                 dir_names[state.dir],
                 state.blue_remaining);

        // Line 2: ANG=X.XXX  TGTANG=X.XXX  FRAC=XX%  RINGS=NNN
        snprintf(line2, sizeof(line2), "ANG=%.3f FRAC=%d%% SPD=%.2f TIER=%d TIME=%d",
                 state.vis_angle,
                 (int)(state.frac * 100.0f),
                 state.speed,
                 (int)(state.stage_time / SPEEDUP_PERIOD) > 4 ? 4 : (int)(state.stage_time / SPEEDUP_PERIOD),
                 (int)state.stage_time);

        build_debug_texture(dbg_pixels, line1, line2);
        sg_update_image(state.dbg_img, &(sg_image_data){
            .mip_levels[0] = { .ptr  = dbg_pixels,
                               .size = sizeof(dbg_pixels) } });

        // Anchor to bottom-left, full texture width, two lines tall
        const float dw = 2.0f * DBG_TEX_W / sapp_widthf();
        const float dh = 2.0f * DBG_TEX_H / sapp_heightf();
        hud_vs_t dv = { .pos  = { -1.0f, -1.0f },
                        .size = { dw, dh },
                        .uv0  = { 0.0f, 1.0f },
                        .uv1  = { 1.0f, 0.0f } };
        sg_apply_pipeline(state.hud_pip);
        sg_apply_bindings(&state.dbg_bind);
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(dv));
        sg_draw(0, 6, 1);
    }
    // --- end debug overlay --------------------------------------------------

    sg_end_pass();

    if (state.crt_enabled) {
        sg_begin_pass(&(sg_pass){ .action = state.crt_pass_action,
                                  .swapchain = sglue_swapchain() });
        sg_apply_pipeline(state.crt_pip);
        sg_apply_bindings(&state.crt_bind);
        crt_params_t cps = { .screen_h = (float)sapp_height() };
        sg_apply_uniforms(UB_crt_params, &SG_RANGE(cps));
        sg_draw(0, 3, 1);
        sg_end_pass();
    }

    sg_commit();
}

static void event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_UP) {
        switch (e->key_code) {
            case SAPP_KEYCODE_LEFT: case SAPP_KEYCODE_A:
                state.left_down = false;
                state.held_turn = state.right_down ? +1 : 0;
                break;
            case SAPP_KEYCODE_RIGHT: case SAPP_KEYCODE_D:
                state.right_down = false;
                state.held_turn = state.left_down ? -1 : 0;
                break;
            case SAPP_KEYCODE_SPACE: case SAPP_KEYCODE_Z: case SAPP_KEYCODE_X:
                state.jump_down = false;
                break;
            default: break;
        }
        return;
    }
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat) {
        switch (e->key_code) {
            case SAPP_KEYCODE_LEFT:  case SAPP_KEYCODE_A:
                state.left_down = true;
                state.held_turn = -1;
                if (!state.game_over && !state.won) {
                    if (!state.started) {
                        state.dir = (state.dir + 3) & 3;
                        state.target_angle -= 1.5707963f;
                        state.turning = true;
                    } else if (state.pending_turn == 0 && !state.turning &&
                               !state.turn_lock) {
                        state.pending_turn = -1;
                        state.turn_air_queued = state.jumping || state.launching;
                    }
                }
                break;
            case SAPP_KEYCODE_RIGHT: case SAPP_KEYCODE_D:
                state.right_down = true;
                state.held_turn = +1;
                if (!state.game_over && !state.won) {
                    if (!state.started) {
                        state.dir = (state.dir + 1) & 3;
                        state.target_angle += 1.5707963f;
                        state.turning = true;
                    } else if (state.pending_turn == 0 && !state.turning &&
                               !state.turn_lock) {
                        state.pending_turn = +1;
                        state.turn_air_queued = state.jumping || state.launching;
                    }
                }
                break;
            case SAPP_KEYCODE_UP: case SAPP_KEYCODE_W:
                if (!state.game_over && !state.won) {
                    if (!state.started) state.started = true;
                    else if (state.move_sign < 0) state.forward_queued = true;
                }
                break;
            case SAPP_KEYCODE_SPACE: case SAPP_KEYCODE_Z: case SAPP_KEYCODE_X:
                if (state.congrats && state.congrats_timer >= 5.0f)
                    { reset_game(0); state.fade_in_timer = 0.0f; }
                else if (state.game_over)
                    { reset_game(state.current_level); state.fade_in_timer = 0.0f; }
                else if (state.won && state.win_lift >= 9.0f)
                    { reset_game(state.current_level); state.fade_in_timer = 0.0f; }
                else if (!state.won && !state.congrats) {
                    if (!state.jump_down) { state.jump_queued = true; }
                    state.jump_down = true;
                }
                break;
            case SAPP_KEYCODE_ENTER:
                if (state.started && !state.game_over && !state.won)
                    state.paused = !state.paused;
                break;
            case SAPP_KEYCODE_ESCAPE: sapp_request_quit(); break;
            case SAPP_KEYCODE_S: state.crt_enabled = !state.crt_enabled; break;
            case SAPP_KEYCODE_F1: state.dbg_show = !state.dbg_show; break;
            default: break;
        }
    }
}

static void cleanup(void) { sg_shutdown(); }

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    return (sapp_desc){
        .init_cb = init, .frame_cb = frame, .event_cb = event,
        .cleanup_cb = cleanup, .width = 800, .height = 600,
        .window_title = "Blue Spheres", .icon.sokol_default = true,
        .logger.func = slog_func };
}