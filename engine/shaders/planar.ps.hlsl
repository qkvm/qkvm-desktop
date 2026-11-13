#include "tile.hlsli"

// A picture decoded in software: three planes, each a texture of its own.
Texture2D<float> lumaPlane : register(t0);
Texture2D<float> bluePlane : register(t1);
Texture2D<float> redPlane : register(t2);
SamplerState smooth : register(s0);

float4 main(Corner corner) : SV_Target
{
    float4 colour = float4(lumaPlane.Sample(smooth, corner.at), bluePlane.Sample(smooth, corner.at),
                           redPlane.Sample(smooth, corner.at), 1);
    return float4(dot(colour, toRed), dot(colour, toGreen), dot(colour, toBlue), 1);
}
