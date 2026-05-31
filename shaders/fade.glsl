@vs vs_fade
layout(location=0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
@end

@fs fs_fade
out vec4 frag_color;
layout(binding=0) uniform fade_fs { float alpha; float _pad0; float _pad1; float _pad2; };
void main() { frag_color = vec4(0.0, 0.0, 0.0, alpha); }
@end

@program fade vs_fade fs_fade
