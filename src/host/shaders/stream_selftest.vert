#version 450
// The command stream's self-test (host/stream_selftest.cpp): one triangle
// over the whole viewport; the scissor picks the rectangle drawn.
void main() {
    vec2 t = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0);
}
