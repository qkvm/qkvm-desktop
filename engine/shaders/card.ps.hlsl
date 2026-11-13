#include "tile.hlsli"

// A picture decoded on the card: one slice of the decoder's own texture, seen once as its luma and
// once as its two colour differences side by side.
Texture2DArray<float> lumaPlane : register(t0);
Texture2DArray<float2> colourPlane : register(t1);
SamplerState smooth : register(s0);

float4 main(Corner corner) : SV_Target
{
    float3 at = float3(corner.at, 0);
    float4 colour = float4(lumaPlane.Sample(smooth, at), colourPlane.Sample(smooth, at), 1);
    return float4(dot(colour, toRed), dot(colour, toGreen), dot(colour, toBlue), 1);
}
