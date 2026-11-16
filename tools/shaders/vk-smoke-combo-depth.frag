#version 450
layout(location = 0) out vec4 colour;
void main() {
  /* Drawn at z=0.75 with LESS against a 1.0 clear: must be rejected by the depth
   * test, so a pure-red full-screen result means depth testing did not run. */
  colour = vec4(1.0, 0.0, 0.0, 1.0);
}
