//------------------------------------------------------------------------------
//  ball.glsl  (sokol-shdc input)
//
//  A single blue/red sphere drawn as a 2D billboard quad (flat, faces the
//  camera) that LOOKS 3D via shading -- exactly like the original game's
//  pre-rendered sphere sprites. The C side computes each ball's screen-space
//  rect (center + size, from its projected 3D position) and draws one quad per
//  ball, back-to-front, so nearer balls overlap farther ones.
//
//  Swapping in a real sprite later: replace the procedural shading in the
//  fragment shader with a texture sample. The quad/placement plumbing stays.
//------------------------------------------------------------------------------

//== VERTEX SHADER =============================================================
@vs vs_ball
layout(location=0) in vec2 corner;     // 0..1 quad corner
out vec2 luv;

layout(binding=0) uniform ball_vs {
    vec2 center;     // clip-space center of this ball (-1..1)
    vec2 halfsize;   // clip-space half extents
};

void main() {
    vec2 p = center + (corner * 2.0 - 1.0) * halfsize;
    gl_Position = vec4(p, 0.0, 1.0);
    luv = corner;
}
@end

//== FRAGMENT SHADER ===========================================================
@fs fs_ball
in vec2 luv;
out vec4 frag_color;

layout(binding=1) uniform ball_fs {
    vec4 color;      // rgb = base color, a unused
};

// 4x4 Bayer ordered-dither threshold (0..15)/16. Classic for retro stipple.
float bayer4(ivec2 p) {
    int m[16] = int[16](0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5);
    int i = (p.y & 3) * 4 + (p.x & 3);
    return (float(m[i]) + 0.5) / 16.0;
}

void main() {
    // disc coords -1..1 centered
    vec2 d = luv * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;                 // outside the circle -> transparent

    // fake-sphere normal. Light from the UPPER-LEFT (note the -d.y flip), so the
    // shine sits upper-left and the ball darkens to a near-black bottom.
    float z = sqrt(max(0.0, 1.0 - r2));
    vec3 n = normalize(vec3(d.x, -d.y, z));
    vec3 L = normalize(vec3(-0.5, -0.55, 0.65));
    float diff = dot(n, L);

    // --- retro dithered shading -------------------------------------------
    // 5-step palette from a near-black shadow up to a bright lit tint, derived
    // from the ball's base color. Between steps we ordered-dither (Bayer) in
    // SCREEN pixel space so the stipple stays a constant chunk size and never
    // shimmers as the ball scales with distance.
    vec3 c = color.rgb;
    vec3 pal0 = c * 0.16 + vec3(0.0, 0.0, 0.04);   // deep shadow
    vec3 pal1 = c * 0.45;
    vec3 pal2 = c * 0.78;
    vec3 pal3 = c;                                  // lit base
    vec3 pal4 = mix(c, vec3(1.0), 0.55);            // bright rim

    float lev = clamp(diff * 0.5 + 0.5, 0.0, 1.0);
    float fidx = lev * 4.0;                          // 0..4 across 5 steps
    int lo = int(floor(fidx));
    float frac = fidx - float(lo);

    // dither chunk size in screen pixels (bigger = chunkier stipple)
    const float PIX = 2.0;
    ivec2 sp = ivec2(gl_FragCoord.xy / PIX);
    float th = bayer4(sp);
    int idx = (frac > th) ? (lo + 1) : lo;

    vec3 pal[5] = vec3[5](pal0, pal1, pal2, pal3, pal4);
    if (idx > 4) idx = 4;
    if (idx < 0) idx = 0;
    vec3 col = pal[idx];

    // crisp white shine in the upper-left, on top of the dithered body
    float spec = pow(max(0.0, diff), 40.0);
    if (spec > 0.5) col = vec3(1.0);

    frag_color = vec4(col, 1.0);
}
@end

@program ball vs_ball fs_ball
