#include "common.h"

uniform sampler2D g_Texture0;
uniform vec3 g_Tint; // {"material":"tint","default":"1 1 1"}
uniform float g_Tag; // {"material":"tag","default":1.0}
varying vec2 v_TexCoord;

void main() {
    vec4 source = texSample2D(g_Texture0, v_TexCoord);
    // Keep the selected effect's tag in both the material interface and real pixels.
    // The paired control supplies its own authored effect index and tint schedule.
    gl_FragColor = vec4(source.rgb * g_Tint * (0.5 + 0.0625 * g_Tag), source.a);
}
