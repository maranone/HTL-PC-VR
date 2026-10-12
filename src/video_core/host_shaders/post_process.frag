// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    // Above 0, the picture is sharpened by so much (up to 1): for one that is shown larger
    // than it was drawn, which blurs it.
    float sharpen;
    // The target encodes what it is given for display by itself (an sRGB image): it has to be
    // given linear light, or the picture would be encoded twice.
    bool linear_out;
    vec4 source_uv;
    float contrast;
    float saturation;
    // Above 0, an SDR picture is shown on an HDR10 target: its white at hdr_paper_nits, and its
    // brightest part stretched up to hdr_peak_nits.
    float hdr_paper_nits;
    float hdr_peak_nits;
} pp;

// SDR as displayed (sRGB curve) to HDR10 (Rec.2020 primaries, PQ curve).
vec3 expand_hdr(vec3 shown_srgb) {
    vec3 linear = mix(pow((shown_srgb + 0.055) / 1.055, vec3(2.4)), shown_srgb / 12.92,
                      lessThanEqual(shown_srgb, vec3(0.04045)));
    float luma = dot(linear, vec3(0.2126, 0.7152, 0.0722));
    // Up to the knee as on an SDR display at paper white; past it the light grows smoothly
    // (no kink) until white reaches the peak.
    const float knee = 0.55;
    float over = max(luma - knee, 0.0) / (1.0 - knee);
    float nits = luma * pp.hdr_paper_nits +
                 max(pp.hdr_peak_nits - pp.hdr_paper_nits, 0.0) * over * over;
    vec3 rgb = linear * (luma > 1e-6 ? nits / luma : pp.hdr_paper_nits);
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164,
                             0.3293, 0.9195, 0.0880,
                             0.0433, 0.0114, 0.8956);
    vec3 y = pow(clamp(to2020 * rgb / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// What goes to the display for the picture at a place.
vec3 shown(vec2 at) {
    vec2 half_texel = min(0.5 / vec2(textureSize(texSampler, 0)), pp.source_uv.xy * 0.5);
    at = clamp(at, pp.source_uv.zw + half_texel,
               pp.source_uv.zw + pp.source_uv.xy - half_texel);
    vec3 rgb = textureLod(texSampler, at, 0.0).rgb;
    return pp.hdr ? rgb : gamma(rgb);
}

void main() {
    vec2 source = uv * pp.source_uv.xy + pp.source_uv.zw;
    // Clamp the sampling footprint to this eye, including sharpening taps at the seam.
    vec2 half_texel = min(0.5 / vec2(textureSize(texSampler, 0)), pp.source_uv.xy * 0.5);
    source = clamp(source, pp.source_uv.zw + half_texel,
                   pp.source_uv.zw + pp.source_uv.xy - half_texel);
    vec4 color_linear = texture(texSampler, source);
    vec3 here = pp.hdr ? color_linear.rgb : gamma(color_linear.rgb);
    if (pp.sharpen > 0.0) {
        // Contrast adaptive sharpening: a pixel is pushed away from the four next to it, the
        // more the less they differ already, and never beyond black or white.
        vec2 texel = 1.0 / vec2(textureSize(texSampler, 0));
        vec3 above = shown(source - vec2(0.0, texel.y));
        vec3 below = shown(source + vec2(0.0, texel.y));
        vec3 left = shown(source - vec2(texel.x, 0.0));
        vec3 right = shown(source + vec2(texel.x, 0.0));
        vec3 darkest = min(min(min(above, below), min(left, right)), here);
        vec3 brightest = max(max(max(above, below), max(left, right)), here);
        vec3 room = sqrt(clamp(min(darkest, 1.0 - brightest) / max(brightest, vec3(1e-5)),
                               0.0, 1.0));
        vec3 weight = room * (-1.0 / mix(8.0, 5.0, clamp(pp.sharpen, 0.0, 1.0)));
        here = clamp(((above + below + left + right) * weight + here) / (1.0 + 4.0 * weight),
                     0.0, 1.0);
    }
    if (!pp.hdr && (pp.contrast != 1.0 || pp.saturation != 1.0)) {
        // Grading in display terms (as QuestRetroDepth does): colour around its brightness,
        // then everything away from mid grey.
        float luma = dot(here, vec3(0.299, 0.587, 0.114));
        here = mix(vec3(luma), here, pp.saturation);
        here = clamp((here - 0.5) * pp.contrast + 0.5, 0.0, 1.0);
    }
    if (pp.hdr_paper_nits > 0.0 && !pp.hdr) {
        here = expand_hdr(here);
    } else if (pp.linear_out && !pp.hdr) {
        // Back to linear light, by the curve the target encodes with.
        here = mix(pow((here + 0.055) / 1.055, vec3(2.4)), here / 12.92,
                   lessThanEqual(here, vec3(0.04045)));
    }
    color = vec4(here, color_linear.a);
}
