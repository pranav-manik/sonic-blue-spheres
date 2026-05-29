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

#include "sonic_tex.h"   // embedded sprite sheet (RGBA pixel data)

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

#define MAX_LEVEL_SPHERES 256
#define MAX_VISIBLE_SPHERES 64
#define VISIBLE_RANGE 12
#define GRID_SIZE 32

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

static const int LEVEL_LAYOUT[][3] = {
    {0,3,0},{1,3,0},{2,3,0},{3,3,0},{4,3,0},
    {0,4,0},                        {4,4,0},
    {0,5,0},                        {4,5,0},
    {0,6,0},                        {4,6,0},
    {0,7,0},{1,7,0},{2,7,0},{3,7,0},{4,7,0},
    {1,4,0},{2,4,0},{3,4,0},
    {1,5,0},{2,5,0},{3,5,0},
    {1,6,0},{2,6,0},{3,6,0},
    {29,5,0},{29,6,0},{28,6,0},
    {0,1,3},
    {29,9,3},{3,9,3},
    {0,30,0},{0,31,0},{1,31,0},
};
#define LEVEL_LAYOUT_COUNT ((int)(sizeof(LEVEL_LAYOUT)/sizeof(LEVEL_LAYOUT[0])))

#define G_GR     12.5f
#define G_GCx    0.0f
#define G_GCy    0.0f
#define G_GCz   -12.5f
#define G_PIVOTx 0.0f
#define G_PIVOTy 1.223f
#define BALL_RADIUS_C 0.30f

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
    out[0] = sx + nx * BALL_RADIUS_C;
    out[1] = sy + ny * BALL_RADIUS_C;
    out[2] = sz + nz * BALL_RADIUS_C;
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

    sg_pipeline ball_pip;
    sg_bindings ball_bind;

    int   node_x, node_y;
    float frac;
    int   dir;
    int   pending_turn;
    float speed;

    int   move_sign;
    float bounce_dist;
    bool  forward_queued;

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
    bool     game_over;
    bool     won;
    bool     started;        // false until Up is pressed; game is frozen showing idle pose
    int      last_node_x, last_node_y;

    uint64_t last_time;
} state;

static void reset_game(void) {
    state.node_x = 0; state.node_y = 0;
    state.frac = 0.0f; state.dir = 0;
    state.pending_turn = 0; state.speed = 4.0f;
    state.move_sign = 1; state.bounce_dist = 1.0f;
    state.forward_queued = false;
    state.last_node_x = 0; state.last_node_y = 0;
    state.sphere_count = 0; state.blue_remaining = 0;
    state.rings = 0; state.game_over = false; state.won = false;
    state.started = false;
    for (int i = 0; i < LEVEL_LAYOUT_COUNT && i < MAX_LEVEL_SPHERES; i++) {
        sphere_t* s = &state.spheres[state.sphere_count];
        s->x = gwrap(LEVEL_LAYOUT[i][0]); s->y = gwrap(LEVEL_LAYOUT[i][1]);
        s->type = (sphere_type)LEVEL_LAYOUT[i][2]; s->active = true;
        state.sphere_count++;
        if (s->type == SPH_BLUE) state.blue_remaining++;
    }
    state.vis_angle = 0.0f; state.target_angle = 0.0f;
    state.turn_speed = 1.5707963f / 0.18f;
    state.turning = false; state.accum = 0.0;
    state.jumping = false; state.jump_total = 0.0f;
    state.jump_remaining = 0.0f; state.height = 0.0f;
    state.jump_queued = false;
    state.player_phase = 0.0f; state.player_frame = 0;
}

static void init(void) {
    sg_setup(&(sg_desc){ .environment = sglue_environment(), .logger.func = slog_func });
    stm_setup(); state.last_time = stm_now();
    reset_game();

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
}

static int sphere_at(int x, int y) {
    int wx = gwrap(x), wy = gwrap(y);
    for (int i = 0; i < state.sphere_count; i++) {
        sphere_t* s = &state.spheres[i];
        if (s->active && s->x == wx && s->y == wy) return i;
    }
    return -1;
}

static int g_conv_x[MAX_LEVEL_SPHERES], g_conv_y[MAX_LEVEL_SPHERES], g_conv_n;

static void flood_cluster_to_rings(int x, int y) {
    int wx = gwrap(x), wy = gwrap(y);
    int idx = sphere_at(wx, wy);
    if (idx < 0) return;
    sphere_t* s = &state.spheres[idx];
    if (s->type != SPH_BLUE) return;
    s->type = SPH_RING;
    if (state.blue_remaining > 0) state.blue_remaining--;
    if (g_conv_n < MAX_LEVEL_SPHERES) {
        g_conv_x[g_conv_n] = wx; g_conv_y[g_conv_n] = wy; g_conv_n++;
    }
    flood_cluster_to_rings(wx+1, wy); flood_cluster_to_rings(wx-1, wy);
    flood_cluster_to_rings(wx, wy+1); flood_cluster_to_rings(wx, wy-1);
}

static void convert_enclosed_to_rings(void) {
    static unsigned char reached[GRID_SIZE * GRID_SIZE];
    for (int i = 0; i < GRID_SIZE * GRID_SIZE; i++) reached[i] = 0;
    int stackx[GRID_SIZE*GRID_SIZE], stacky[GRID_SIZE*GRID_SIZE], sp = 0;

    // Seed the flood from EVERY empty cell (no sphere at all). These are
    // guaranteed to be outside any loop. On a torus, seeding from just one
    // point can't work because the flood wraps around and reaches the interior
    // from the other side. Seeding from all empties ensures that any blue
    // reachable from the outside (through empty or non-red cells) is found.
    for (int y = 0; y < GRID_SIZE; y++) {
        for (int x = 0; x < GRID_SIZE; x++) {
            int idx = sphere_at(x, y);
            // empty cell (no sphere) or a non-red sphere (blue/ring/star) — passable
            // We only want to seed from cells that are definitely outside: empty cells.
            if (idx < 0) {
                int li = y * GRID_SIZE + x;
                if (!reached[li]) {
                    reached[li] = 1;
                    stackx[sp] = x; stacky[sp] = y; sp++;
                }
            }
        }
    }

    // Now flood through passable cells (non-red). Red = walls.
    while (sp > 0) {
        sp--; int cx=stackx[sp], cy=stacky[sp];
        const int dx4[4]={1,-1,0,0}, dy4[4]={0,0,1,-1};
        for (int d=0;d<4;d++) {
            int nx=gwrap(cx+dx4[d]), ny=gwrap(cy+dy4[d]);
            int li=ny*GRID_SIZE+nx; if(reached[li]) continue;
            int idx=sphere_at(nx,ny);
            if(idx>=0 && state.spheres[idx].type==SPH_RED) continue;
            reached[li]=1; stackx[sp]=nx; stacky[sp]=ny; sp++;
        }
    }
    g_conv_n = 0;
    for (int i=0;i<state.sphere_count;i++) {
        sphere_t* s=&state.spheres[i];
        if(!s->active||s->type!=SPH_BLUE) continue;
        if(!reached[s->y*GRID_SIZE+s->x]) flood_cluster_to_rings(s->x,s->y);
    }
    if (g_conv_n > 0) {
        int wall_n=g_conv_n;
        const int wx8[8]={1,-1,0,0,1,1,-1,-1}, wy8[8]={0,0,1,-1,1,-1,1,-1};
        for(int c=0;c<wall_n;c++) for(int d=0;d<8;d++) {
            int rx=gwrap(g_conv_x[c]+wx8[d]), ry=gwrap(g_conv_y[c]+wy8[d]);
            int ri=sphere_at(rx,ry);
            if(ri>=0 && state.spheres[ri].type==SPH_RED) state.spheres[ri].type=SPH_RING;
        }
    }
}

static void touch_node(int nx, int ny) {
    if (state.height > JUMP_COLLIDE_H) return;
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
        s->active = false; state.rings++;
    } else if (s->type == SPH_STAR) {
        state.move_sign = -state.move_sign;
        state.bounce_dist = 0.0f;
        if (state.move_sign > 0 && state.frac > 0.5f) {
            state.node_x = gwrap(state.node_x + DIR_DX[state.dir]);
            state.node_y = gwrap(state.node_y + DIR_DY[state.dir]);
        }
        state.frac = 0.0f;
    }
    if (!state.game_over && state.blue_remaining == 0) state.won = true;
}

static void advance(float dist) {
    while (dist > 0.0f) {
        float room = state.move_sign > 0 ? (1.0f - state.frac) : state.frac;
        if (dist < room) {
            state.frac += (float)state.move_sign * dist;
            state.bounce_dist = fminf(1.0f, state.bounce_dist + dist);
            dist = 0.0f;
        } else {
            if (state.move_sign > 0) {
                state.node_x = gwrap(state.node_x + DIR_DX[state.dir]);
                state.node_y = gwrap(state.node_y + DIR_DY[state.dir]);
                state.frac = 0.0f;
            } else {
                state.node_x = gwrap(state.node_x - DIR_DX[state.dir]);
                state.node_y = gwrap(state.node_y - DIR_DY[state.dir]);
                state.frac = 1.0f;
            }
            dist -= room;
            state.bounce_dist = fminf(1.0f, state.bounce_dist + room);
            touch_node(state.node_x, state.node_y);
            if (state.game_over || state.won) return;
            if (!state.jumping && state.bounce_dist >= 1.0f && state.pending_turn != 0) {
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
    const float  step = state.speed * (float)FIXED_DT;

    while (state.accum >= FIXED_DT) {
        state.accum -= FIXED_DT;
        if (state.game_over || state.won || !state.started) { state.jump_queued = false; continue; }
        if (state.jump_queued && !state.jumping && !state.turning) {
            state.jumping = true;
            state.jump_total = JUMP_DISTANCE;
            state.jump_remaining = JUMP_DISTANCE;
        }
        state.jump_queued = false;
        if (state.move_sign < 0 && state.forward_queued &&
            state.bounce_dist >= 1.0f && !state.jumping) {
            state.move_sign = 1; state.forward_queued = false;
        }
        if (!state.turning || state.jumping) advance(step);
        if (state.jumping) {
            state.jump_remaining -= step;
            float dj = (state.jump_total - state.jump_remaining) / state.jump_total;
            dj = dj < 0.0f ? 0.0f : (dj > 1.0f ? 1.0f : dj);
            float arc = 1.0f - (2.0f*dj - 1.0f)*(2.0f*dj - 1.0f);
            state.height = arc * JUMP_HEIGHT;
            if (state.jump_remaining <= 0.0f) { state.jumping = false; state.height = 0.0f; }
        }
        float diff = state.target_angle - state.vis_angle;
        float maxstep = state.turn_speed * (float)FIXED_DT;
        if (diff >  maxstep) diff =  maxstep;
        if (diff < -maxstep) diff = -maxstep;
        state.vis_angle += diff;
        if (state.turning && fabsf(state.target_angle - state.vis_angle) < 1e-4f) {
            state.vis_angle = state.target_angle; state.turning = false;
        }
        if (!state.turning && !state.jumping) {
            state.player_phase += 9.0f * (float)FIXED_DT;
            if (state.player_phase > 6.2831853f) state.player_phase -= 6.2831853f;
            // run loop is frames 1..12 (sonic2-sonic13); frame 0 is idle-only
            state.player_frame = 1 + ((int)(state.player_phase / 6.2831853f * 12.0f)) % 12;
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

    struct bd { float cx, cy, hx, hy, depth, r, g, b, star; };
    struct bd draws[MAX_VISIBLE_SPHERES];
    int ndraw = 0;
    for (int i = 0; i < state.sphere_count && ndraw < MAX_VISIBLE_SPHERES; i++) {
        sphere_t* s = &state.spheres[i];
        if (!s->active) continue;
        float dx = gwrap_deltaf(pos_x, s->x), dy = gwrap_deltaf(pos_y, s->y);
        if (dx*dx + dy*dy > (float)(VISIBLE_RANGE*VISIBLE_RANGE)) continue;
        float sx = pos_x + dx, sy = pos_y + dy;
        float center[3]; ball_center(sx, sy, pos_x, pos_y, state.vis_angle, center);
        float cx, cy, hx, hy, depth;
        if (!project_ball(center, aspect, &cx, &cy, &hx, &hy, &depth)) continue;
        struct bd* d = &draws[ndraw++];
        d->cx=cx; d->cy=cy; d->hx=hx; d->hy=hy; d->depth=depth; d->star=0.0f;
        if (s->type == SPH_RED)       { d->r=0.95f; d->g=0.15f; d->b=0.15f; }
        else if (s->type == SPH_RING) { d->r=1.00f; d->g=0.84f; d->b=0.10f; }
        else if (s->type == SPH_STAR) { d->r=0.92f; d->g=0.92f; d->b=0.95f; d->star=1.0f; }
        else                          { d->r=0.15f; d->g=0.35f; d->b=0.95f; }
    }
    for (int a=0;a<ndraw;a++) for (int b=a+1;b<ndraw;b++)
        if (draws[b].depth > draws[a].depth) { struct bd tmp=draws[a]; draws[a]=draws[b]; draws[b]=tmp; }

    sg_begin_pass(&(sg_pass){ .action = state.pass_action, .swapchain = sglue_swapchain() });
    sg_apply_pipeline(state.pip); sg_apply_bindings(&state.bind);
    sg_apply_uniforms(UB_fs_params, &SG_RANGE(fsp)); sg_draw(0, 3, 1);

    sg_apply_pipeline(state.ball_pip); sg_apply_bindings(&state.ball_bind);
    for (int i = 0; i < ndraw; i++) {
        ball_vs_t bvs = { .center = { draws[i].cx, draws[i].cy },
                          .halfsize = { draws[i].hx, draws[i].hy } };
        ball_fs_t bfs = { .color = { draws[i].r, draws[i].g, draws[i].b, draws[i].star } };
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

    sg_end_pass(); sg_commit();
}

static void event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat) {
        switch (e->key_code) {
            case SAPP_KEYCODE_LEFT:  case SAPP_KEYCODE_A:
                if (!state.game_over && !state.won) state.pending_turn = -1; break;
            case SAPP_KEYCODE_RIGHT: case SAPP_KEYCODE_D:
                if (!state.game_over && !state.won) state.pending_turn = +1; break;
            case SAPP_KEYCODE_UP: case SAPP_KEYCODE_W:
                if (!state.game_over && !state.won) {
                    if (!state.started) state.started = true;
                    else if (state.move_sign < 0) state.forward_queued = true;
                }
                break;
            case SAPP_KEYCODE_SPACE: case SAPP_KEYCODE_Z: case SAPP_KEYCODE_X:
                if (state.game_over || state.won) reset_game();
                else state.jump_queued = true;
                break;
            case SAPP_KEYCODE_ESCAPE: sapp_request_quit(); break;
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