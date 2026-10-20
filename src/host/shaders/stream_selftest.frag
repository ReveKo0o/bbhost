#version 450
// The command stream's self-test (host/stream_selftest.cpp): every pixel the
// value of the word a push descriptor's range starts at (DrawCmds::push_buffers,
// set 2).
layout(set = 2, binding = 0) readonly buffer Value { uint v[]; } value;
layout(location = 0) out uint color;
void main() {
    color = value.v[0];
}
