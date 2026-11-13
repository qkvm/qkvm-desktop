#include "tile.hlsli"

// A tile that has no picture yet, and what a picture leaves free of its tile.
float4 main(Corner corner) : SV_Target
{
    return fill;
}
