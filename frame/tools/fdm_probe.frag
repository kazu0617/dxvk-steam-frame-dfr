#version 450
layout(location = 0) out vec4 color;
void main() {
  // Encode the pixel position at which this fragment was shaded into the colour so coarse
  // fragments (one invocation covering several pixels) show up as runs of identical pixels.
  uvec2 c = uvec2(gl_FragCoord.xy * 4.0);
  color = vec4(float(c.x & 255u) / 255.0, float((c.x >> 8) & 255u) / 255.0, float(c.y & 255u) / 255.0, float((c.y >> 8) & 255u) / 255.0);
}
