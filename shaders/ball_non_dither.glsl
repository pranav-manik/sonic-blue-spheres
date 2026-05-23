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

void main() {
    // disc coords -1..1 centered
    vec2 d = luv * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;                 // outside the circle -> transparent

    // fake-sphere normal. Light comes from the UPPER-LEFT (note the -d.y flip),
    // so the bright shine sits upper-left and the ball falls off to a near-black
    // shadow at the bottom -- matching the classic Blue Spheres look.
    float z = sqrt(max(0.0, 1.0 - r2));
    vec3 n = normalize(vec3(d.x, -d.y, z));
    vec3 L = normalize(vec3(-0.5, -0.55, 0.65));
    float diff = dot(n, L);                 // signed: negative = shadow side

    // body: blend from a near-black shadow up to the lit base color.
    vec3 base   = color.rgb;                          // lit colour
    vec3 shadow = color.rgb * 0.10 + vec3(0.0, 0.0, 0.03);  // near-black, slight navy
    float t = clamp(diff * 0.5 + 0.5, 0.0, 1.0);
    vec3 col = mix(shadow, base, t);

    // crisp white specular shine in the upper-left
    float spec = pow(max(0.0, diff), 32.0);
    col += vec3(1.0) * spec;

    frag_color = vec4(col, 1.0);
}
@end

@program ball vs_ball fs_ball
