//------------------------------------------------------------------------------
//  player.glsl  (sokol-shdc input)
//
//  The player billboard: a screen-space quad in the lower-center that draws a
//  spinning blue ball with red shoes (a placeholder for a Sonic sprite, in the
//  Blue-Spheres ball aesthetic). Pinned to the screen because the player never
//  moves -- the world scrolls under them. Animated via a `phase` uniform.
//
//  Swapping in a real PNG sprite later is a small change: replace the
//  procedural body in the fragment shader with a texture sample, keyed by the
//  current animation frame.
//------------------------------------------------------------------------------

//== VERTEX SHADER =============================================================
@vs vs_player
// a unit quad (0..1) that we place in clip space via the uniform rect.
layout(location=0) in vec2 corner;     // 0..1
out vec2 luv;                          // local 0..1 within the sprite

layout(binding=0) uniform player_vs {
    vec2 center;   // clip-space center of the sprite (-1..1)
    vec2 halfsize; // clip-space half extents
};

void main() {
    vec2 p = center + (corner * 2.0 - 1.0) * halfsize;
    gl_Position = vec4(p, 0.0, 1.0);
    luv = corner;
}
@end

//== FRAGMENT SHADER ===========================================================
@fs fs_player
in vec2 luv;
out vec4 frag_color;

layout(binding=1) uniform player_fs {
    float phase;   // run-cycle phase (radians)
};

float sdCirc(vec2 p, vec2 c, float r) { return length(p - c) - r; }
float sdCap(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

void main() {
    // local space with y up
    vec2 uv = vec2(luv.x, luv.y);
    float bob = sin(phase) * 0.015;
    float swing = sin(phase) * 0.06;

    vec2 c = vec2(0.5, 0.46 + bob);
    float R = 0.30;

    // red shoes poking out the bottom, swinging with the cycle
    float shoeL = sdCap(uv, vec2(c.x - 0.10, c.y - 0.22), vec2(c.x - 0.17 + swing, c.y - 0.40), 0.075);
    float shoeR = sdCap(uv, vec2(c.x + 0.10, c.y - 0.22), vec2(c.x + 0.17 - swing, c.y - 0.40), 0.075);
    float shoes = min(shoeL, shoeR);

    float ball = sdCirc(uv, c, R);

    vec4 col = vec4(0.0);   // transparent by default

    if (ball < 0.0) {
        vec2 nrm = (uv - c) / R;
        float lit = 0.45 + 0.55 * clamp(-nrm.x * 0.5 + nrm.y * 0.7 + 0.5, 0.0, 1.0);
        vec3 base = vec3(0.12, 0.32, 0.92) * lit;
        float band = sin((nrm.x + nrm.y) * 3.0 + phase * 3.0);
        if (band > 0.7) base += vec3(0.25, 0.25, 0.30);
        col = vec4(base, 1.0);
    } else if (ball < 0.02) {
        col = vec4(0.08, 0.20, 0.60, 1.0 - ball / 0.02);
    }

    if (shoes < 0.0) {
        col = vec4(0.90, 0.13, 0.10, 1.0);
    } else if (shoes < 0.015 && col.a < 0.5) {
        col = vec4(0.70, 0.10, 0.08, 1.0 - shoes / 0.015);
    }

    if (col.a < 0.01) discard;   // keep the floor visible around the sprite
    frag_color = col;
}
@end

@program player vs_player fs_player
