@vs vs_fade
layout(location=0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
@end

@fs fs_fade
out vec4 frag_color;
layout(binding=0) uniform fade_fs {
    float alpha;
    float r; float g; float b;  // fade color (0,0,0=black  1,1,1=white)
};
void main() { frag_color = vec4(r, g, b, alpha); }
@end

@program fade vs_fade fs_fade
