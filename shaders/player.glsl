//------------------------------------------------------------------------------
//  player.glsl  (sokol-shdc input)
//
//  The player sprite: a screen-space quad that samples from a sprite sheet
//  texture with proper alpha transparency. Frame selection is done by offsetting
//  the UV x coordinate.
//------------------------------------------------------------------------------

//== VERTEX SHADER =============================================================
@vs vs_player
layout(location=0) in vec2 corner;     // 0..1
out vec2 luv;

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

layout(binding=0) uniform texture2D sprite_tex;
layout(binding=0) uniform sampler sprite_smp;

layout(binding=1) uniform player_fs {
    float frame;        // current frame index (0-based, integer)
    float total_frames;  // total slots in the sheet
    float _pad0;
    float _pad1;
};

void main() {
    float frame_width = 1.0 / total_frames;
    float u = floor(frame) * frame_width + luv.x * frame_width;
    float v = 1.0 - luv.y;   // flip y (sheet has y=0 at top)

    vec4 texel = texture(sampler2D(sprite_tex, sprite_smp), vec2(u, v));

    if (texel.a < 0.5) discard;   // transparent background
    frag_color = texel;
}
@end

@program player vs_player fs_player
