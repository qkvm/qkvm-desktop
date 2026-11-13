// What every shader of a tile is told about that tile. The same in the vertex and the pixel shader,
// and the same as struct TileConstants in scene_core.cpp.
cbuffer Tile : register(b0)
{
    float4 place;   // where the quad is: left, top, right, bottom, in clip space
    float4 crop;    // which part of the picture it shows: left, top, right, bottom, in texture space
    float4 toRed;   // red = dot((luma, blue difference, red difference, 1), toRed)
    float4 toGreen;
    float4 toBlue;
    float4 fill;    // the colour of a quad without a picture
};

struct Corner
{
    float4 position : SV_Position;
    float2 at : TEXCOORD0;
};
