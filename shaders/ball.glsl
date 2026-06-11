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
    vec4  color;    // rgb=color, a: 0=sphere, >=0.5=bumper, >=1.5=ring
    float spin;     // ring spin angle (radians)
    float tilt;     // unused for 3D rings
    float _pad1;
    float _pad2;
    vec4  tc;       // torus center in world space (w unused)
    vec4  ta;       // torus axis / surface normal (w unused)
};

// Camera matching sphere.glsl exactly
const vec3  CAM    = vec3(0.0, 1.6, 1.6);
const vec3  TARGET = vec3(0.0, 6.0, -7.0);
const float FOCAL  = 1.0;

// Analytic torus ray intersection.
// Torus defined by center C, axis A (unit), major radius R, tube radius r.
// Returns the nearest positive t along ray (ro + t*rd), or -1 if no hit.
// Based on Inigo Quilez's analytic torus intersection.
float torus_intersect(vec3 ro, vec3 rd, vec3 C, vec3 A, float R, float r) {
    vec3 o = ro - C;

    // project ray into torus frame: u along A, v/w in the plane
    float dA = dot(rd, A);
    float oA = dot(o, A);

    // quartic coefficients (Inigo Quilez method)
    float R2 = R*R, r2 = r*r;
    float od = dot(o, o);
    float dd = dot(rd, rd);  // should be 1
    float od_ = od - r2 - R2;

    float a = dd*dd;
    float b = 4.0*dd*dot(o,rd);
    float c = 2.0*dd*od_ + 4.0*dot(o,rd)*dot(o,rd) + 4.0*R2*dA*dA;
    float d = 4.0*od_*dot(o,rd) + 8.0*R2*oA*dA;
    float e = od_*od_ - 4.0*R2*(r2 - oA*oA);

    // solve quartic numerically with a few Newton iterations seeded from
    // the sphere (major+tube) bounding intersection
    // First find sphere bound t
    float bb = dot(o, rd);
    float cc = od - (R+r)*(R+r);
    float disc = bb*bb - cc;
    if (disc < 0.0) return -1.0;
    float t0 = -bb - sqrt(disc);
    float t1 = -bb + sqrt(disc);
    if (t1 < 0.001) return -1.0;
    float t = (t0 > 0.001) ? t0 : (t1 * 0.5);

    // 8 Newton steps
    for (int i = 0; i < 8; i++) {
        float f  = e + t*(d + t*(c + t*(b + t*a)));
        float df = d + t*(2.0*c + t*(3.0*b + t*4.0*a));
        if (abs(df) < 1e-7) break;
        t -= f/df;
    }

    if (t < 0.001) return -1.0;

    // verify it's actually on the torus — tight tolerance to kill speckles
    vec3 p = o + t*rd;
    float pA = dot(p, A);
    vec3 pP = p - pA*A;
    float rP = length(pP);
    if (abs(rP - R) > r * 1.1) return -1.0;

    // also verify the residual of the quartic is small
    float f = e + t*(d + t*(c + t*(b + t*a)));
    if (abs(f) > 0.01) return -1.0;

    return t;
}

vec3 torus_normal(vec3 p, vec3 C, vec3 A, float R) {
    vec3 o = p - C;
    float oA = dot(o, A);
    vec3 oP = o - oA*A;           // projection onto equatorial plane
    vec3 closest = normalize(oP) * R; // closest point on ring centreline
    return normalize(o - closest);
}

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

void main() {
    vec2 d = luv * 2.0 - 1.0;

    // --- 3D torus ring ---
    if (color.a >= 1.5) {
        // reconstruct camera ray for this fragment
        float aspect = tc.w;   // passed from C
        vec3 fwd   = normalize(TARGET - CAM);
        vec3 right = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
        vec3 up    = cross(right, fwd);
        vec3 rd    = normalize(fwd + d.x * FOCAL * right + d.y * FOCAL * up);
        vec3 ro    = CAM;

        vec3 C = tc.xyz;
        vec3 surf = ta.xyz;
        vec3 wup = vec3(0.0, 1.0, 0.0);
        // spin rotates the axis between face-on and edge-on each half cycle
        vec3 face_axis = normalize(cross(wup, surf));
        vec3 edge_axis = wup;
        float blend = cos(spin) * 0.5 + 0.5;
        vec3 A = normalize(mix(face_axis, edge_axis, blend));

        // torus radii: R = ring radius (how big the ring is), r = tube radius
        float R = 0.22 * 1.22;
        float r = 0.22 * 0.32;

        // spin the ring around its axis: rotate a reference vector in the plane
        // The spin doesn't change the torus shape, only the texture orientation
        // (since we have no texture, spin is visual-only via shading variation)

        float t = torus_intersect(ro, rd, C, A, R, r);
        if (t < 0.0) discard;

        vec3 hitpos = ro + t * rd;
        vec3 hn = torus_normal(hitpos, C, A, R);

        // compute the angular position of the hit point around the ring (0..2pi)
        // by projecting onto two orthogonal axes in the ring's plane
        vec3 o3 = hitpos - C;
        vec3 oP = o3 - dot(o3, A) * A;  // project onto ring plane
        // build a stable reference axis in the ring plane
        vec3 ref = normalize(cross(A, vec3(0.0, 1.0, 0.0)));
        vec3 ref2 = cross(A, ref);
        float ring_angle = atan(dot(oP, ref2), dot(oP, ref));

        // spinning highlight band: a bright stripe that sweeps around the ring
        float sweep = fract((ring_angle + spin) / 6.2831853);
        float band = exp(-pow(sweep - 0.5, 2.0) * 80.0);  // narrow bright band
        float band2 = exp(-pow(sweep - 0.0, 2.0) * 80.0); // second band opposite

        // base shading from normal
        vec3 L = normalize(vec3(-0.3, 0.8, 0.5));
        float diff = clamp(dot(hn, L) * 0.5 + 0.5, 0.0, 1.0);

        vec3 c = color.rgb;
        vec3 pal0 = c * 0.16;
        vec3 pal1 = c * 0.45;
        vec3 pal2 = c * 0.78;
        vec3 pal3 = c;
        vec3 pal4 = mix(c, vec3(1.0, 0.98, 0.7), 0.55);

        // combine base lighting with sweep band
        float lev = clamp(diff + band * 0.8 + band2 * 0.3, 0.0, 1.0);
        float fidx = lev * 4.0;
        int lo = int(floor(fidx));
        float frac2 = fidx - float(lo);
        const float PIX = 2.0;
        ivec2 sp = ivec2(gl_FragCoord.xy / PIX);
        float th = bayer4(sp);
        int idx = (frac2 > th) ? (lo + 1) : lo;
        vec3 pal[5] = vec3[5](pal0, pal1, pal2, pal3, pal4);
        if (idx > 4) idx = 4; if (idx < 0) idx = 0;
        vec3 col = pal[idx];
        col = clamp(col, 0.0, 1.0);

        frag_color = vec4(col, 1.0);
        return;
    }

    // --- sphere / bumper ---
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
        // Perspective star placement: the star sits on the sphere's equator
        // facing the camera.  From the top-down view angle used here, close
        // spheres are seen almost from above so the star appears near the
        // bottom; far spheres are seen more horizontally so it sits centred.
        // Derive the elevation angle (how steeply the camera looks down at
        // this sphere) from the world-space position in tc.xyz, then map it
        // to a downward UV offset in the billboard's local space.
        vec3 fwd_c   = normalize(TARGET - CAM);
        vec3 right_c = normalize(cross(fwd_c, vec3(0.0, 1.0, 0.0)));
        vec3 up_c    = cross(right_c, fwd_c);
        // elev > 0  →  camera is above the sphere's sight-line (looking down)
        // elev < 0  →  sphere is above/beyond horizon (looking up at it)
        float elev      = dot(normalize(CAM - tc.xyz), up_c);
        // Remap the full visible elev range (≈−0.45 at horizon → +0.43 at
        // nearest sphere) to star_shift 0→0.42, so the perspective lean starts
        // as soon as a sphere is visible rather than only for the closest 2 cells.
        float star_shift = clamp((elev + 0.45) * 0.47, 0.0, 0.42);
        // d.y + shift  moves the sample point up, making the star *appear* lower
        if (in_star(vec2(d.x, d.y + star_shift), 0.62)) {
            float sh = 0.55 + 0.45 * clamp(diff, 0.0, 1.0);
            col = vec3(0.85, 0.10, 0.10) * sh;
        }
    }

    frag_color = vec4(col, 1.0);
}
@end

@program ball vs_ball fs_ball
