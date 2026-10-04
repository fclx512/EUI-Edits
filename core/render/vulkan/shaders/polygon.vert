#version 450

layout(location = 0) in vec3 aScreenPos;
layout(location = 1) in vec2 aLocalPos;

layout(location = 0) out vec2 vLocalPos;

layout(push_constant) uniform PushConstants {
    vec4 windowAndOpacity;
    vec4 fillColor;
    vec4 edgeRange;
} pc;

void main() {
    vLocalPos = aLocalPos;
    vec2 ndc = vec2((aScreenPos.x / pc.windowAndOpacity.x) * 2.0 - 1.0,
                    (aScreenPos.y / pc.windowAndOpacity.y) * 2.0 - 1.0);
    gl_Position = vec4(ndc * aScreenPos.z, 0.0, aScreenPos.z);
}
