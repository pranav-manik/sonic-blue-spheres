@vs vs_ball
layout(location=0) in vec2 corner;
out vec2 luv;

layout(binding=0) uniform ball_vs {
    vec2 center;
    vec2 halfsize;
};

void main() {
    vec2 p = center + (corner * 2.0 - 1.0) * halfsize;
    gl_Position = vec4(p, 0.0, 1.0);
    luv = corner;
}
@end

@fs fs_ball
in vec2 luv;
out vec4 frag_color;

layout(binding=1) uniform ball_fs {
    vec4 color;   // rgb=color, a: 0=sphere, >=0.5=star bumper, >=1.5=ring
    float spin;   // ring spin angle in radians
    float tilt;   // ring tilt (0=flat/ahead, 1=upright/side) — per ring
    float _pad1;
    float _pad2;
};

float bayer4(ivec2 p) {
    int m[16] = int[16](0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5);
    return (float(m[(p.y&3)*4+(p.x&3)]) + 0.5) / 16.0;
}

bool in_star(vec2 p, float scale) {
    float ang = atan(p.x, -p.y);
    float r = length(p) / scale;
    float seg = 6.28318530 / 5.0;
    float a = mod(ang, seg) / seg;
    float tri = abs(a - 0.5) * 2.0;
    return r < mix(0.45, 1.0, tri);
}

// Ring: top-down perspective spinning around vertical axis.
// The ring is always seen from ~40 degrees above (tilt compresses Y),
// and spins by compressing X with |cos(angle)|.
// At edge-on it becomes a thin horizontal sliver.
bool in_ring(vec2 d, float angle, float tilt_amt, out float shade) {
    float squeeze = abs(cos(angle));
    float ux = (squeeze > 0.03) ? (d.x / squeeze) : 100.0;
    float uy = d.y / tilt_amt;
    float r = length(vec2(ux, uy));
    shade = cos(angle);
    return r > 0.58 && r < 1.0;
}

void main() {
    vec2 d = luv * 2.0 - 1.0;

    // --- ring mode ---
    if (color.a >= 1.5) {
        float shade;
        if (!in_ring(d, spin, tilt, shade)) discard;

        float light = clamp(-d.y * 0.9 + 0.6, 0.0, 1.0);
        light = light * light;
        vec3 dark   = color.rgb * 0.20;
        vec3 bright = mix(color.rgb, vec3(1.0, 0.95, 0.6), 0.5);
        vec3 col = mix(dark, bright, light);

        frag_color = vec4(col, 1.0);
        return;
    }

    // --- sphere / bumper mode ---
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;

    float z = sqrt(max(0.0, 1.0 - r2));
    vec3 n = normalize(vec3(d.x, -d.y, z));
    vec3 L = normalize(vec3(-0.5, -0.55, 0.65));
    float diff = dot(n, L);

    vec3 c = color.rgb;
    vec3 pal0 = c * 0.16 + vec3(0.0, 0.0, 0.04);
    vec3 pal1 = c * 0.45;
    vec3 pal2 = c * 0.78;
    vec3 pal3 = c;
    vec3 pal4 = mix(c, vec3(1.0), 0.55);

    float lev = clamp(diff * 0.5 + 0.5, 0.0, 1.0);
    float fidx = lev * 4.0;
    int lo = int(floor(fidx));
    float frac = fidx - float(lo);
    const float PIX = 2.0;
    ivec2 sp = ivec2(gl_FragCoord.xy / PIX);
    float th = bayer4(sp);
    int idx = (frac > th) ? (lo + 1) : lo;
    vec3 pal[5] = vec3[5](pal0, pal1, pal2, pal3, pal4);
    if (idx > 4) idx = 4; if (idx < 0) idx = 0;
    vec3 col = pal[idx];

    float spec = pow(max(0.0, diff), 40.0);
    if (spec > 0.5) col = vec3(1.0);

    if (color.a >= 0.5) {
        if (in_star(d, 0.62)) {
            float sh = 0.55 + 0.45 * clamp(diff, 0.0, 1.0);
            col = vec3(0.85, 0.10, 0.10) * sh;
        }
    }

    frag_color = vec4(col, 1.0);
}
@end

@program ball vs_ball fs_ball
