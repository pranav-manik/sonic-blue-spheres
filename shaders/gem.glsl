//------------------------------------------------------------------------------
//  gem.glsl -- Spinning 3D Chaos Emerald
//
//  Changes from previous:
//    • Flat table rectangle removed — crown tapers to a near-point at apex
//    • 7 horizontal facet lines (girdle + 3 crown + 3 pavilion)
//    • Width reduced 25% via bounding box
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

void main() {
    float ca = cos(spin), sa = sin(spin);
    float x = fc.x, y = fc.y;

    // ------------------------------------------------------------------
    // 1. Profile: no flat table — crown tapers linearly to apex
    //    y=+1.0 near-point, y=0.0 girdle (widest), y=-1.0 bottom point
    // ------------------------------------------------------------------
    float r;
    if (y >= 0.0) {
        r = max(0.0, 1.0 - y);          // crown: 1.0 at girdle → 0 at apex
    } else {
        r = max(0.0, y + 1.0);          // pavilion: same taper
    }

    float hw = r * (abs(ca) + abs(sa));
    if (abs(x) > hw + 0.005 || r < 0.008) discard;

    // ------------------------------------------------------------------
    // 2. Face selection (exact geometry, same as before)
    // ------------------------------------------------------------------
    float x_bnd = r * (sign(sa)*ca - sign(ca)*sa);

    vec3 nF0=vec3(-sa,0., ca), nF1=vec3(ca,0.,sa),
         nF2=vec3( sa,0.,-ca), nF3=vec3(-ca,0.,-sa);

    vec3 nl, nr;
    if      (ca>=0.&&sa>=0.) { nl=nF0; nr=nF1; }
    else if (ca< 0.&&sa>=0.) { nl=nF1; nr=nF2; }
    else if (ca< 0.&&sa< 0.) { nl=nF2; nr=nF3; }
    else                     { nl=nF3; nr=nF0; }

    vec3 face_n = (x <= x_bnd) ? nl : nr;

    // ------------------------------------------------------------------
    // 3. Normal tilt — crown has steeper angles now (no table to flatten)
    //    Divide crown into 3 bands matching the 3 facet lines
    // ------------------------------------------------------------------
    vec3 nrm;
    if (y >= 0.0) {
        float ny_tilt;
        if (y > 0.68) {
            // Near apex: very steep upward tilt
            ny_tilt = mix(1.10, 2.00, (y - 0.68) / 0.32);
        } else if (y > 0.35) {
            // Upper crown
            ny_tilt = mix(0.65, 1.10, (y - 0.35) / 0.33);
        } else {
            // Lower crown / near girdle
            ny_tilt = mix(0.0, 0.65, y / 0.35);
        }
        nrm = normalize(face_n + vec3(0.0, ny_tilt, 0.0));
    } else {
        // Pavilion: downward tilt, deeper near bottom
        float t = -y;
        float ny_tilt = -mix(0.0, 1.05, t * t);
        nrm = normalize(face_n + vec3(0.0, ny_tilt, 0.0));
    }

    // ------------------------------------------------------------------
    // 4. Lighting: main upper-left + fill from above
    // ------------------------------------------------------------------
    vec3 L1 = normalize(vec3(-0.65, 0.82, 0.52));
    vec3 L2 = normalize(vec3( 0.10, 0.95, 0.30));
    vec3 V  = vec3(0.0, 0.0, 1.0);

    float diff  = max(0.0, dot(nrm, L1)) * 0.72
                + max(0.0, dot(nrm, L2)) * 0.35;
    float spec  = pow(max(0.0, dot(normalize(L1+V), nrm)), 9.0) * 0.88
                + pow(max(0.0, dot(normalize(L2+V), nrm)), 9.0) * 0.55;

    float bright = diff + spec + 0.04;

    // ------------------------------------------------------------------
    // 5. Horizontal facet lines: girdle + 3 crown bands + 3 pavilion bands
    //    These are the key visual additions for the multi-face diamond look
    // ------------------------------------------------------------------
    float edge = 1.0;
    float ey = 0.018;
    float ex = 0.030 * (hw + 0.04);

    // Crown lines (3 bands above girdle)
    if (abs(y - 0.75) < ey)  edge = min(edge, 0.22);   // upper crown
    if (abs(y - 0.50) < ey)  edge = min(edge, 0.22);   // mid crown
    if (abs(y - 0.25) < ey)  edge = min(edge, 0.30);   // lower crown

    // Girdle (widest point)
    if (abs(y)        < ey)  edge = min(edge, 0.22);

    // Pavilion lines (3 bands below girdle)
    if (abs(y + 0.28) < ey)  edge = min(edge, 0.22);   // upper pavilion
    if (abs(y + 0.58) < ey)  edge = min(edge, 0.22);   // mid pavilion
    if (abs(y + 0.82) < ey)  edge = min(edge, 0.32);   // lower pavilion

    // Vertical face seam and pavilion centre line
    if (abs(x - x_bnd)          < ex)          edge = min(edge, 0.20);
    if (y < 0.0 && abs(x)       < ex * 0.65)   edge = min(edge, 0.20);

    bright *= edge;

    frag_color = vec4(gem_shade(bright), 1.0);
}
@end

@program gem vs_gem fs_gem
