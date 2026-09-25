#include "common.h"
uniform sampler2D g_Texture0;
uniform float g_Tag; // {"material":"tag","default":1.0}
uniform vec3 g_Marker; // {"material":"marker","default":"0 0 0"}
varying vec2 v_TexCoord;
void main() {
    vec4 source = texSample2D(g_Texture0, v_TexCoord);
    float band = floor(min(v_TexCoord.x, 0.9999) * 8.0) + 1.0;
    gl_FragColor = vec4(abs(band - g_Tag) < 0.25 ? g_Marker : source.rgb, source.a);
}
