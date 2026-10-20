#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 0) out vec4 out_colour;
void main() {
    // The atlas is coverage only; the colour comes from the vertex.
    out_colour = vec4(v_colour.rgb, v_colour.a * texture(atlas, v_uv).r);
}
