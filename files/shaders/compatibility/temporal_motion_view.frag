#version 120
uniform sampler2D lastShader;
varying vec2 uv;
void main()
{
    vec2 flow = texture2D(lastShader, uv).rg;
    gl_FragColor = vec4(clamp(vec2(0.5) + flow / 32.0, 0.0, 1.0), 0.5, 1.0);
}
