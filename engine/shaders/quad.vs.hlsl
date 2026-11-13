#include "tile.hlsli"

// The four corners of a tile, from nothing but their number: a strip of two triangles.
Corner main(uint number : SV_VertexID)
{
    float2 corner = float2(number & 1, number >> 1);
    Corner result;
    result.position = float4(lerp(place.x, place.z, corner.x), lerp(place.y, place.w, corner.y), 0, 1);
    result.at = float2(lerp(crop.x, crop.z, corner.x), lerp(crop.y, crop.w, corner.y));
    return result;
}
