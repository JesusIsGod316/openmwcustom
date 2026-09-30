#version 120
attribute vec3 previousPosition;
uniform mat4 currentModelViewProjection;
uniform mat4 previousModelViewProjection;
varying vec4 previousClip;
void main()
{
    gl_Position = currentModelViewProjection * gl_Vertex;
    previousClip = previousModelViewProjection * vec4(previousPosition, 1.0);
}
