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
//------------------------------------------------------------------------------

@vs vs_gem
layout(location=0) in vec2 corner;
out vec2 fc;

layout(binding=0) uniform gem_vs {
    vec2 center;
    vec2 halfsize;
};

void main() {
    vec2 p = center + (corner * 2.0 - 1.0) * halfsize;
    gl_Position = vec4(p, 0.0, 1.0);
    fc = corner * 2.0 - 1.0;
}
@end

// ---------------------------------------------------------------------------
@fs fs_gem
in  vec2 fc;
out vec4 frag_color;

layout(binding=1) uniform gem_fs {
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

    float r;
    if (y >= 0.0) {
        float table_height = 0.95;
        float trap_start = 0.45;  // Start trapezoid taper at this height
        float base_r = 1.0;
        float table_r = 0.55;
        
        if (y > table_height) {
            // Top flat table
            r = mix(table_r, 0.0, (y - table_height) / (1.0 - table_height));
        } else if (y > trap_start) {
            // Trapezoid: taper from base_r to table_r
            r = mix(base_r, table_r, (y - trap_start) / (table_height - trap_start));
        } else {
            // Below trapezoid start: full width
            r = base_r;
        }
    } else {
        r = max(0.0, y + 1.0);
    }


    // Square silhouette approximation (close enough for octagon)
    float hw = r * (abs(ca) + abs(sa));
    if (abs(x) > hw + 0.005 || r < 0.008) discard;

    // ------------------------------------------------------------------
    // 2. Octagon face selection:
    //    8 face normals at angles k*π/4 in gem space.
    //    After Y-rotation by spin: world_nz = sin(k*π/4 + spin).
    //    Find the face whose screen-x range contains x.
    //
    //    Each face k occupies x ∈ [R_v*cos((k+0.5)*π/4+spin),
    //                                R_v*cos((k-0.5)*π/4+spin)]
    //    (left/right may be swapped depending on angle).
    //    Equivalent: the face whose normal is most aligned with the
    //    direction from x-axis rotation.
    //
    //    Shortcut: find the two front-facing faces that straddle x.
    //    For each k, face k "owns" x when:
    //      the angular position atan2(x_unprojected, z) is closest to k*π/4.
    //    For the approximate square silhouette, use a continuous face index:
    // ------------------------------------------------------------------

    // Continuous face angle from screen position (approximate)
    // Map x back to gem angle: for the dominant-face approach, use x/hw to get
    // approximate angular position within the visible semicircle.
    // More precisely: for a circular cross-section, face_angle = asin(x/r).
    // We use that as the per-face normal direction for the octagon.
    float x_norm = clamp(x / max(hw, 0.01), -1.0, 1.0);
    // Angle of the surface point in world space (approximate, for normal)
    float surf_angle = asin(x_norm) - spin;  // gem-space angle of this surface point

    // Round to nearest octagon face (multiples of π/4)
    float face_idx_f = surf_angle / 0.7854;      // π/4 ≈ 0.7854
    float face_angle = round(face_idx_f) * 0.7854;

    // Face normal in gem space: (cos(face_angle), 0, sin(face_angle))
    // Rotate to world space:
    float fn_gem_x = cos(face_angle);
    float fn_gem_z = sin(face_angle);
    vec3 face_n = vec3(fn_gem_x * ca - fn_gem_z * sa,
                       0.0,
                       fn_gem_x * sa + fn_gem_z * ca);

    // ------------------------------------------------------------------
    // 3. Normal tilt for crown / pavilion
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    // 4. Lighting
    // ------------------------------------------------------------------
    vec3 L1 = normalize(vec3(-0.65, 0.82, 0.52));
    vec3 L2 = normalize(vec3( 0.10, 0.95, 0.30));
    vec3 V  = vec3(0.0, 0.0, 1.0);

    float diff = max(0.0, dot(nrm, L1)) * 0.72
               + max(0.0, dot(nrm, L2)) * 0.35;
    float spec = pow(max(0.0, dot(normalize(L1+V), nrm)), 9.0) * 0.88
               + pow(max(0.0, dot(normalize(L2+V), nrm)), 9.0) * 0.55;

    float bright = diff + spec + 0.04;

    // ------------------------------------------------------------------
    // 5. Seam lines: 3 animated vertical lines from octagon vertices
    //    R_v = r/cos(π/8) ≈ 1.082 r  (octagon circumradius)
    //    Vertex j at world x = R_v * cos(j*π/4 + π/8 + spin)
    //    Show only interior vertices (not at silhouette edge).
    // ------------------------------------------------------------------
    float edge = 1.0;
    float R_v  = r * 1.0824;
    float eps_x = 0.030 * (hw + 0.04);
    float inner_limit = hw - eps_x * 3.5;  // exclude near-silhouette vertices

    for (int j = 0; j < 8; j++) {
        float vx = R_v * cos(float(j) * 0.7854 + 0.3927 + spin);
        if (abs(vx) < inner_limit && abs(x - vx) < eps_x)
            edge = min(edge, 0.22);
    }

    // Pavilion centre vertical line
    if (y < 0.0 && abs(x) < eps_x * 0.65)
        edge = min(edge, 0.20);

    bright *= edge;

    frag_color = vec4(gem_shade(bright), 1.0);
}
@end

@program gem vs_gem fs_gem
