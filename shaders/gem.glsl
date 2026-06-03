//------------------------------------------------------------------------------
//  gem.glsl -- Spinning 3D Chaos Emerald
//
//  Cross-section: regular OCTAGON (8 faces).
//    Square (4 faces) → 1 visible seam line at any time.
//    Octagon (8 faces) → 3 visible seam lines at any time, all animating.
//
//  Silhouette: r * (|cos θ| + |sin θ|) kept as-is (square approx — close
//  enough; the octagon is only ~8% wider at 45° vs 0°).
//  Seam lines: the 8 vertex positions R_v*cos(j*π/4 + π/8 + θ); the 3 that
//  fall inside the silhouette are the interior seams.
//
//  CHANGES:
//    - Spin slowed by 50%: on CPU side, multiply your spin increment by 0.5
//      (see gem_vs uniform: halfsize is halved → 50% smaller gem)
//    - Size reduced 50%: halfsize passed from CPU should be halved, OR
//      the shader scales fc by 2.0 to effectively shrink the rendered gem.
//------------------------------------------------------------------------------

@vs vs_gem
layout(location=0) in vec2 corner;
out vec2 fc;

layout(binding=0) uniform gem_vs {
    vec2 center;
    vec2 halfsize;
};

void main() {
    // Halve halfsize here to make the gem 50% smaller.
    // Multiply by 0.5 so the rendered quad footprint is half the original.
    vec2 p = center + (corner * 2.0 - 1.0) * halfsize * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
    fc = corner * 2.0 - 1.0;
}
@end

// ---------------------------------------------------------------------------
@fs fs_gem
in  vec2 fc;
out vec4 frag_color;

layout(binding=1) uniform gem_fs {
    // spin: pass (original_time * original_speed * 0.5) from CPU to slow by 50%.
    // i.e. wherever you did:  spin = elapsed * speed;
    //      change to:         spin = elapsed * speed * 0.5;
    float spin;
    float table_r;
};

float bayer_thr() {
    float B[16];
    B[0]= 0.5/16.; B[1]= 8.5/16.; B[2]= 2.5/16.; B[3]=10.5/16.;
    B[4]=12.5/16.; B[5]= 4.5/16.; B[6]=14.5/16.; B[7]= 6.5/16.;
    B[8]= 3.5/16.; B[9]=11.5/16.; B[10]=1.5/16.; B[11]= 9.5/16.;
    B[12]=15.5/16.;B[13]= 7.5/16.;B[14]=13.5/16.;B[15]= 5.5/16.;
    ivec2 p = ivec2(int(mod(gl_FragCoord.x,4.0)), int(mod(gl_FragCoord.y,4.0)));
    return B[p.y*4 + p.x];
}

vec3 gem_shade(float t) {
    vec3 C[6];
    C[0] = vec3(0.006, 0.085, 0.000);
    C[1] = vec3(0.025, 0.240, 0.010);
    C[2] = vec3(0.075, 0.480, 0.020);
    C[3] = vec3(0.200, 0.780, 0.035);
    C[4] = vec3(0.520, 0.970, 0.090);
    C[5] = vec3(0.920, 1.000, 0.540);
    float s = clamp(t, 0.0, 1.0) * 5.0;
    int   i = int(s), j = min(i+1, 5);
    return C[(s - float(i) > bayer_thr()) ? j : i];
}

// ---------------------------------------------------------------------------
void main() {
    float ca = cos(spin), sa = sin(spin);
    float x = fc.x, y = fc.y;

    // Aspect: gem is wider than tall. Stretch x coord.
    float ax = x * 0.80;   // compress x-sample so gem appears wider
    float ay = y;

    // Layout constants (in ay space)
    const float girdle_y    =  0.00;   // widest horizontal band
    const float crown_top   =  0.30;   // top of crown facets (short crown)
    const float table_top   =  0.42;   // top of table face
    const float pav_tip     = -1.00;   // bottom point of pavilion

    // Width profile: r(y) = half-width of gem cross-section at height y
    float r;
    if (ay > table_top) {
        // Above table: discard (flat top)
        discard;
    } else if (ay > crown_top) {
        // Table face — wide flat rectangle, slight taper to table width
        float table_w = 0.72;
        float girdle_w = 1.00;
        r = mix(girdle_w, table_w, (ay - crown_top) / (table_top - crown_top));
    } else if (ay >= girdle_y) {
        // Crown: full width at girdle, tapers up to crown_top
        r = mix(1.00, 1.00, (ay - girdle_y) / (crown_top - girdle_y));
        // Actually crown stays near full width (reference is very squat)
        r = 1.00;
    } else {
        // Pavilion: linear taper from full width at girdle down to point
        r = max(0.0, 1.0 + ay);  // y=0 → r=1, y=-1 → r=0
        // Slightly bulge the pavilion sides for Sonic gem look
        r = r * (1.0 + 0.08 * (1.0 - r));
    }

    float hw = r * (abs(ca) + abs(sa));
    if (abs(x) > hw + 0.005 || r < 0.008) discard;

    float x_norm = clamp(x / max(hw, 0.01), -1.0, 1.0);
    float surf_angle = asin(x_norm) - spin;

    float face_idx_f = surf_angle / 0.7854;
    float face_angle = round(face_idx_f) * 0.7854;

    float fn_gem_x = cos(face_angle);
    float fn_gem_z = sin(face_angle);
    vec3 face_n = vec3(fn_gem_x * ca - fn_gem_z * sa,
                       0.0,
                       fn_gem_x * sa + fn_gem_z * ca);

    vec3 nrm;
    if (y >= 0.0) {
        float ny_tilt;
        if (y > 0.68) {
            ny_tilt = mix(1.10, 2.00, (y - 0.68) / 0.32);
        } else if (y > 0.35) {
            ny_tilt = mix(0.65, 1.10, (y - 0.35) / 0.33);
        } else {
            ny_tilt = mix(0.0, 0.65, y / 0.35);
        }
        nrm = normalize(face_n + vec3(0.0, ny_tilt, 0.0));
    } else {
        float t = -y;
        nrm = normalize(face_n + vec3(0.0, -mix(0.0, 1.05, t * t), 0.0));
    }

    vec3 L1 = normalize(vec3(-0.65, 0.82, 0.52));
    vec3 L2 = normalize(vec3( 0.10, 0.95, 0.30));
    vec3 V  = vec3(0.0, 0.0, 1.0);

    float diff = max(0.0, dot(nrm, L1)) * 0.72
               + max(0.0, dot(nrm, L2)) * 0.35;
    float spec = pow(max(0.0, dot(normalize(L1+V), nrm)), 9.0) * 0.88
               + pow(max(0.0, dot(normalize(L2+V), nrm)), 9.0) * 0.55;

    float bright = diff + spec + 0.04;

    float edge = 1.0;
    float R_v  = r * 1.0824;
    float eps_x = 0.030 * (hw + 0.04);
    float inner_limit = hw - eps_x * 3.5;

    for (int j = 0; j < 8; j++) {
        float vx = R_v * cos(float(j) * 0.7854 + 0.3927 + spin);
        if (abs(vx) < inner_limit && abs(x - vx) < eps_x)
            edge = min(edge, 0.22);
    }

    if (y < 0.0 && abs(x) < eps_x * 0.65)
        edge = min(edge, 0.20);

    bright *= edge;

    frag_color = vec4(gem_shade(bright), 1.0);
}
@end

@program gem vs_gem fs_gem
