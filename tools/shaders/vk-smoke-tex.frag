#version 450
layout(location = 0) out vec4 colour;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
void main() { colour = texture(tex, uv); }
