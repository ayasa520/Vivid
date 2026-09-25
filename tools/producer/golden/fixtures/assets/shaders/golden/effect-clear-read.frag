#include "common.h"
uniform sampler2D g_Texture0;
uniform sampler2D g_Texture1;
uniform sampler2D g_Texture2;
varying vec2 v_TexCoord;
void main() {
    float band = floor(min(v_TexCoord.x, 0.9999) * 6.0);
    vec4 a = texSample2D(g_Texture0, v_TexCoord);
    vec4 b = texSample2D(g_Texture1, v_TexCoord);
    vec4 c = texSample2D(g_Texture2, v_TexCoord);
    vec4 value = band < 2.0 ? a : (band < 4.0 ? b : c);
    vec3 marker = mod(band, 2.0) < 0.5 ? value.rgb : vec3(value.a, value.a, value.a);
    gl_FragColor = vec4(marker, 1.0);
}
