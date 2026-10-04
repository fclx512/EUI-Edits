#version 450

layout(location = 0) out vec2 vUv;

vec2 positionForIndex(int index) {
    if (index == 0) {
        return vec2(-1.0, -1.0);
    }
    if (index == 1) {
        return vec2(3.0, -1.0);
    }
    return vec2(-1.0, 3.0);
}

vec2 uvForIndex(int index) {
    if (index == 0) {
        return vec2(0.0, 0.0);
    }
    if (index == 1) {
        return vec2(2.0, 0.0);
    }
    return vec2(0.0, 2.0);
}

void main() {
    gl_Position = vec4(positionForIndex(gl_VertexIndex), 0.0, 1.0);
    vUv = uvForIndex(gl_VertexIndex);
}
