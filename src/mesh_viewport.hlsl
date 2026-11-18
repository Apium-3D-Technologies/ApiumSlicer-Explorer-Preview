cbuffer Scene : register(b0) {
    float4 projectX;
    float4 projectY;
    float4 projectZ;
    float4 lightAndDiffuse;
    float4 viewAndFacing;
    float4 colorAndAmbient;
    float4 shadeLimits;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    nointerpolation float3 color : COLOR;
};

VertexOutput vsMain(float3 position : POSITION, float4 normal : NORMAL) {
    const float4 local = float4(position, 1.0);
    const float diffuse = abs(dot(normal.xyz, lightAndDiffuse.xyz));
    const float facing = abs(dot(normal.xyz, viewAndFacing.xyz));
    const float shade = clamp(colorAndAmbient.w + lightAndDiffuse.w * diffuse + viewAndFacing.w * facing,
        shadeLimits.x, 1.0);

    VertexOutput output;
    output.position = float4(dot(local, projectX), dot(local, projectY), dot(local, projectZ), 1.0);
    output.color = colorAndAmbient.rgb * shade;
    return output;
}

float4 psMain(VertexOutput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
