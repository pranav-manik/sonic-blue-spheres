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
//------------------------------------------------------------------------------
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "sphere.glsl.h"
#include "player.glsl.h"
#include "ball.glsl.h"
#include "hud.glsl.h"
#include "fade.glsl.h"
#include "crt.glsl.h"
#include "gem.glsl.h"
#include "hud_atlas.h"   // 4x5 bitmap digit atlas

#include "sonic_tex.h"   // embedded sprite sheet (RGBA pixel data)
#include "levels.h"

// minimal mat4 type so the generated @ctype resolves (shader uses no mat4)
typedef struct { float m[16]; } hmm_mat4;

// the four facing directions as integer grid steps, ordered so that
// turning right = (dir+1)%4 and turning left = (dir+3)%4.
//   0 = +Y (north)   1 = +X (east)   2 = -Y (south)   3 = -X (west)
static const int DIR_DX[4] = {  0,  1,  0, -1 };
static const int DIR_DY[4] = {  1,  0, -1,  0 };

// --- spheres -----------------------------------------------------------------
typedef enum { SPH_BLUE = 0, SPH_RED = 1, SPH_RING = 2, SPH_STAR = 3 } sphere_type;
typedef struct {
    int x, y;
    sphere_type type;
    bool active;
} sphere_t;

#define MAX_LEVEL_SPHERES 600
#define MAX_VISIBLE_SPHERES 256
#define VISIBLE_RANGE 8
#define GRID_SIZE 32

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

// Sonic 3 Special Stage 1 — decoded from s3stage1.bssj
// 102 blue spheres, 384 pre-placed red spheres, 66 bumpers = 552 total
// Start: node_x=2, node_y=2, facing east (dir=1)
static const int LEVEL_LAYOUT[][3] = {
    {0,0,1},{1,0,1},{2,0,1},{3,0,1},{4,0,1},{5,0,1},{6,0,1},{7,0,1},{8,0,1},{9,0,1},{10,0,1},{11,0,1},{12,0,1},{13,0,1},{14,0,1},{15,0,1},{16,0,1},{17,0,1},{18,0,1},{19,0,1},{20,0,1},{21,0,1},{22,0,1},{23,0,1},{24,0,1},{25,0,1},{26,0,1},{27,0,1},{28,0,1},{29,0,1},{30,0,1},{31,0,1},
    {0,1,1},{1,1,1},{2,1,1},{3,1,1},{4,1,1},{5,1,1},{6,1,1},{7,1,1},{8,1,1},{9,1,1},{10,1,1},{11,1,1},{12,1,1},{13,1,1},{14,1,1},{15,1,1},{16,1,1},{17,1,1},{18,1,1},{19,1,1},{20,1,1},{21,1,1},{22,1,1},{23,1,1},{24,1,1},{25,1,1},{26,1,1},{27,1,1},{28,1,1},{29,1,1},{30,1,1},{31,1,1},
    {0,2,1},{1,2,1},{15,2,3},{16,2,3},{30,2,1},{31,2,1},
    {0,3,1},{1,3,1},{15,3,3},{16,3,3},{30,3,1},{31,3,1},
    {0,4,1},{1,4,1},{4,4,0},{5,4,0},{6,4,0},{7,4,0},{24,4,0},{25,4,0},{26,4,0},{27,4,0},{30,4,1},{31,4,1},
    {0,5,1},{1,5,1},{4,5,0},{5,5,0},{6,5,0},{7,5,0},{9,5,0},{10,5,0},{15,5,3},{16,5,3},{21,5,0},{22,5,0},{24,5,0},{25,5,0},{26,5,0},{27,5,0},{30,5,1},{31,5,1},
    {0,6,1},{1,6,1},{4,6,0},{5,6,0},{6,6,0},{7,6,0},{9,6,0},{10,6,0},{15,6,3},{16,6,3},{21,6,0},{22,6,0},{24,6,0},{25,6,0},{26,6,0},{27,6,0},{30,6,1},{31,6,1},
    {0,7,1},{1,7,1},{4,7,0},{5,7,0},{6,7,0},{7,7,0},{24,7,0},{25,7,0},{26,7,0},{27,7,0},{30,7,1},{31,7,1},
    {0,8,1},{1,8,1},{15,8,3},{16,8,3},{30,8,1},{31,8,1},
    {0,9,1},{1,9,1},{15,9,3},{16,9,3},{30,9,1},{31,9,1},
    {0,10,1},{1,10,1},{10,10,1},{11,10,1},{12,10,1},{13,10,1},{14,10,1},{15,10,1},{16,10,1},{17,10,1},{18,10,1},{19,10,1},{20,10,1},{21,10,1},{30,10,1},{31,10,1},
    {0,11,1},{1,11,1},{10,11,1},{11,11,1},{12,11,1},{13,11,1},{14,11,1},{15,11,1},{16,11,1},{17,11,1},{18,11,1},{19,11,1},{20,11,1},{21,11,1},{30,11,1},{31,11,1},
    {0,12,1},{1,12,1},{4,12,3},{5,12,0},{6,12,3},{7,12,3},{10,12,1},{11,12,1},{12,12,1},{13,12,1},{14,12,1},{15,12,1},{16,12,1},{17,12,1},{18,12,1},{19,12,1},{20,12,1},{21,12,1},{24,12,3},{25,12,3},{26,12,0},{27,12,3},{30,12,1},{31,12,1},
    {0,13,1},{1,13,1},{4,13,3},{5,13,0},{6,13,0},{7,13,3},{10,13,1},{11,13,1},{12,13,1},{13,13,1},{14,13,1},{15,13,1},{16,13,1},{17,13,1},{18,13,1},{19,13,1},{20,13,1},{21,13,1},{24,13,3},{25,13,0},{26,13,0},{27,13,3},{30,13,1},{31,13,1},
    {0,14,1},{1,14,1},{4,14,3},{5,14,3},{6,14,0},{7,14,3},{10,14,1},{11,14,1},{12,14,1},{13,14,1},{14,14,1},{15,14,1},{16,14,1},{17,14,1},{18,14,1},{19,14,1},{20,14,1},{21,14,1},{24,14,3},{25,14,0},{26,14,3},{27,14,3},{30,14,1},{31,14,1},
    {0,15,1},{1,15,1},{4,15,3},{5,15,0},{6,15,0},{7,15,3},{10,15,1},{11,15,1},{12,15,1},{13,15,1},{14,15,1},{15,15,1},{16,15,1},{17,15,1},{18,15,1},{19,15,1},{20,15,1},{21,15,1},{24,15,3},{25,15,0},{26,15,0},{27,15,3},{30,15,1},{31,15,1},
    {0,16,1},{1,16,1},{4,16,3},{5,16,0},{6,16,3},{7,16,3},{10,16,1},{11,16,1},{12,16,1},{13,16,1},{14,16,1},{15,16,1},{16,16,1},{17,16,1},{18,16,1},{19,16,1},{20,16,1},{21,16,1},{24,16,3},{25,16,3},{26,16,0},{27,16,3},{30,16,1},{31,16,1},
    {0,17,1},{1,17,1},{4,17,3},{5,17,0},{6,17,0},{7,17,3},{10,17,1},{11,17,1},{12,17,1},{13,17,1},{14,17,1},{15,17,1},{16,17,1},{17,17,1},{18,17,1},{19,17,1},{20,17,1},{21,17,1},{24,17,3},{25,17,0},{26,17,0},{27,17,3},{30,17,1},{31,17,1},
    {0,18,1},{1,18,1},{4,18,3},{5,18,3},{6,18,0},{7,18,3},{10,18,1},{11,18,1},{12,18,1},{13,18,1},{14,18,1},{15,18,1},{16,18,1},{17,18,1},{18,18,1},{19,18,1},{20,18,1},{21,18,1},{24,18,3},{25,18,0},{26,18,3},{27,18,3},{30,18,1},{31,18,1},
    {0,19,1},{1,19,1},{4,19,3},{5,19,3},{6,19,0},{7,19,3},{10,19,1},{11,19,1},{12,19,1},{13,19,1},{14,19,1},{15,19,1},{16,19,1},{17,19,1},{18,19,1},{19,19,1},{20,19,1},{21,19,1},{24,19,3},{25,19,0},{26,19,3},{27,19,3},{30,19,1},{31,19,1},
    {0,20,1},{1,20,1},{10,20,1},{11,20,1},{12,20,1},{13,20,1},{14,20,1},{15,20,1},{16,20,1},{17,20,1},{18,20,1},{19,20,1},{20,20,1},{21,20,1},{30,20,1},{31,20,1},
    {0,21,1},{1,21,1},{10,21,1},{11,21,1},{12,21,1},{13,21,1},{14,21,1},{15,21,1},{16,21,1},{17,21,1},{18,21,1},{19,21,1},{20,21,1},{21,21,1},{30,21,1},{31,21,1},
    {0,22,1},{1,22,1},{15,22,3},{16,22,3},{30,22,1},{31,22,1},
    {0,23,1},{1,23,1},{15,23,3},{16,23,3},{30,23,1},{31,23,1},
    {0,24,1},{1,24,1},{4,24,0},{5,24,0},{6,24,0},{7,24,0},{24,24,0},{25,24,0},{26,24,0},{27,24,0},{30,24,1},{31,24,1},
    {0,25,1},{1,25,1},{4,25,0},{5,25,0},{6,25,0},{7,25,0},{9,25,0},{10,25,0},{15,25,3},{16,25,3},{21,25,0},{22,25,0},{24,25,0},{25,25,0},{26,25,0},{27,25,0},{30,25,1},{31,25,1},
    {0,26,1},{1,26,1},{4,26,0},{5,26,0},{6,26,0},{7,26,0},{9,26,0},{10,26,0},{15,26,3},{16,26,3},{21,26,0},{22,26,0},{24,26,0},{25,26,0},{26,26,0},{27,26,0},{30,26,1},{31,26,1},
    {0,27,1},{1,27,1},{4,27,0},{5,27,0},{6,27,0},{7,27,0},{24,27,0},{25,27,0},{26,27,0},{27,27,0},{30,27,1},{31,27,1},
    {0,28,1},{1,28,1},{15,28,3},{16,28,3},{30,28,1},{31,28,1},
    {0,29,1},{1,29,1},{15,29,3},{16,29,3},{30,29,1},{31,29,1},
    {0,30,1},{1,30,1},{2,30,1},{3,30,1},{4,30,1},{5,30,1},{6,30,1},{7,30,1},{8,30,1},{9,30,1},{10,30,1},{11,30,1},{12,30,1},{13,30,1},{14,30,1},{15,30,1},{16,30,1},{17,30,1},{18,30,1},{19,30,1},{20,30,1},{21,30,1},{22,30,1},{23,30,1},{24,30,1},{25,30,1},{26,30,1},{27,30,1},{28,30,1},{29,30,1},{30,30,1},{31,30,1},
    {0,31,1},{1,31,1},{2,31,1},{3,31,1},{4,31,1},{5,31,1},{6,31,1},{7,31,1},{8,31,1},{9,31,1},{10,31,1},{11,31,1},{12,31,1},{13,31,1},{14,31,1},{15,31,1},{16,31,1},{17,31,1},{18,31,1},{19,31,1},{20,31,1},{21,31,1},{22,31,1},{23,31,1},{24,31,1},{25,31,1},{26,31,1},{27,31,1},{28,31,1},{29,31,1},{30,31,1},{31,31,1},
};
#define LEVEL_LAYOUT_COUNT ((int)(sizeof(LEVEL_LAYOUT)/sizeof(LEVEL_LAYOUT[0])))

#define G_GR     12.5f
#define G_GCx    0.0f
#define G_GCy    0.0f
#define G_GCz   -12.5f
#define G_PIVOTx 0.0f
#define G_PIVOTy 1.223f
#define BALL_RADIUS_C 0.22f

#define JUMP_DISTANCE   2.0f
#define JUMP_HEIGHT     0.5f
#define JUMP_COLLIDE_H  0.2f

static bool project_ball(const float center[3], float aspect,
                         float* cx, float* cy, float* hx, float* hy, float* depth) {
    float camx = 0.0f, camy = 1.1f, camz = 1.6f;
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

// Same as ball_center but also returns the surface normal (torus axis for rings).
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
    out[0] = sx - nx * BALL_RADIUS_C * 0.3f;
    out[1] = sy - ny * BALL_RADIUS_C * 0.3f;
    out[2] = sz - nz * BALL_RADIUS_C * 0.3f;
    nout[0] = nx; nout[1] = ny; nout[2] = nz;
}

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
    int         perfect_phase;  // 0=off, 1=slide_in, 2=hold, 3=slide_out
    float       perfect_timer;
    sg_image    perfect_img;
    sg_bindings perfect_bind;
    bool        perfect_phase_triggered; // only fires once per level

    // Congratulations screen (shown after last level)
    bool        congrats;
    float       congrats_timer;   // seconds since congrats started (5s skip-lock)
    float       emerald_spin;     // Y-rotation angle for the spinning emerald
    float       wbk_timer;        // white→black fade timer before congrats
    sg_image    congrats_img;
    sg_bindings congrats_bind;
    sg_pipeline gem_pip;
    sg_bindings gem_bind;

    // CRT post-process ('S' to toggle)
    sg_image        crt_img;
    sg_view         crt_att_view;
    sg_view         crt_depth_att_view;
    sg_pipeline     crt_pip;
    sg_bindings     crt_bind;
    sg_pass_action  crt_pass_action;
    bool            crt_enabled;

    int   node_x, node_y;
    float frac;
    int   dir;
    int   pending_turn;
    float speed;
    float stage_time;

    int   move_sign;
    float bounce_dist;
    float backward_travel;
    bool  forward_queued;
    int   last_star_x, last_star_y;

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
    state.turn_speed = 1.5707963f / 0.18f;
    state.turning = false; state.accum = 0.0;
    state.jumping = false; state.jump_total = 0.0f;
    state.jump_remaining = 0.0f; state.height = 0.0f;
    state.jump_queued = false;
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
}

// ---------------------------------------------------------------------------
//  PERFECT notification texture (128×20 RGBA8)
//
//  Layout: letters P E R F | E C T
//   PERF  → x=[0..55]  (56 px, split_u = 56/128)
//   ECT   → x=[56..97] (42 px, end_u  = 98/128)
//
//  Slots 4-6 start at 56/70/84 (shifted +2px vs old 54/68/82) so the
//  visual F→E gap matches every other adjacent-letter gap.
// ---------------------------------------------------------------------------
#define PERF_TEX_W  128
#define PERF_TEX_H  20
#define PERF_SPLIT  56   // x pixel where PERF ends / ECT begins
#define PERF_END    98   // x pixel where ECT ends

static const int perf_slots[7] = { 0, 14, 28, 42, 56, 70, 84 };

// 5-col × 7-row glyph bitmaps for P E R F E C T (rows ordered top→bottom)
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

    // Pass 1: white→light-grey top-to-bottom gradient, 2× scaled
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

    // Pass 2: dark-grey 8-connected outline
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
//  CONGRATULATIONS screen texture (256×20 RGBA8)
//
//  15 chars × 14 px/slot = 210 px, starting at x=0 in the texture.
//  Same reversed-row convention as PERFECT (row 0 = visual bottom).
//  Unique glyphs indexed: C=0 O=1 N=2 G=3 R=4 A=5 T=6 U=7 L=8 I=9 S=10
// ---------------------------------------------------------------------------
#define CONG_GLYPH_W  7
#define CONG_GLYPH_H  9
#define CONG_SLOT_W   16   // wider slots: 7*2 + 2px gap
#define CONG_TEX_W   256   // stays the same, 15*16=240 fits fine
#define CONG_TEX_H    24   // taller: 9*2 + 2px padding top+bottom
#define CONG_LEN    15
#define CONG_PX  240   // 15 * 16


static const int cong_seq[CONG_LEN] = {0,1,2,3,4,5,6,7,8,5,6,9,1,2,10};

static const uint8_t cong_glyphs[11][9][7] = {
    // C
    {{0,0,1,1,1,0,0},
     {0,1,0,0,0,1,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {0,1,0,0,0,1,0},
     {0,0,1,1,1,0,0}},
    // O
    {{0,0,1,1,1,0,0},
     {0,1,0,0,0,1,0},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {0,1,0,0,0,1,0},
     {0,0,1,1,1,0,0}},
    // N
    {{1,0,0,0,0,0,1},
     {1,0,0,0,0,1,1},
     {1,0,0,0,0,1,1},
     {1,0,0,0,1,0,1},
     {1,0,0,1,0,0,1},
     {1,0,1,0,0,0,1},
     {1,1,0,0,0,0,1},
     {1,1,0,0,0,0,1},
     {1,0,0,0,0,0,1}},
    // G
    {{0,0,1,1,1,0,0},
     {0,1,0,0,0,1,0},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,1,1,1,1},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {0,1,0,0,0,1,0},
     {0,0,1,1,1,0,0}},
    // R
    {{1,0,0,0,0,1,0},
     {1,0,0,0,1,0,0},
     {1,0,0,1,0,0,0},
     {1,0,1,0,0,0,0},
     {1,1,1,1,1,0,0},
     {1,0,0,0,0,1,0},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,1,0},
     {1,1,1,1,1,0,0}},
    // A
    {{1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,1,1,1,1,1,1},
     {0,1,0,0,0,1,0},
     {0,1,0,0,0,1,0},
     {0,0,1,0,1,0,0},
     {0,0,1,0,1,0,0},
     {0,0,0,1,0,0,0}},
    // T
    {{0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {1,1,1,1,1,1,1}},
    // U
    {{0,0,1,1,1,0,0},
     {0,1,0,0,0,1,0},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1},
     {1,0,0,0,0,0,1}},
    // L
    {{1,1,1,1,1,1,1},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {1,0,0,0,0,0,0}},
    // I
    {{1,1,1,1,1,1,1},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {0,0,0,1,0,0,0},
     {1,1,1,1,1,1,1}},
    // S
    {{0,0,1,1,1,0,0},
     {0,1,0,0,0,1,0},
     {0,0,0,0,0,0,1},
     {0,0,0,0,0,1,0},
     {0,0,1,1,1,0,0},
     {0,1,0,0,0,0,0},
     {1,0,0,0,0,0,0},
     {0,1,0,0,0,1,0},
     {0,0,1,1,1,0,0}},
};

static const int cong_slots[CONG_LEN] = {
    0,16,32,48,64,80,96,112,128,144,160,176,192,208,224
};

static void build_congrats_texture(uint8_t* tex) {
    memset(tex, 0, CONG_TEX_W * CONG_TEX_H * 4);

    // Pass 1: drop shadow (+1,+1 offset)
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
                            if (p[3] == 0) {
                                p[0]=0x00; p[1]=0x08; p[2]=0x10; p[3]=0xCC;
                            }
                        }
                    }
            }
    }

    // Pass 2: white-to-grey gradient fill across 9 rows
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

    // Pass 3: dark grey outline
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

    // Pass 4: white specular highlight on top pixel row of each glyph column
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
        .data.mip_levels[0] = { .ptr = perfect_pixels,
                                 .size = sizeof(perfect_pixels) },
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

    // --- Spinning 3D Chaos Emerald (gem.glsl shader pipeline) ---------------
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
    if (state.height > JUMP_COLLIDE_H) return false;
    int idx = sphere_at(nx, ny);
    if (idx < 0) return false;
    sphere_t* s = &state.spheres[idx];
    if (s->type == SPH_RED) {
        if (state.move_sign < 0) return false;
        if (state.bounce_dist < 0.5f) return false;
        state.game_over = true;
        state.game_over_spinning = true;
        state.game_over_spin_speed = 4.0f;
        state.game_over_timer = 0.0f;
        state.fade_in_timer = 0.0f;
    } else if (s->type == SPH_BLUE) {
        state.last_sphere_x = nx;
        state.last_sphere_y = ny;
        s->type = SPH_RED;
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
            state.last_star_x = -1;
            state.last_star_y = -1;
            return false;
        }
        state.last_star_x = nx;
        state.last_star_y = ny;
        state.move_sign = -state.move_sign;
        state.bounce_dist = 0.0f;
        state.backward_travel = 0.0f;
        state.forward_queued = false;
        if (!state.game_over && state.blue_remaining == 0) state.won = true;
        return true;
    }
    if (!state.game_over && state.blue_remaining == 0) state.won = true;
    return false;
}

static void advance(float dist) {
    if (!state.jumping && state.pending_turn != 0 &&
        (state.frac < 1e-4f || state.frac > 1.0f - 1e-4f)) {
        if (state.frac > 0.5f) {
            state.node_x = gwrap(state.node_x + DIR_DX[state.dir]);
            state.node_y = gwrap(state.node_y + DIR_DY[state.dir]);
        }
        state.frac = 0.0f;
        if (state.pending_turn == -1) {
            state.dir = (state.dir + 3) & 3;
            state.target_angle -= 1.5707963f;
        } else {
            state.dir = (state.dir + 1) & 3;
            state.target_angle += 1.5707963f;
        }
        state.pending_turn = 0;
        state.turning = true;
        return;
    }

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
                if (touch_node(state.node_x, state.node_y))
                    return;
            } else {
                state.frac = 0.0f;
                dist -= room;
                state.bounce_dist = fminf(1.0f, state.bounce_dist + room);
                state.backward_travel += room;
                {
                    int idx = sphere_at(state.node_x, state.node_y);
                    if (idx >= 0 && state.spheres[idx].type == SPH_RED &&
                        state.height <= JUMP_COLLIDE_H && !state.won &&
                        state.bounce_dist >= 0.5f) {
                        state.game_over = true;
                        state.game_over_spinning = true;
                        state.game_over_spin_speed = 4.0f;
                        state.game_over_timer = 0.0f;
                        state.fade_in_timer = 0.0f;
                    }
                }
                if (!state.game_over) {
                    if (touch_node(state.node_x, state.node_y))
                        return;
                }
                state.node_x = gwrap(state.node_x - DIR_DX[state.dir]);
                state.node_y = gwrap(state.node_y - DIR_DY[state.dir]);
                state.frac = 1.0f;
            }
            if (state.game_over) return;
            if (!state.jumping && state.pending_turn != 0) {
                if (state.frac > 0.5f) {
                    state.node_x = gwrap(state.node_x + DIR_DX[state.dir]);
                    state.node_y = gwrap(state.node_y + DIR_DY[state.dir]);
                }
                state.frac = 0.0f;
                if (state.pending_turn == -1) {
                    state.dir = (state.dir + 3) & 3;
                    state.target_angle -= 1.5707963f;
                } else {
                    state.dir = (state.dir + 1) & 3;
                    state.target_angle += 1.5707963f;
                }
                state.pending_turn = 0;
                state.turning = true;
                return;
            }
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
            continue;
        }

        // Congrats screen: tick timers and spin the emerald
        if (state.congrats) {
            state.fade_in_timer  += (float)FIXED_DT;
            state.congrats_timer += (float)FIXED_DT;
            state.emerald_spin   += 7.0f * (float)FIXED_DT; // ~1.1 rev/s (spinning top)
            if (state.emerald_spin > 6.2831853f)
                state.emerald_spin -= 6.2831853f;
            continue;
        }

        // PERFECT: trigger once when all rings collected (regardless of win state)
        if (state.max_rings > 0 && state.rings_remaining == 0 &&
            !state.perfect_phase_triggered) {
            state.perfect_phase = 1;
            state.perfect_timer = 0.0f;
            state.perfect_phase_triggered = true;
        }

        // Advance PERFECT animation (0.33s in, 2.5s hold, 0.33s out)
        if (state.perfect_phase > 0) {
            state.perfect_timer += (float)FIXED_DT;
            const float SLIDE = 0.33f, HOLD = 2.5f;
            if      (state.perfect_timer < SLIDE)            state.perfect_phase = 1;
            else if (state.perfect_timer < SLIDE + HOLD)     state.perfect_phase = 2;
            else if (state.perfect_timer < 2*SLIDE + HOLD)   state.perfect_phase = 3;
            else                                              state.perfect_phase = 0;
        }

        if (state.won) {
            state.win_lift += (1.0f + state.win_lift * 1.5f) * (float)FIXED_DT;
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
                    // Last level: fade white → black, then show congrats
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

        int tier = 0;
        if      (state.stage_time >= 120.0f) tier = 4;
        else if (state.stage_time >=  90.0f) tier = 3;
        else if (state.stage_time >=  60.0f) tier = 2;
        else if (state.stage_time >=  30.0f) tier = 1;
        static const float SPEED_TIERS[5] = {
            4.000f, 4.200f, 4.410f, 4.631f, 4.863f
        };
        state.speed = SPEED_TIERS[tier];
        float step = state.speed * (float)FIXED_DT;

        if (state.jump_queued && !state.jumping && !state.turning) {
            state.jumping = true;
            state.jump_total = JUMP_DISTANCE;
            state.jump_remaining = JUMP_DISTANCE;
        }
        state.jump_queued = false;
        if (state.move_sign < 0 && state.forward_queued && !state.jumping) {
            state.move_sign = 1; state.forward_queued = false; state.backward_travel = 0.0f;
        }
        bool was_jumping = state.jumping;
        if (state.jumping) {
            state.jump_remaining -= step;
            float dj = (state.jump_total - state.jump_remaining) / state.jump_total;
            dj = dj < 0.0f ? 0.0f : (dj > 1.0f ? 1.0f : dj);
            float arc = 1.0f - (2.0f*dj - 1.0f)*(2.0f*dj - 1.0f);
            state.height = arc * JUMP_HEIGHT;
            if (state.jump_remaining <= 0.0f) { state.jumping = false; state.height = 0.0f; }
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
            float dj = (state.jump_total - state.jump_remaining) / state.jump_total;
            dj = dj < 0.0f ? 0.0f : (dj > 1.0f ? 1.0f : dj);
            state.player_frame = SONIC_RUN_FRAMES + (int)(dj * (SONIC_JUMP_FRAMES - 1));
        }
    }

    float pos_x = (float)state.node_x + (float)DIR_DX[state.dir] * state.frac;
    float pos_y = (float)state.node_y + (float)DIR_DY[state.dir] * state.frac;
    fs_params_t fsp = {
        .aspect = sapp_widthf() / sapp_heightf(),
        .scroll = { pos_x, pos_y }, .tile_size = 1.0f, .rot = state.vis_angle };
    float aspect = sapp_widthf() / sapp_heightf();

    struct bd { float cx, cy, hx, hy, depth, r, g, b, star, spin, wdx, wdy;
                float tc[3], ta[3]; };
    struct bd draws[MAX_VISIBLE_SPHERES];
    int ndraw = 0;
    for (int i = 0; i < state.sphere_count && ndraw < MAX_VISIBLE_SPHERES; i++) {
        sphere_t* s = &state.spheres[i];
        if (!s->active) continue;
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
        if (s->type != SPH_RING) { normal[0] = normal[1] = normal[2] = 0.0f; }
        float cx, cy, hx, hy, depth;
        if (!project_ball(center, aspect, &cx, &cy, &hx, &hy, &depth)) continue;
        float dist = sqrtf(dist2);
        float scale = 1.0f - fmaxf(0.0f, (dist - 4.0f) / (float)(VISIBLE_RANGE - 4));
        scale = scale * scale;
        hx *= scale; hy *= scale;
        float bhx = hx, bhy = hy;
        if (s->type == SPH_RING) { bhx *= 3.0f; bhy *= 3.0f; }
        if (bhx < 1e-4f) continue;
        struct bd* d = &draws[ndraw++];
        d->cx=cx; d->cy=cy; d->hx=bhx; d->hy=bhy; d->depth=depth;
        d->star=0.0f; d->spin=0.0f; d->wdx=dx; d->wdy=dy;
        d->tc[0]=center[0]; d->tc[1]=center[1]; d->tc[2]=center[2];
        d->ta[0]=normal[0]; d->ta[1]=normal[1]; d->ta[2]=normal[2];
        if (s->type == SPH_RED)       { d->r=0.95f; d->g=0.15f; d->b=0.15f; }
        else if (s->type == SPH_RING) { d->r=1.00f; d->g=0.84f; d->b=0.10f; d->star=2.0f; d->spin=state.ring_spin; }
        else if (s->type == SPH_STAR) { d->r=0.92f; d->g=0.92f; d->b=0.95f; d->star=1.0f; }
        else                          { d->r=0.15f; d->g=0.35f; d->b=0.95f; }
    }
    for (int a=0;a<ndraw;a++) for (int b=a+1;b<ndraw;b++)
        if (draws[b].depth > draws[a].depth) { struct bd tmp=draws[a]; draws[a]=draws[b]; draws[b]=tmp; }

    // -------------------------------------------------------------------------
    // CONGRATULATIONS screen: black bg + text banner + green emerald
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

        float aspect = sapp_widthf() / sapp_heightf();

        // --- Spinning 3D Chaos Emerald (gem shader) -------------------------
        // Bounding quad ~1.45:1 to match reference gem proportions.
        // The shader discards pixels outside the gem silhouette.
        {
            const float gw = 0.48f, gh = 0.44f;  // 25% narrower than before
            sg_apply_pipeline(state.gem_pip);
            sg_apply_bindings(&state.gem_bind);
            gem_vs_t gvs = { .center   = { 0.0f, -0.02f },
                             .halfsize = { gw, gh } };
            gem_fs_t gfs = { .spin = state.emerald_spin };
            sg_apply_uniforms(UB_gem_vs, &SG_RANGE(gvs));
            sg_apply_uniforms(UB_gem_fs, &SG_RANGE(gfs));
            sg_draw(0, 6, 1);
        }

        // --- CONGRATULATIONS text near top ----------------------------------
        // 210 used px × 2 screen-scale = 420 px → NDC 420/400 = 1.05 wide
        // height 20 px × 2 = 40 px → NDC 40/300 = 0.133
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

        // --- Fade in from black after transition ----------------------------
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
                     .attachments.colors[0]      = state.crt_att_view,
                     .attachments.depth_stencil   = state.crt_depth_att_view }
        : (sg_pass){ .action = state.pass_action, .swapchain = sglue_swapchain() };
    sg_begin_pass(&game_pass);
    sg_apply_pipeline(state.pip); sg_apply_bindings(&state.bind);
    sg_apply_uniforms(UB_fs_params, &SG_RANGE(fsp)); sg_draw(0, 3, 1);

    sg_apply_pipeline(state.ball_pip); sg_apply_bindings(&state.ball_bind);
    for (int i = 0; i < ndraw; i++) {
        ball_vs_t bvs = { .center = { draws[i].cx, draws[i].cy },
                          .halfsize = { draws[i].hx, draws[i].hy } };
        ball_fs_t bfs = { .color = { draws[i].r, draws[i].g, draws[i].b, draws[i].star },
                          .spin = draws[i].spin, .tilt = 0.0f,
                          .tc = { draws[i].tc[0], draws[i].tc[1], draws[i].tc[2], aspect },
                          .ta = { draws[i].ta[0], draws[i].ta[1], draws[i].ta[2], 0.0f } };
        sg_apply_uniforms(UB_ball_vs, &SG_RANGE(bvs));
        sg_apply_uniforms(UB_ball_fs, &SG_RANGE(bfs));
        sg_draw(0, 6, 1);
    }

    float jump_rise = state.height * 1.2f;
    float sprite_aspect = (float)SONIC_FRAME_W / (float)SONIC_TEX_HEIGHT;
    float sprite_h = 0.22f;
    float sprite_w = sprite_h * sprite_aspect / aspect;
    player_vs_t pvs = { .center = { 0.0f, -0.30f + jump_rise },
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
    float gpx = 2.0f / sw;
    float gpy = 2.0f / sh;
    float scale = 3.0f;
    float gw = HUD_GLYPH_W  * gpx * scale;
    float gh = HUD_GLYPH_H  * gpy * scale;
    float pad = 4.0f * gpx * scale;
    float margin_x = 0.04f;
    float margin_y = 0.04f;
    float top = 1.0f - margin_y - gh;

    #define DRAW_GLYPH(glyph_idx, clip_x, clip_y) do { \
        float _gu = (float)(glyph_idx) * (float)HUD_GLYPH_W / (float)HUD_ATLAS_W; \
        float _guw = (float)HUD_GLYPH_W / (float)HUD_ATLAS_W; \
        hud_vs_t _h = { \
            .pos  = { (clip_x), (clip_y) }, \
            .size = { gw, gh }, \
            .uv0  = { _gu,       1.0f }, \
            .uv1  = { _gu+_guw,  0.0f }, \
        }; \
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(_h)); \
        sg_draw(0, 6, 1); \
    } while(0)

    #define DRAW_DIGITS(num, start_x, out_x) do { \
        int _n = (num) < 999 ? (num) : 999; \
        float _x = (start_x); \
        if (_n >= 100) { DRAW_GLYPH(_n/100,       _x, top); _x += gw + gpx; } \
        if (_n >= 10)  { DRAW_GLYPH((_n/10)%10,   _x, top); _x += gw + gpx; } \
        DRAW_GLYPH(_n%10, _x, top); _x += gw + gpx; \
        (out_x) = _x; \
    } while(0)

    float icon_gap = gw * 0.5f;

    {
        float x = -1.0f + margin_x + pad;
        float end_x;
        DRAW_DIGITS(state.blue_remaining, x, end_x);
        DRAW_GLYPH(HUD_GLYPH_SPHERE, end_x + icon_gap, top);
    }

    {
        int r = state.rings_remaining;
        int nd = r >= 100 ? 3 : (r >= 10 ? 2 : 1);
        float total_w = gw + icon_gap + nd * (gw + gpx);
        float x = 1.0f - margin_x - total_w;
        DRAW_GLYPH(HUD_GLYPH_RING, x, top);
        x += gw + icon_gap;
        float end_x;
        DRAW_DIGITS(r, x, end_x);
    }

    {
        float alpha = 0.0f, fr = 0.0f, fg = 0.0f, fb = 0.0f;
        if (state.game_over && state.game_over_spinning)
            alpha = fminf(state.game_over_timer / 2.0f, 1.0f);
        else if (state.won && state.win_lift > 3.0f && state.wbk_timer <= 0.0f)
            { alpha = fminf((state.win_lift - 3.0f) / 6.0f, 1.0f); fr = fg = fb = 1.0f; }
        else if (state.won && state.wbk_timer > 0.0f) {
            // White → black crossfade before congrats screen
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

    // --- PERFECT notification: "PERF" slides from left, "ECT" from right ---
    if (state.perfect_phase > 0) {
        const float SLIDE = 0.33f, HOLD = 2.5f;
        float t = state.perfect_timer;
        float offset;
        if      (state.perfect_phase == 1) offset = 1.05f * (1.0f - t / SLIDE);
        else if (state.perfect_phase == 3) offset = 1.05f * ((t - SLIDE - HOLD) / SLIDE);
        else                               offset = 0.0f;
        offset = fmaxf(0.0f, fminf(offset, 1.05f));

        // PERF: 56 tex-px × 3 = 168 screen-px → NDC 168/400
        // ECT:  42 tex-px × 3 = 126 screen-px → NDC 126/400
        // Height: 20 tex-px × 3 = 60 screen-px → NDC 60/300
        const float pw = 168.0f / 400.0f;
        const float ew = 126.0f / 400.0f;
        const float ht =  60.0f / 300.0f;
        const float by =   0.0f;
        const float su = (float)PERF_SPLIT / PERF_TEX_W;
        const float eu = (float)PERF_END   / PERF_TEX_W;

        sg_apply_pipeline(state.hud_pip);
        sg_apply_bindings(&state.perfect_bind);

        hud_vs_t lv = { .pos  = { -offset - pw, by },
                        .size = { pw, ht },
                        .uv0  = { 0.0f, 0.0f },
                        .uv1  = { su,   1.0f  } };
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(lv));
        sg_draw(0, 6, 1);

        hud_vs_t rv = { .pos  = { offset, by },
                        .size = { ew, ht },
                        .uv0  = { su, 0.0f },
                        .uv1  = { eu, 1.0f } };
        sg_apply_uniforms(UB_hud_vs, &SG_RANGE(rv));
        sg_draw(0, 6, 1);
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
}

static void event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat) {
        switch (e->key_code) {
            case SAPP_KEYCODE_LEFT:  case SAPP_KEYCODE_A:
                if (!state.game_over && !state.won && state.pending_turn == 0 && !state.turning)
                    state.pending_turn = -1;
                break;
            case SAPP_KEYCODE_RIGHT: case SAPP_KEYCODE_D:
                if (!state.game_over && !state.won && state.pending_turn == 0 && !state.turning)
                    state.pending_turn = +1;
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
                else if (state.game_over) { reset_game(state.current_level); state.fade_in_timer = 0.0f; }
                else if (state.won && state.win_lift >= 9.0f) { reset_game(state.current_level); state.fade_in_timer = 0.0f; }
                else if (!state.won && !state.congrats) state.jump_queued = true;
                break;
            case SAPP_KEYCODE_ENTER:
                if (state.started && !state.game_over && !state.won)
                    state.paused = !state.paused;
                break;
            case SAPP_KEYCODE_ESCAPE: sapp_request_quit(); break;
            case SAPP_KEYCODE_S:
                state.crt_enabled = !state.crt_enabled; break;
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