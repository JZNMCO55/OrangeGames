#version 450

// Big-triangle fullscreen VS: 3 vertices cover [-1,1]^2 with no vertex buffer
// (positions synthesised from gl_VertexIndex). vUv is [0,1] across the screen.
layout(location = 0) out vec2 vUv;

void main()
{
    vec2 pos = vec2((gl_VertexIndex == 1) ?  3.0 : -1.0,
                    (gl_VertexIndex == 2) ?  3.0 : -1.0);
    vUv = (pos + vec2(1.0)) * 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
