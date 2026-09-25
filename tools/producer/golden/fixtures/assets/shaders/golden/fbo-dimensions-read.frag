#include "common.h"
uniform sampler2D g_Texture0;
uniform vec4 g_Texture0Resolution;
uniform vec2 g_Texture0Texel;
varying vec2 v_TexCoord;
void main() {
    // The two halves encode the sampled target's width and height as byte pairs.
    // Keep an actual texture read in the third channel so a detached or replaced
    // target must provide both current metadata and its submitted color contents.
    float dimension = v_TexCoord.x < 0.5 ? g_Texture0Resolution.x : g_Texture0Resolution.y;
    vec4 stored = texSample2D(g_Texture0, v_TexCoord + g_Texture0Texel * 0.25);
    gl_FragColor = vec4(mod(dimension, 256.0) / 255.0,
                        floor(dimension / 256.0) / 255.0, stored.r, 1.0);
}
