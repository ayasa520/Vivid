#include "common.h"

uniform sampler2D g_Texture0;
uniform float g_Level; // {"material":"level","default":0.125}
uniform float g_Tag; // {"material":"tag","default":1.0}
varying vec2 v_TexCoord;

void main() {
    vec4 source = texSample2D(g_Texture0, v_TexCoord);
    float band = floor(min(v_TexCoord.x, 0.9999) * 3.0) + 1.0;
    float selected = abs(band - g_Tag) < 0.25 ? 1.0 : 0.0;
    vec3 marker = vec3(g_Level, 0.125 * g_Tag, 0.75 - g_Level * 0.5);
    gl_FragColor = vec4(mix(source.rgb, marker, selected), source.a);
}
