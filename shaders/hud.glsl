//------------------------------------------------------------------------------
//  hud.glsl -- pixel-perfect HUD digit rendering
//  Each digit/icon is a 4x5 pixel glyph from a 1D atlas texture.
//  The VS places a screen-space quad; the FS samples the glyph and discards
//  transparent pixels.
//------------------------------------------------------------------------------
@vs vs_hud
layout(location=0) in vec2 corner;
out vec2 luv;

layout(binding=0) uniform hud_vs {
    vec2 pos;       // bottom-left in clip space (-1..1)
    vec2 size;      // width/height in clip space
    vec2 uv0;       // atlas UV start
    vec2 uv1;       // atlas UV end
    float rotation; // radians; 0 = no rotation (default for HUD text/digits)
    float rot_pad;  // alignment padding
};

void main() {
    vec2 center = pos + size * 0.5;
    vec2 local  = (corner - vec2(0.5)) * size;
    float c = cos(rotation), s = sin(rotation);
    vec2 rot = vec2(local.x * c - local.y * s,
                    local.x * s + local.y * c);
    gl_Position = vec4(center + rot, 0.0, 1.0);
    luv = mix(uv0, uv1, corner);
}
@end

@fs fs_hud
in vec2 luv;
out vec4 frag_color;

layout(binding=0) uniform texture2D hud_tex;
layout(binding=0) uniform sampler hud_smp;

void main() {
    vec4 t = texture(sampler2D(hud_tex, hud_smp), luv);
    if (t.a < 0.5) discard;
    frag_color = t;
}
@end

@program hud vs_hud fs_hud
