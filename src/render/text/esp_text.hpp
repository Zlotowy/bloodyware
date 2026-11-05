#pragma once

#include <imgui.h>

#include <string_view>

struct ID3D11DeviceContext;

// ESP text with a one-pixel outline, drawn through unmodified ImGui.
//
// Outlined ESP text is collected while the features draw and emitted in one block by flush(): a callback switches
// to a pixel shader that rebuilds the outline from the ordinary font atlas (the glyph quads are widened by one
// pixel; the shader takes the strongest coverage of the four neighbouring texels), the text is added with the
// stock ImDrawList::AddText, and ImDrawCallback_ResetRenderState hands the pipeline back to the standard backend.
// One block per frame keeps the number of draw calls independent of the number of texts.
namespace render::esp_text
{
// Called by the render thread before flush(): the context whose device owns the shader.
void bind(ID3D11DeviceContext* context) noexcept;
// Frees the pixel shader (device loss / shutdown). The next flush recreates it.
void release() noexcept;

// Queues a text for the block. `outlined` selects the outline shader; plain texts (shadows, the clipped ammo colour
// mask) are queued too so that the order of everything drawn on ESP fonts is kept. The clip rectangle of `list` at the
// moment of the call travels with the text.
void enqueue(const ImDrawList* list, ImFont* font, float size, ImVec2 position, ImU32 color, std::string_view text,
             bool outlined);
// Emits everything queued so far into `list` and empties the queue.
void flush(ImDrawList* list);
} // namespace render::esp_text
