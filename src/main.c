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

#include "sphere.glsl.h"
#include "player.glsl.h"
#include "ball.glsl.h"

// minimal mat4 type so the generated @ctype resolves (shader uses no mat4)
typedef struct { float m[16]; } hmm_mat4;

// the four facing directions as integer grid steps, ordered so that
// turning right = (dir+1)%4 and turning left = (dir+3)%4.
//   0 = +Y (north)   1 = +X (east)   2 = -Y (south)   3 = -X (west)
static const int DIR_DX[4] = {  0,  1,  0, -1 };
static const int DIR_DY[4] = {  1,  0, -1,  0 };

// --- spheres -----------------------------------------------------------------
// A sphere sits on a grid node (integer coords). type: 0 = blue, 1 = red.
// "collected" blue spheres flip to red. We keep a flat list for the level.
typedef enum { SPH_BLUE = 0, SPH_RED = 1, SPH_RING = 2, SPH_STAR = 3 } sphere_type;
typedef struct {
    int x, y;            // grid node
    sphere_type type;    // current color
    bool active;         // present in the world
} sphere_t;

#define MAX_LEVEL_SPHERES 256
#define MAX_VISIBLE_SPHERES 64       // must match MAX_SPHERES in the shader
#define VISIBLE_RANGE 12             // only spheres within this many tiles draw

// A small hand-made test layout: {x, y, type}. type 0=blue, 3=star.
// The 5x5 block is a closeable loop with a blue interior (run the border to
// convert the inside to rings). A couple of star spheres are placed on the
// approach so you can test the bounce (hit one -> you reverse; press Up/forward
// to resume forward once you've cleared a tile).
static const int LEVEL_LAYOUT[][3] = {
    // 5x5 border (the loop to close), y = 3..7, x = 0..4
    {0,3,0},{1,3,0},{2,3,0},{3,3,0},{4,3,0},
    {0,4,0},                        {4,4,0},
    {0,5,0},                        {4,5,0},
    {0,6,0},                        {4,6,0},
    {0,7,0},{1,7,0},{2,7,0},{3,7,0},{4,7,0},
    // interior blue spheres that convert to rings when the loop closes
    {1,4,0},{2,4,0},{3,4,0},
    {1,5,0},{2,5,0},{3,5,0},
    {1,6,0},{2,6,0},{3,6,0},
    // scattered extras
    {-3,5,0},{-3,6,0},{-4,6,0},
    // star (bumper) spheres to test the bounce
    {0,1,3},          // straight ahead on the way in
    {-3,9,3},{3,9,3}, // a couple further out
};
#define LEVEL_LAYOUT_COUNT ((int)(sizeof(LEVEL_LAYOUT)/sizeof(LEVEL_LAYOUT[0])))

// --- 3D ground geometry (must match the shader's constants) ------------------
// Used to compute each ball's 3D world-space center so the shader can ray-trace
// it as a real standing ball. The ball sits on the ground sphere surface at its
// grid node, raised by the ball radius.
#define G_GR     12.5f                       // ground radius
#define G_GCx    0.0f
#define G_GCy    0.0f
#define G_GCz   -12.5f                        // ground center
#define G_PIVOTx 0.0f
#define G_PIVOTy 1.223f                       // rotation pivot (under the player)
#define BALL_RADIUS_C 0.30f                   // must match shader BALL_RADIUS

// --- jump constants (from the original game) ---------------------------------
#define JUMP_DISTANCE   2.0f    // a jump always carries you this many tiles fwd
#define JUMP_HEIGHT     0.5f    // peak height of the arc
#define JUMP_COLLIDE_H  0.2f    // above this height you pass over spheres

// Project a ball's 3D center to a clip-space billboard rect. Returns false if
// the ball is behind the camera. Fills clip-space center (cx,cy), half-size
// (hx,hy), and the view-space depth (for back-to-front sorting).
static bool project_ball(const float center[3], float aspect,
                         float* cx, float* cy, float* hx, float* hy, float* depth) {
    // camera basis (must match the shader)
    float camx = G_GCx*0 + 0.0f, camy = 1.1f, camz = 1.6f;
    float tgx = 0.0f, tgy = 6.0f, tgz = -7.0f;
    float fx = tgx - camx, fy = tgy - camy, fz = tgz - camz;
    float fl = sqrtf(fx*fx + fy*fy + fz*fz); fx/=fl; fy/=fl; fz/=fl;
    // right = normalize(cross(fwd, up0)), up0 = (0,1,0)
    float rx = fy*0.0f - fz*1.0f, ry = fz*0.0f - fx*0.0f, rz = fx*1.0f - fy*0.0f;
    float rl = sqrtf(rx*rx + ry*ry + rz*rz); rx/=rl; ry/=rl; rz/=rl;
    // up = cross(right, fwd)
    float ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;

    float tox = center[0]-camx, toy = center[1]-camy, toz = center[2]-camz;
    float d = tox*fx + toy*fy + toz*fz;     // depth along view dir
    if (d <= 0.05f) return false;
    *depth = d;

    const float FOCAL = 1.0f;
    const float BALL_R = BALL_RADIUS_C;
    // screen-plane coords of the center (perspective divide by depth, /FOCAL)
    float sx = (tox*rx + toy*ry + toz*rz) / d / FOCAL;
    float sy = (tox*ux + toy*uy + toz*uz) / d / FOCAL;
    // apparent radius in the same units
    float ar = (BALL_R / d) / FOCAL;
    // convert screen-plane (ndc-with-aspect) to clip space: x was *aspect in shader
    *cx = sx / aspect;
    *cy = sy;
    *hx = ar / aspect;
    *hy = ar;
    return true;
}

// Compute a ball's 3D center given its world grid node and the player's current
// scroll (pos) and rotation. Mirrors the shader's projection, inverted:
//   world node -> player-fixed grid point -> ground surface point -> raise.
static void ball_center(float wx, float wy, float pos_x, float pos_y, float rot,
                        float out[3]) {
    // world node -> player-fixed grid coord (inverse of rotate(gp-PIVOT)+scroll)
    float rpx = wx - pos_x;
    float rpy = wy - pos_y;
    float cr = cosf(rot), sr = sinf(rot);
    // inverse rotation (transpose of the shader's matrix)
    float gpx = rpx * cr - rpy * sr + G_PIVOTx;
    float gpy = rpx * sr + rpy * cr + G_PIVOTy;
    // grid point (gpx,gpy,0) -> ground sphere surface point along the ray from GC
    float dirx = gpx - G_GCx, diry = gpy - G_GCy, dirz = 0.0f - G_GCz;
    float dlen = sqrtf(dirx*dirx + diry*diry + dirz*dirz);
    float t = G_GR / dlen;
    float sx = G_GCx + dirx * t;
    float sy = G_GCy + diry * t;
    float sz = G_GCz + dirz * t;
    // surface normal, raise center by the ball radius
    float nx = (sx - G_GCx) / G_GR, ny = (sy - G_GCy) / G_GR, nz = (sz - G_GCz) / G_GR;
    out[0] = sx + nx * BALL_RADIUS_C;
    out[1] = sy + ny * BALL_RADIUS_C;
    out[2] = sz + nz * BALL_RADIUS_C;
}

static struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_pass_action pass_action;

    // player billboard (drawn as a second pass over the floor)
    sg_pipeline player_pip;
    sg_bindings player_bind;
    float player_phase;   // run-cycle animation phase

    // sphere billboards (one quad per visible ball, drawn back-to-front)
    sg_pipeline ball_pip;
    sg_bindings ball_bind;

    // --- grid-locked player state ---
    // Position is stored as an EXACT integer node we last left, plus a fraction
    // [0,1) of progress along the current edge toward the next node. The
    // continuous world position is reconstructed from these, so it is
    // mathematically impossible to drift into the middle of a square: you are
    // always exactly on an edge, and turns can only happen when frac == 0.
    int   node_x, node_y; // last grid corner left (always integer)
    float frac;           // progress along current edge, 0..1
    int   dir;            // current facing: index into DIR_* tables (logical)
    int   pending_turn;   // -1 = turn left queued, +1 = right, 0 = none
    float speed;          // tiles per second

    // --- star sphere (bumper) bounce ---
    // Hitting a star reverses your travel: you keep FACING the same way but move
    // backward (the world scrolls toward you). move_sign is +1 forward, -1 back.
    // bounce_dist ramps 0->1 as you travel away from the last star; until it
    // reaches 1 you can't turn or recover forward (prevents instant re-trigger).
    int   move_sign;      // +1 = forward, -1 = backward
    float bounce_dist;    // distance traveled since last bounce, capped at 1
    bool  forward_queued; // forward key pressed while going backward

    // --- smooth pivot (visual only; logical dir still snaps at the node) ---
    float vis_angle;      // angle the shader actually uses, eased over time
    float target_angle;   // where vis_angle is heading; ±pi/2 added per turn
    float turn_speed;     // radians per second of the pivot
    bool  turning;        // true while the camera is mid-pivot at a corner;
                          // forward motion is paused so the turn always happens
                          // visibly ON the corner, never partway into a square.

    double accum;         // fixed-timestep accumulator (seconds of unspent time)

    // --- jump (grid-based, matches the original) ---
    // A jump carries you a FIXED distance forward (JUMP_DISTANCE tiles) along a
    // parabolic height arc. While airborne above JUMP_COLLIDE_H you pass OVER
    // spheres without touching them. Because the distance is fixed but your
    // start position within an edge varies, jumping at the last moment before a
    // node lets the same arc clear two spheres instead of one.
    bool  jumping;
    float jump_total;     // total distance of this jump (tiles)
    float jump_remaining; // distance left to travel before landing
    float height;         // current height off the ground (for collision + visual)
    bool  jump_queued;    // jump pressed; consumed on the next ground tick

    // --- spheres ---
    sphere_t spheres[MAX_LEVEL_SPHERES];
    int      sphere_count;
    int      blue_remaining;
    int      rings;          // rings collected
    bool     game_over;      // true when you hit a red sphere; space restarts
    bool     won;            // true when all blue spheres are cleared
    int      last_node_x, last_node_y;  // to detect arriving on a new node

    uint64_t last_time;
} state;

// Reset all per-run game state: player back to the start, spheres rebuilt from
// the layout (all blue), counters cleared. Used at startup and on restart.
static void reset_game(void) {
    state.node_x = 0;
    state.node_y = 0;
    state.frac = 0.0f;
    state.dir = 0;
    state.pending_turn = 0;
    state.speed = 4.0f;
    state.move_sign = 1;
    state.bounce_dist = 1.0f;
    state.forward_queued = false;
    state.last_node_x = 0;
    state.last_node_y = 0;

    state.sphere_count = 0;
    state.blue_remaining = 0;
    state.rings = 0;
    state.game_over = false;
    state.won = false;
    for (int i = 0; i < LEVEL_LAYOUT_COUNT && i < MAX_LEVEL_SPHERES; i++) {
        sphere_t* s = &state.spheres[state.sphere_count];
        s->x = LEVEL_LAYOUT[i][0];
        s->y = LEVEL_LAYOUT[i][1];
        s->type = (sphere_type)LEVEL_LAYOUT[i][2];
        s->active = true;
        state.sphere_count++;
        if (s->type == SPH_BLUE) state.blue_remaining++;   // only blues count
    }

    state.vis_angle    = 0.0f;
    state.target_angle = 0.0f;
    state.turn_speed   = 1.5707963f / 0.18f;
    state.turning      = false;
    state.accum        = 0.0;
    state.jumping       = false;
    state.jump_total    = 0.0f;
    state.jump_remaining= 0.0f;
    state.height        = 0.0f;
    state.jump_queued   = false;
}

static void init(void) {
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    stm_setup();
    state.last_time = stm_now();

    reset_game();
    const float verts[] = {
        -1.0f, -1.0f,
         3.0f, -1.0f,
        -1.0f,  3.0f,
    };
    state.bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(verts),
        .label = "fullscreen-tri",
    });

    state.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(sphere_shader_desc(sg_query_backend())),
        .layout = {
            .attrs = {
                [ATTR_sphere_position].format = SG_VERTEXFORMAT_FLOAT2,
            },
        },
        .label = "sphere-pipeline",
    });

    state.pass_action = (sg_pass_action){
        .colors[0] = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = { 0.55f, 0.80f, 1.0f, 1.0f },
        },
    };

    // --- player billboard pipeline ---
    // a unit quad (two triangles) in 0..1 corner coords; the vertex shader
    // places it in clip space via the center/halfsize uniform.
    const float quad[] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    state.player_bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad),
        .label = "player-quad",
    });
    state.player_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(player_shader_desc(sg_query_backend())),
        .layout = {
            .attrs = { [ATTR_player_corner].format = SG_VERTEXFORMAT_FLOAT2 },
        },
        // alpha blend so the discarded/edge pixels let the floor show through
        .colors[0].blend = {
            .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ZERO,
        },
        .label = "player-pipeline",
    });
    state.player_phase = 0.0f;

    // --- sphere billboard pipeline ---
    // reuses the same unit-quad; one draw per ball with its own center/size/color.
    state.ball_bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad),
        .label = "ball-quad",
    });
    state.ball_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(ball_shader_desc(sg_query_backend())),
        .layout = {
            .attrs = { [ATTR_ball_corner].format = SG_VERTEXFORMAT_FLOAT2 },
        },
        .colors[0].blend = {
            .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ZERO,
        },
        .label = "ball-pipeline",
    });
}

// Look up a sphere at a grid node; returns index or -1.
static int sphere_at(int x, int y) {
    for (int i = 0; i < state.sphere_count; i++) {
        sphere_t* s = &state.spheres[i];
        if (s->active && s->x == x && s->y == y) return i;
    }
    return -1;
}

// Scratch list of cells converted to rings in the current conversion, so we can
// then turn the bounding red loop into rings too.
static int g_conv_x[MAX_LEVEL_SPHERES];
static int g_conv_y[MAX_LEVEL_SPHERES];
static int g_conv_n;

// Flood-fill the entire connected cluster of BLUE spheres starting at (x,y),
// converting each to a ring. Matches the original's FloodFillRings: from a seed
// blue, spread to all orthogonal neighbors that are blue, and theirs, etc. So
// the whole connected blob converts, not just an enclosed core. Records each
// converted cell in the scratch list.
static void flood_cluster_to_rings(int x, int y) {
    int idx = sphere_at(x, y);
    if (idx < 0) return;
    sphere_t* s = &state.spheres[idx];
    if (s->type != SPH_BLUE) return;
    s->type = SPH_RING;
    if (state.blue_remaining > 0) state.blue_remaining--;
    if (g_conv_n < MAX_LEVEL_SPHERES) {
        g_conv_x[g_conv_n] = x; g_conv_y[g_conv_n] = y; g_conv_n++;
    }
    flood_cluster_to_rings(x + 1, y);
    flood_cluster_to_rings(x - 1, y);
    flood_cluster_to_rings(x, y + 1);
    flood_cluster_to_rings(x, y - 1);
}

// Ring conversion: when a region becomes enclosed by red spheres, the enclosed
// blue spheres -- AND the entire blue cluster connected to them -- turn into
// rings. We detect enclosure with a flood fill from OUTSIDE (red = walls); any
// blue the outside can't reach is enclosed. We then seed the cluster flood from
// each enclosed blue, so the whole connected blob converts like the original.
static void convert_enclosed_to_rings(void) {
    // bounding box of all active spheres, padded by 1 so "outside" exists.
    int minx = 1<<30, miny = 1<<30, maxx = -(1<<30), maxy = -(1<<30);
    for (int i = 0; i < state.sphere_count; i++) {
        sphere_t* s = &state.spheres[i];
        if (!s->active) continue;
        if (s->x < minx) minx = s->x;
        if (s->x > maxx) maxx = s->x;
        if (s->y < miny) miny = s->y;
        if (s->y > maxy) maxy = s->y;
    }
    if (maxx < minx) return;     // no spheres
    minx--; miny--; maxx++; maxy++;       // pad
    int W = maxx - minx + 1, H = maxy - miny + 1;
    if (W <= 0 || H <= 0 || (long)W*H > 1000000) return;  // safety

    static unsigned char reached[1 << 20];
    if ((long)W*H > (long)sizeof(reached)) return;
    for (int i = 0; i < W*H; i++) reached[i] = 0;

    // flood from outside; red spheres block, everything else is passable.
    int stackx[1 << 16], stacky[1 << 16], sp = 0;
    stackx[sp] = minx; stacky[sp] = miny; sp++;
    reached[0] = 1;
    while (sp > 0) {
        sp--;
        int cx = stackx[sp], cy = stacky[sp];
        const int dx4[4] = {1,-1,0,0}, dy4[4] = {0,0,1,-1};
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx4[d], ny = cy + dy4[d];
            if (nx < minx || nx > maxx || ny < miny || ny > maxy) continue;
            int li = (ny - miny) * W + (nx - minx);
            if (reached[li]) continue;
            int idx = sphere_at(nx, ny);
            if (idx >= 0 && state.spheres[idx].type == SPH_RED) continue; // wall
            reached[li] = 1;
            if (sp < (1<<16)) { stackx[sp] = nx; stacky[sp] = ny; sp++; }
        }
    }

    // Every enclosed BLUE sphere seeds a cluster flood. Because the flood spreads
    // through all connected blues, one closed loop converts the whole blob it
    // surrounds. Seeding from each enclosed blue is harmless (already-converted
    // rings stop the recursion) and guarantees we catch every connected piece.
    g_conv_n = 0;
    for (int i = 0; i < state.sphere_count; i++) {
        sphere_t* s = &state.spheres[i];
        if (!s->active || s->type != SPH_BLUE) continue;
        int li = (s->y - miny) * W + (s->x - minx);
        if (!reached[li]) {
            flood_cluster_to_rings(s->x, s->y);
        }
    }

    // If anything converted, also turn the enclosing red loop into rings -- like
    // the original. The loop is the red spheres bounding the cluster. We check
    // all 8 neighbors (orthogonal AND diagonal) so the loop's corner spheres,
    // which only touch the interior diagonally, convert too.
    if (g_conv_n > 0) {
        int wall_n = g_conv_n;   // freeze count; only scan original conversions
        const int wx8[8] = {1,-1,0,0, 1,1,-1,-1};
        const int wy8[8] = {0,0,1,-1, 1,-1,1,-1};
        for (int c = 0; c < wall_n; c++) {
            for (int d = 0; d < 8; d++) {
                int rx = g_conv_x[c] + wx8[d], ry = g_conv_y[c] + wy8[d];
                int ri = sphere_at(rx, ry);
                if (ri >= 0 && state.spheres[ri].type == SPH_RED) {
                    state.spheres[ri].type = SPH_RING;   // wall sphere -> ring
                }
            }
        }
    }
}

// When the player lands on a node, act on the sphere there. While airborne above
// the collide height, we pass OVER spheres without touching them.
//   red  -> game over (you crashed into a red sphere)
//   blue -> turns red, then check if that enclosed any blues -> rings
//   ring -> collected
static void touch_node(int nx, int ny) {
    if (state.height > JUMP_COLLIDE_H) return;   // sailing over -> no contact
    int idx = sphere_at(nx, ny);
    if (idx < 0) return;
    sphere_t* s = &state.spheres[idx];
    if (s->type == SPH_RED) {
        state.game_over = true;
    } else if (s->type == SPH_BLUE) {
        s->type = SPH_RED;
        if (state.blue_remaining > 0) state.blue_remaining--;
        convert_enclosed_to_rings();
    } else if (s->type == SPH_RING) {
        s->active = false;       // collected
        state.rings++;
    } else if (s->type == SPH_STAR) {
        // bumper: reverse travel and snap to the star. You keep FACING the same
        // way but now move backward (or forward again, if you were backward).
        // bounce_dist resets so you must clear a full tile before turning or
        // recovering forward -- prevents instantly re-triggering on the star.
        state.move_sign = -state.move_sign;
        state.bounce_dist = 0.0f;
        state.frac = 0.0f;       // snapped exactly onto the star node
    }

    // win when no blue spheres remain -- whether they were turned red or
    // converted to rings (both decrement blue_remaining). matches the original's
    // Count(BlueSphere) == 0 clear condition.
    if (!state.game_over && state.blue_remaining == 0) {
        state.won = true;
    }
}

// Advance the player by `dist` tiles. move_sign selects forward (+1, frac rises
// to the next node) or backward (-1, frac falls to the previous node). Arrival
// at a node is exact. Turns fire only at a node, on the ground, going forward,
// and once we're a full tile clear of the last star (bounce_dist == 1).
static void advance(float dist) {
    while (dist > 0.0f) {
        // ramp the bounce distance up as we travel (gates turning/recovery).
        float room;
        if (state.move_sign > 0) room = 1.0f - state.frac;   // dist to next node
        else                     room = state.frac;          // dist to prev node

        if (dist < room) {
            state.frac += (float)state.move_sign * dist;
            state.bounce_dist = fminf(1.0f, state.bounce_dist + dist);
            dist = 0.0f;
        } else {
            // arrive exactly at a node (next if forward, previous if backward)
            if (state.move_sign > 0) {
                state.node_x += DIR_DX[state.dir];
                state.node_y += DIR_DY[state.dir];
                state.frac = 0.0f;
            } else {
                state.node_x -= DIR_DX[state.dir];
                state.node_y -= DIR_DY[state.dir];
                state.frac = 1.0f;
            }
            dist -= room;
            state.bounce_dist = fminf(1.0f, state.bounce_dist + room);

            // act on the sphere at this node (star bounce handled in touch_node)
            touch_node(state.node_x, state.node_y);
            if (state.game_over || state.won) return;

            // turn AT the corner -- only on the ground, going forward, and once
            // clear of the last star bounce. queued turns otherwise wait.
            if (!state.jumping && state.move_sign > 0 &&
                state.bounce_dist >= 1.0f && state.pending_turn != 0) {
                if (state.pending_turn == -1) {
                    state.dir = (state.dir + 3) & 3;      // left
                    state.target_angle -= 1.5707963f;
                } else {
                    state.dir = (state.dir + 1) & 3;      // right
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
    // --- fixed timestep ------------------------------------------------------
    // Movement must NOT depend on frame rate, or speed drifts and big frames can
    // jump you off the grid. We accumulate real elapsed time and consume it in
    // identical fixed-size logic ticks. Each tick advances the exact same
    // distance, so speed is constant forever regardless of display refresh.
    double dt = stm_sec(stm_laptime(&state.last_time));
    if (dt > 0.25) dt = 0.25;   // avoid a spiral-of-death after a long stall
    state.accum += dt;

    const double FIXED_DT = 1.0 / 120.0;          // 120 logic ticks per second
    const float  step = state.speed * (float)FIXED_DT;  // tiles per tick (constant)

    while (state.accum >= FIXED_DT) {
        state.accum -= FIXED_DT;

        // when game over OR won, freeze the world; a key (in event) restarts.
        if (state.game_over || state.won) { state.jump_queued = false; continue; }

        // start a jump if one was queued and we're grounded and not pivoting.
        if (state.jump_queued && !state.jumping && !state.turning) {
            state.jumping = true;
            state.jump_total = JUMP_DISTANCE;
            state.jump_remaining = JUMP_DISTANCE;
        }
        state.jump_queued = false;

        // forward-recovery: while going backward, pressing forward flips you
        // back to forward -- but only once you've cleared a full tile from the
        // last star (bounce_dist == 1) and aren't jumping. Matches the original.
        if (state.move_sign < 0 && state.forward_queued &&
            state.bounce_dist >= 1.0f && !state.jumping) {
            state.move_sign = 1;
            state.forward_queued = false;
        }

        // forward motion: normally paused during a pivot, BUT a jump keeps you
        // moving (you can't pivot mid-air anyway, so turning is false here).
        if (!state.turning || state.jumping) {
            advance(step);
        }

        // jump arc: travel the fixed jump distance, height as a parabola that
        // peaks at the midpoint. While height > JUMP_COLLIDE_H we sail over
        // spheres (handled in touch_node). Land when the distance runs out.
        if (state.jumping) {
            state.jump_remaining -= step;
            float dj = (state.jump_total - state.jump_remaining) / state.jump_total;
            dj = dj < 0.0f ? 0.0f : (dj > 1.0f ? 1.0f : dj);
            float arc = 1.0f - (2.0f * dj - 1.0f) * (2.0f * dj - 1.0f);  // 0..1..0
            state.height = arc * JUMP_HEIGHT;
            if (state.jump_remaining <= 0.0f) {
                state.jumping = false;
                state.height = 0.0f;
                // landed exactly on a node? collect a sphere there now.
                // (advance keeps frac continuous; if we landed mid-edge that's
                // fine -- collection happens when we next cross a node.)
            }
        }

        // ease the visual angle toward the target at a constant rate.
        float diff = state.target_angle - state.vis_angle;
        float maxstep = state.turn_speed * (float)FIXED_DT;
        if (diff >  maxstep) diff =  maxstep;
        if (diff < -maxstep) diff = -maxstep;
        state.vis_angle += diff;

        // pivot finished? snap exactly to target and resume running.
        if (state.turning && fabsf(state.target_angle - state.vis_angle) < 1e-4f) {
            state.vis_angle = state.target_angle;
            state.turning = false;
        }

        // advance the run-cycle animation only while actually running on ground.
        if (!state.turning && !state.jumping) {
            state.player_phase += 9.0f * (float)FIXED_DT;   // cycles ~1.4/sec
            if (state.player_phase > 6.2831853f) state.player_phase -= 6.2831853f;
        }
    }

    // reconstruct the continuous world position from the exact node + fraction
    // along the current edge. guaranteed to be exactly on a grid edge.
    float pos_x = (float)state.node_x + (float)DIR_DX[state.dir] * state.frac;
    float pos_y = (float)state.node_y + (float)DIR_DY[state.dir] * state.frac;

    fs_params_t fsp = {
        .aspect    = sapp_widthf() / sapp_heightf(),
        .scroll    = { pos_x, pos_y },
        .tile_size = 1.0f,
        .rot       = state.vis_angle,
    };
    float aspect = sapp_widthf() / sapp_heightf();

    // --- collect visible balls, projected to clip-space rects ----------------
    struct bd { float cx, cy, hx, hy, depth, r, g, b, star; };
    struct bd draws[MAX_VISIBLE_SPHERES];
    int ndraw = 0;
    for (int i = 0; i < state.sphere_count && ndraw < MAX_VISIBLE_SPHERES; i++) {
        sphere_t* s = &state.spheres[i];
        if (!s->active) continue;
        float dx = (float)s->x - pos_x;
        float dy = (float)s->y - pos_y;
        if (dx*dx + dy*dy > (float)(VISIBLE_RANGE*VISIBLE_RANGE)) continue;
        float center[3];
        ball_center((float)s->x, (float)s->y, pos_x, pos_y, state.vis_angle, center);
        float cx, cy, hx, hy, depth;
        if (!project_ball(center, aspect, &cx, &cy, &hx, &hy, &depth)) continue;
        struct bd* d = &draws[ndraw++];
        d->cx = cx; d->cy = cy; d->hx = hx; d->hy = hy; d->depth = depth;
        d->star = 0.0f;
        if (s->type == SPH_RED)       { d->r = 0.95f; d->g = 0.15f; d->b = 0.15f; }
        else if (s->type == SPH_RING) { d->r = 1.00f; d->g = 0.84f; d->b = 0.10f; } // gold
        else if (s->type == SPH_STAR) { d->r = 0.92f; d->g = 0.92f; d->b = 0.95f;   // white
                                        d->star = 1.0f; }                            // + red star marker
        else                          { d->r = 0.15f; d->g = 0.35f; d->b = 0.95f; } // blue
    }
    // sort back-to-front (farthest first) so nearer balls overlap correctly.
    for (int a = 0; a < ndraw; a++)
        for (int b = a + 1; b < ndraw; b++)
            if (draws[b].depth > draws[a].depth) {
                struct bd tmp = draws[a]; draws[a] = draws[b]; draws[b] = tmp;
            }

    sg_begin_pass(&(sg_pass){ .action = state.pass_action, .swapchain = sglue_swapchain() });

    // floor + sky
    sg_apply_pipeline(state.pip);
    sg_apply_bindings(&state.bind);
    sg_apply_uniforms(UB_fs_params, &SG_RANGE(fsp));
    sg_draw(0, 3, 1);

    // sphere billboards, back-to-front
    sg_apply_pipeline(state.ball_pip);
    sg_apply_bindings(&state.ball_bind);
    for (int i = 0; i < ndraw; i++) {
        ball_vs_t bvs = {
            .center   = { draws[i].cx, draws[i].cy },
            .halfsize = { draws[i].hx, draws[i].hy },
        };
        ball_fs_t bfs = { .color = { draws[i].r, draws[i].g, draws[i].b, draws[i].star } };
        sg_apply_uniforms(UB_ball_vs, &SG_RANGE(bvs));
        sg_apply_uniforms(UB_ball_fs, &SG_RANGE(bfs));
        sg_draw(0, 6, 1);
    }

    // player billboard, pinned to the lower-center of the screen. when jumping,
    // lift it on screen by the current height so the jump reads visually. the
    // scale factor maps world height (~0..0.5) to a clip-space rise.
    float jump_rise = state.height * 1.2f;
    player_vs_t pvs = {
        .center   = { 0.0f, -0.32f + jump_rise },
        .halfsize = { 0.16f / aspect, 0.16f },
    };
    player_fs_t pfs = { .phase = state.player_phase };
    sg_apply_pipeline(state.player_pip);
    sg_apply_bindings(&state.player_bind);
    sg_apply_uniforms(UB_player_vs, &SG_RANGE(pvs));
    sg_apply_uniforms(UB_player_fs, &SG_RANGE(pfs));
    sg_draw(0, 6, 1);

    sg_end_pass();
    sg_commit();
}

static void event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat) {
        switch (e->key_code) {
            // queue a turn; latest press wins, fires at the next node.
            case SAPP_KEYCODE_LEFT:  case SAPP_KEYCODE_A: if (!state.game_over && !state.won) state.pending_turn = -1; break;
            case SAPP_KEYCODE_RIGHT: case SAPP_KEYCODE_D: if (!state.game_over && !state.won) state.pending_turn = +1; break;
            // forward arrow: recover to forward motion after a star bounce.
            case SAPP_KEYCODE_UP: case SAPP_KEYCODE_W:
                if (!state.game_over && !state.won) state.forward_queued = true;
                break;
            // jump (also restarts after a win/loss).
            case SAPP_KEYCODE_SPACE: case SAPP_KEYCODE_Z: case SAPP_KEYCODE_X:
                if (state.game_over || state.won) reset_game();   // restart
                else state.jump_queued = true;                    // jump
                break;
            case SAPP_KEYCODE_ESCAPE: sapp_request_quit(); break;
            default: break;
        }
    }
}

static void cleanup(void) {
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    return (sapp_desc){
        .init_cb    = init,
        .frame_cb   = frame,
        .event_cb   = event,
        .cleanup_cb = cleanup,
        .width      = 800,
        .height     = 600,
        .window_title = "Blue Spheres",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}