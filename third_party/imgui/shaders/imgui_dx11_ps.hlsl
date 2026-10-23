// imgui_impl_dx11.cpp 里的像素着色器原文（leprac 预编译用，见 build_shaders.bat）
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

float4 main(PS_INPUT input) : SV_Target
{
    float4 out_col = input.col * texture0.Sample(sampler0, input.uv);
    return out_col;
}
