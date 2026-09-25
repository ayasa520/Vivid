#include "common.h"
// [COMBO] {"combo":"BULK_KIND","default":0}
uniform sampler2D g_Texture0;
uniform float g_Tag; // {"material":"tag","default":1.0}
uniform float g_Gain; // {"material":"gain","default":0.125}
uniform vec2 g_Pair; // {"material":"pair","default":"0.25 0.375"}
uniform vec3 g_Triple; // {"material":"triple","default":"0.25 0.375 0.5"}
uniform vec4 g_Quad; // {"material":"quad","default":"0.25 0.375 0.5 0.625"}
uniform float g_Angle; // {"material":"angle","default":0,"conversion":"rad2deg"}
uniform float g_PlainAngle; // {"material":"plainangle","default":0,"conversion":"RAD2DEG"}
uniform float g_Blend; // {"material":"blending","default":0.125}
uniform float g_Cull; // {"material":"cullmode","default":0.125}
uniform float g_RasterAlpha; // {"material":"alphawriting","default":0.125}
uniform float g_DepthTest; // {"material":"depthtest","default":0.125}
uniform float g_DepthWrite; // {"material":"depthwrite","default":0.125}
varying vec2 v_TexCoord;

float scalarMarker(float value) {
    // Encode nonfinite values and negative zero through their stored bits, avoiding
    // arithmetic on NaNs and making the submitted scalar observable in the frame.
    uint bits = asuint(value);
    uint magnitude = bits & 0x7fffffffu;
    if (magnitude > 0x7f800000u) return 0.9375;
    if (magnitude == 0x7f800000u) return (bits & 0x80000000u) != 0u ? 0.8125 : 0.875;
    if (bits == 0x80000000u) return 0.25;
    return clamp(0.5 + value * 0.125, 0.0625, 0.75);
}

void main() {
    vec4 source = texSample2D(g_Texture0, v_TexCoord);
    vec3 marker;
#if BULK_KIND == 0 || BULK_KIND == 1
    float scalar = scalarMarker(g_Gain);
    marker = vec3(scalar, g_Tag * 0.0625, 0.75 - scalar * 0.25);
#elif BULK_KIND == 2
    marker = vec3(0.125 + g_Pair * 0.625, g_Tag * 0.0625);
#elif BULK_KIND == 3
    marker = vec3(0.125, 0.125, 0.125) + g_Triple * 0.625;
#elif BULK_KIND == 4
    marker = vec3(0.125 + g_Quad.x * 0.5 + g_Quad.w * 0.125,
                  0.125 + g_Quad.y * 0.5, 0.125 + g_Quad.z * 0.5);
#elif BULK_KIND == 5
    float scalar = scalarMarker(g_Angle);
    marker = vec3(scalar, g_Tag * 0.0625, 0.75 - scalar * 0.25);
#elif BULK_KIND == 6
    float scalar = scalarMarker(g_PlainAngle);
    marker = vec3(scalar, g_Tag * 0.0625, 0.75 - scalar * 0.25);
#else
    marker = vec3(0.125 + (g_Blend + g_Cull + g_RasterAlpha + g_DepthTest + g_DepthWrite) * 0.125,
                  0.5, 0.875);
#endif
    float band = floor(min(v_TexCoord.x, 0.9999) * 8.0) + 1.0;
    gl_FragColor = vec4(abs(band - g_Tag) < 0.25 ? marker : source.rgb, source.a);
}
