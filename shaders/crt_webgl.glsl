// ---------------------------------------------------------------------------
//  crt.glsl  —  simple CRT post-process pass
//
//  Features
//    • barrel / pincushion distortion (screen curvature)
//    • scanlines (every-other-row darkening)
//    • phosphor dot mask (subtle per-column RGB tint)
//    • vignette (edge darkening)
//    • rounded-corner crop to black
//
//  Applied as a second fullscreen-triangle pass over the offscreen render of
//  the game.  Toggled on/off at runtime ('S' key) in main.c.
// ---------------------------------------------------------------------------

@vs vs_crt
layout(location=0) in vec2 position;
out vec2 uv;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    uv = position * 0.5 + 0.5;
}
@end

@fs fs_crt
in vec2 uv;
out vec4 frag_color;

layout(binding=0) uniform texture2D screen_tex;
layout(binding=0) uniform sampler   screen_smp;

layout(binding=0) uniform crt_params {
    float screen_h;   // framebuffer height (pixels) — drives scanline frequency
};

void main() {
    // ---- 1. Barrel distortion ------------------------------------------------
    // Map uv to centred [-1,1], apply radial warp, map back.
    vec2 c  = uv * 2.0 - 1.0;
    float r2 = dot(c, c);
    vec2 tc = (c * (1.0 + r2 * 0.10)) * 0.5 + 0.5;

    // Rounded-corner crop: outside the curved screen → black
    if (any(lessThan(tc, vec2(0.0))) || any(greaterThan(tc, vec2(1.0)))) {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Metal stores offscreen textures with Y=0 at the top, but the fullscreen
    // triangle has Y=0 at the bottom — flip when sampling to correct the flip.
    vec3 col = texture(sampler2D(screen_tex, screen_smp), tc).rgb;

    // ---- 2. Scanlines --------------------------------------------------------
    float scan = 0.78 + 0.22 * sin((1.0 - tc.y) * screen_h * 3.14159265);
    col *= scan;

    // ---- 3. Phosphor dot mask ------------------------------------------------
    // Cycle R / G / B every 3 horizontal pixels for a subtle subpixel tint.
    float px    = mod(gl_FragCoord.x, 3.0);
    float r_col = 1.0 - step(1.0, px);
    float g_col = step(1.0, px) * (1.0 - step(2.0, px));
    float b_col = step(2.0, px);
    vec3  mask  = vec3(r_col, g_col, b_col);
    // Blend 15 % toward the tinted mask — subtle, keeps overall brightness.
    col *= mix(vec3(1.0), 0.5 + 1.5 * mask, 0.15);

    // ---- 4. Vignette ---------------------------------------------------------
    vec2  vp   = uv - 0.5;
    float vign = 1.0 - dot(vp, vp) * 1.6;
    col *= clamp(vign, 0.0, 1.0);

    // ---- 5. Slight global lift to compensate for darkening -------------------
    col *= 1.10;

    frag_color = vec4(col, 1.0);
}
@end

@program crt vs_crt fs_crt
