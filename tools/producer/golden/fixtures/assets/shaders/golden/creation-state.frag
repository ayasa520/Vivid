#include "common.h"

uniform sampler2D g_Texture0;
uniform vec3 g_Tint; // {"material":"tint","default":"0.75 0.25 0.625"}
varying vec2 v_TexCoord;

void main() {
    vec4 source = texSample2D(g_Texture0, v_TexCoord);
    gl_FragColor = vec4(g_Tint, source.a);
}
