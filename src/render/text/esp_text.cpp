#include <render/text/esp_text.hpp>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

namespace render::esp_text
{
namespace
{
struct item
{
    ImFont* font{};
    float size{};
    ImVec2 position{};
    ImU32 color{};
    std::size_t text_offset{};
    std::size_t text_size{};
    ImVec4 clip{};
    bool outlined{};
};

// The queue is only touched by the render thread; storage is reused from frame to frame.
std::vector<item> g_items;
std::string g_text;

ID3D11DeviceContext* g_context{};
Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11PixelShader> g_shader;
bool g_shader_failed{};

constexpr const char* k_pixel_shader = R"(
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
sampler sampler0;
Texture2D texture0;
float4 main(PS_INPUT input) : SV_Target
{
    uint width, height;
    texture0.GetDimensions(width, height);
    int2 p = int2(floor(input.uv * float2(width, height)));
    float4 c = texture0.Load(int3(p, 0));
    // Colour glyphs (emoji) are not tinted white: draw them as the stock shader would.
    if (c.a > 0.0 && min(c.r, min(c.g, c.b)) < 0.97)
        return input.col * c;
    float fill = c.a;
    float l = texture0.Load(int3(p + int2(-1, 0), 0)).a;
    float r = texture0.Load(int3(p + int2( 1, 0), 0)).a;
    float u = texture0.Load(int3(p + int2( 0,-1), 0)).a;
    float d = texture0.Load(int3(p + int2( 0, 1), 0)).a;
    float outline = max(fill, max(max(l, r), max(u, d)));
    float ratio = outline > 0.0001 ? fill / outline : 0.0;
    return float4(input.col.rgb * ratio, input.col.a * outline);
}
)";

bool ensure_shader() noexcept
{
    if (!g_context)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    g_context->GetDevice(&device);
    if (!device)
        return false;
    if (device.Get() != g_device.Get())
    {
        g_shader.Reset();
        g_device = device;
        g_shader_failed = false;
    }
    if (g_shader)
        return true;
    if (g_shader_failed)
        return false;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const HRESULT compiled = ::D3DCompile(k_pixel_shader, std::char_traits<char>::length(k_pixel_shader), nullptr,
                                          nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &errors);
    if (FAILED(compiled) && errors)
    {
        ::OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
    }
    if (FAILED(compiled) ||
        FAILED(g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_shader)))
    {
        g_shader_failed = true; // the text then falls back to plain, unoutlined glyphs
        return false;
    }
    return true;
}

void set_shader(const ImDrawList*, const ImDrawCmd*)
{
    if (g_context && g_shader)
        g_context->PSSetShader(g_shader.Get(), nullptr, 0);
}

// Widens every glyph quad written since `first_vertex` by one pixel (and its texture rectangle by one texel) so the
// shader has room to draw the outline around the glyph. The atlas keeps one texel of padding around glyphs.
void widen(ImDrawList* list, int first_vertex, ImFont* font)
{
    const ImTextureData* atlas = font && font->ContainerAtlas ? font->ContainerAtlas->TexData : nullptr;
    if (!atlas || atlas->Width <= 0 || atlas->Height <= 0)
        return;
    const float du = 1.0f / static_cast<float>(atlas->Width);
    const float dv = 1.0f / static_cast<float>(atlas->Height);
    ImDrawVert* vertices = list->VtxBuffer.Data;
    for (int i = first_vertex; i + 3 < list->VtxBuffer.Size; i += 4)
    {
        // Order written by ImFont::RenderText: top-left, top-right, bottom-right, bottom-left.
        vertices[i + 0].pos.x -= 1.0f;
        vertices[i + 0].pos.y -= 1.0f;
        vertices[i + 0].uv.x -= du;
        vertices[i + 0].uv.y -= dv;
        vertices[i + 1].pos.x += 1.0f;
        vertices[i + 1].pos.y -= 1.0f;
        vertices[i + 1].uv.x += du;
        vertices[i + 1].uv.y -= dv;
        vertices[i + 2].pos.x += 1.0f;
        vertices[i + 2].pos.y += 1.0f;
        vertices[i + 2].uv.x += du;
        vertices[i + 2].uv.y += dv;
        vertices[i + 3].pos.x -= 1.0f;
        vertices[i + 3].pos.y += 1.0f;
        vertices[i + 3].uv.x -= du;
        vertices[i + 3].uv.y += dv;
    }
}
} // namespace

void bind(ID3D11DeviceContext* context) noexcept
{
    g_context = context;
}

void release() noexcept
{
    g_shader.Reset();
    g_device.Reset();
    g_shader_failed = false;
}

void enqueue(const ImDrawList* list, ImFont* font, const float size, const ImVec2 position, const ImU32 color,
             const std::string_view text, const bool outlined)
{
    // A caller that never flushes (no overlay frame running) must not make the queue grow without bound.
    if (!list || !font || text.empty() || g_items.size() >= 16384)
        return;
    const ImVec4 clip(list->GetClipRectMin().x, list->GetClipRectMin().y, list->GetClipRectMax().x,
                      list->GetClipRectMax().y);
    g_items.push_back({font, size, position, color, g_text.size(), text.size(), clip, outlined});
    g_text.append(text);
}

void flush(ImDrawList* list)
{
    if (g_items.empty())
        return;
    const bool shader_ready = list && ensure_shader();
    if (list)
    {
        bool shader_active = false;
        for (const auto& entry : g_items)
        {
            const bool outline = entry.outlined && shader_ready;
            if (outline != shader_active)
            {
                if (outline)
                    list->AddCallback(set_shader, nullptr);
                else
                    list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
                shader_active = outline;
            }
            list->PushClipRect(ImVec2(entry.clip.x, entry.clip.y), ImVec2(entry.clip.z, entry.clip.w), false);
            const int first_vertex = list->VtxBuffer.Size;
            const char* begin = g_text.data() + entry.text_offset;
            list->AddText(entry.font, entry.size, entry.position, entry.color, begin, begin + entry.text_size);
            if (outline)
                widen(list, first_vertex, entry.font);
            list->PopClipRect();
        }
        if (shader_active)
            list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
    g_items.clear();
    g_text.clear();
}
} // namespace render::esp_text
