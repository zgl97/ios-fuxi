#version 450
layout(location = 0) out vec4 colour;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
void main() {
  /* Sample the green texture, then halve alpha so the blend stage has work to do:
   * dst is the cleared (0,0,0,1) so src-over yields (0,128,0,128). */
  vec4 t = texture(tex, uv);
  colour = vec4(t.rgb, 0.5);
}
