#version 450
// The presenter's one pass (host/present_pass.cpp): one triangle over the
// viewport - the picture's rectangle in the swapchain image - carrying the
// coordinates of the display image's region it shows. t runs (0,0), (2,0),
// (0,2): the triangle's corners past the viewport are clipped away, and at
// the viewport's far edges (t = 1) the coordinates reach uv1.
layout(push_constant) uniform Push { vec2 uv0; vec2 uv1; } pc;
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 t = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0);
    v_uv = pc.uv0 + (pc.uv1 - pc.uv0) * t;
}
