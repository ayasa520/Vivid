#include "common.h"
uniform float g_Write; // {"material":"write","default":1.0}
uniform vec3 g_SeedColor; // {"material":"color","default":"0.875 0.125 0.25"}
uniform float g_SeedAlpha; // {"material":"opacity","default":0.875}
varying vec2 v_TexCoord;
void main() {
    if (g_Write < 0.5) discard;
    gl_FragColor = vec4(g_SeedColor, g_SeedAlpha);
}
