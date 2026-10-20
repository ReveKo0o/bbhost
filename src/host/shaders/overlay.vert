#version 450
// The host overlay: the pointer, the text box that
// replaces the PS4's on-screen keyboard, and the options screen. Positions
// arrive in the game's display-buffer pixels, the same space host_mouse_state()
// reports, and the push constant turns them into clip space. Plain rectangles
// point at a lit texel in the font atlas, so one pipeline draws both.
layout(push_constant) uniform Push { vec2 inv_size; } pc;
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_colour;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;
void main() {
    gl_Position = vec4(in_pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
    v_uv = in_uv;
    v_colour = in_colour;
}
