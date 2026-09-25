#include "common.h"
uniform sampler2D g_Texture0;
uniform sampler2D g_Texture1; // {"default":"util/white"}
uniform vec3 g_Tint; // {"material":"tint","default":"0.85 0.35 0.15"}
uniform float g_Gain; // {"material":"gain","default":1.0}
uniform float g_Opacity; // {"material":"opacity","default":0.5}
varying vec2 v_TexCoord;
void main() {
#if DIRECTDRAW
    vec3 source = texSample2D(g_Texture0, v_TexCoord).rgb;
    vec3 detail = texSample2D(g_Texture1, v_TexCoord).rgb;
    gl_FragColor = vec4(source * detail * g_Tint * g_Gain, g_Opacity);
#else
    gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);
#endif
}
