//------------------------------------------------------------------------------
//  sphere.glsl  (sokol-shdc input) -- THE FLOOR ONLY.
//
//  Real sphere projection (erbuka method): a ground sphere centered behind+below
//  the player; a flat checkerboard is bent onto it. The player is on the side
//  (no pole), so cells stay square and every edge is unambiguous geometry. The
//  grid rotation pivots around the player's foot point so turning spins in place.
//
//  The blue/red spheres are NOT drawn here -- they are separate billboard quads
//  (see player.glsl's sibling pipeline in main.c), so they can later be swapped
//  for real sprite textures.
//------------------------------------------------------------------------------
@ctype mat4 hmm_mat4

//== VERTEX SHADER =============================================================
@vs vs
layout(location=0) in vec2 position;
out vec2 uv;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    uv = position * 0.5 + 0.5;
}
@end

//== FRAGMENT SHADER ===========================================================
@fs fs
in vec2 uv;
out vec4 frag_color;

layout(binding=0) uniform fs_params {
    float aspect;
    vec2  scroll;     // player position on the flat grid (x, y)
    float tile_size;  // world units per checker cell
    float rot;        // player facing; rotates the grid so forward is up-screen
};

const float GR = 12.5;
const vec3  GC = vec3(0.0, 0.0, -12.5);
const vec3  CAM    = vec3(0.0, 1.6, 1.6);
const vec3  TARGET = vec3(0.0, 6.0, -7.0);
const float FOCAL  = 1.0;

vec3 checker(vec2 g) {
    float c = mod(floor(g.x) + floor(g.y), 2.0);
    if (c < 0.0) c += 2.0;
    vec3 darkTan  = vec3(0.55, 0.31, 0.05);
    vec3 lightTan = vec3(0.88, 0.64, 0.20);
    return mix(darkTan, lightTan, c);
}

vec3 shade_ground(vec3 hitn) {
    float k = -GC.z / hitn.z;
    vec2 gp = vec2(GC.x + hitn.x * k, GC.y + hitn.y * k);
    const vec2 PIVOT = vec2(0.0, 1.5);
    float cr = cos(rot), sr = sin(rot);
    vec2 p = gp - PIVOT;
    vec2 rp = vec2(p.x * cr + p.y * sr, -p.x * sr + p.y * cr);
    vec2 grid = (rp + scroll) / tile_size;
    vec3 col = checker(grid);
    float lit = 0.55 + 0.45 * max(0.0, dot(hitn, normalize(vec3(0.1, 0.6, 0.8))));
    return col * lit;
}

vec3 sky(float y) {
    return mix(vec3(0.60,0.82,1.0), vec3(0.18,0.52,0.95), clamp(y, 0.0, 1.0));
}

void main() {
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.x *= aspect;
    vec3 fwd   = normalize(TARGET - CAM);
    vec3 right = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
    vec3 up    = cross(right, fwd);
    vec3 rd = normalize(fwd + ndc.x * FOCAL * right + ndc.y * FOCAL * up);

    vec3 outc;
    vec3 oc = CAM - GC;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - GR * GR;
    float disc = b * b - c;
    bool hitground = false;
    if (disc >= 0.0) {
        float t = -b - sqrt(disc);
        if (t < 0.0) t = -b + sqrt(disc);
        if (t > 0.0) {
            vec3 hn = normalize((CAM + rd * t) - GC);
            if (hn.z > 0.0001) { outc = shade_ground(hn); hitground = true; }
        }
    }
    if (!hitground) outc = sky(uv.y);
    frag_color = vec4(outc, 1.0);
}
@end

@program sphere vs fs
