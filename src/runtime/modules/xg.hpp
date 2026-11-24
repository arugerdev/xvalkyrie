#pragma once
#include <cstdint>
#include <string_view>

// xg_x.dll (Xbox One) and xg_xs.dll (Xbox Series X|S): how the console lays a texture out in
// memory. Titles use it to size and fill memory they hand to Direct3D themselves. For Xbox One
// titles both sides of that are ours, so the layout is the plainest one: every mip of every
// array slice in order, rows top to bottom, no tiling. A Series title names the console's
// swizzle mode of a texture, and its data may come laid out that way: it gets the console's
// own layouts (texture_layout.hpp), and the plain one for what those do not describe.
namespace xv::runtime {
class Registry;
namespace modules {
// DXGI_FORMAT -> bytes of one pixel, or of one 4x4 block for the block-compressed formats
struct FormatSize {
    std::uint32_t bytes;
    bool blocks;
};
FormatSize format_size(std::uint32_t format);
// bytes of a whole texture in the plain layout, rounded up to a 4 KiB page
std::uint64_t texture_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                            std::uint32_t array_size, std::uint32_t format);
// xg_xs.dll's texture computer for a Series title's description of a texture (its
// D3D12_RESOURCE_DESC, whose layout names a swizzle mode: 0x100 | mode), with the console's own
// layout of it; null where that layout is not known here. One reference, the caller's.
void *series_texture_computer(const void *description);
// `module` is the name the console gives the DLL
void install_xg(Registry &registry, std::string_view module);
} // namespace modules
} // namespace xv::runtime
