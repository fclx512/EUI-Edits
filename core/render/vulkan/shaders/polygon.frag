#version 450

#define MAX_POLYGON_EDGES 128

layout(location = 0) in vec2 vLocalPos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) readonly buffer PolygonEdges {
    vec4 edges[];
} polygonEdges;

layout(push_constant) uniform PushConstants {
    vec4 windowAndOpacity;
    vec4 fillColor;
    vec4 edgeRange;
} pc;

float edgeDistance(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 0.0001), 0.0, 1.0);
    return length(p - (a + ab * t));
}

bool polygonContains(vec2 p, int edgeOffset, int edgeCount) {
    bool inside = false;
    for (int i = 0; i < MAX_POLYGON_EDGES; ++i) {
        if (i >= edgeCount) {
            break;
        }
        vec4 edge = polygonEdges.edges[edgeOffset + i];
        vec2 a = edge.xy;
        vec2 b = edge.zw;
        bool crosses = ((a.y > p.y) != (b.y > p.y));
        if (crosses) {
            float xAtY = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < xAtY) {
                inside = !inside;
            }
        }
    }
    return inside;
}

void main() {
    int edgeOffset = int(pc.edgeRange.x + 0.5);
    int edgeCount = int(pc.edgeRange.y + 0.5);
    bool inside = polygonContains(vLocalPos, edgeOffset, edgeCount);
    float minDistance = 1000000.0;
    for (int i = 0; i < MAX_POLYGON_EDGES; ++i) {
        if (i >= edgeCount) {
            break;
        }
        vec4 edge = polygonEdges.edges[edgeOffset + i];
        minDistance = min(minDistance, edgeDistance(vLocalPos, edge.xy, edge.zw));
    }

    float signedDistance = inside ? -minDistance : minDistance;
    float edgeWidth = max(fwidth(signedDistance), 0.75);
    float shapeAlpha = 1.0 - smoothstep(-edgeWidth, edgeWidth, signedDistance);
    if (shapeAlpha <= 0.0) {
        discard;
    }

    outColor = vec4(pc.fillColor.rgb, pc.fillColor.a * shapeAlpha * pc.windowAndOpacity.z);
}
