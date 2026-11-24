#include "runtime/modules/xg.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/texture_layout.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

namespace xv::runtime::modules {
FormatSize format_size(std::uint32_t format) {
    const auto within = [&](std::uint32_t first, std::uint32_t last) {
        return format >= first && format <= last;
    };
    if (within(1, 4)) // R32G32B32A32
        return {16, false};
    if (within(5, 8)) // R32G32B32
        return {12, false};
    if (within(9, 22)) // R16G16B16A16, R32G32, D32 + S8
        return {8, false};
    if (within(23, 47)) // 10:10:10:2, 11:11:10, R8G8B8A8, R16G16, R32, D24 + S8
        return {4, false};
    if (within(48, 59)) // R8G8, R16
        return {2, false};
    if (within(60, 65)) // R8, A8
        return {1, false};
    if (within(70, 72) || within(79, 81)) // BC1, BC4
        return {8, true};
    if (within(73, 78) || within(82, 84) || within(94, 99)) // BC2, BC3, BC5, BC6H, BC7
        return {16, true};
    if (format == 85 || format == 86 || format == 115) // 5:6:5, 5:5:5:1, 4:4:4:4
        return {2, false};
    if (format == 190) // the consoles' own R4G4
        return {1, false};
    return {4, false}; // B8G8R8A8 and friends, and anything unlisted
}

std::uint64_t texture_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                            std::uint32_t array_size, std::uint32_t format) {
    const auto size = format_size(format);
    if (!mips) { // a full chain
        for (auto side = std::max(width, height); side; side >>= 1)
            ++mips;
    }
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint64_t w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
        total += size.blocks ? ((w + 3) / 4) * ((h + 3) / 4) * size.bytes : w * h * size.bytes;
    }
    total *= std::max(array_size, 1u);
    return (total + 4095) & ~std::uint64_t{4095};
}

namespace {
// XG_TEXTURE2D_DESC as titles fill it in: the Direct3D 11.X texture description followed
// by the tile mode and the pitch.
struct TextureDescription {
    std::uint32_t width, height, mips, array_size, format, sample_count, sample_quality, usage,
        bind_flags, cpu_access, misc_flags, esram_offset, esram_usage, tile_mode, pitch;
};
// Titles read the total size from the start of XG_RESOURCE_LAYOUT. The structure goes on
// with per-plane and per-mip tables, 0xd40 bytes in all, which are left zeroed but for the
// row pitch of the first mip in bytes, at 0x5c: a title multiplies it by the height to
// size a copy of the texture.
constexpr std::size_t layout_bytes = 0xd40, first_pitch = 0x5c;
struct Computer {
    void *const *vtable;
    std::atomic<std::uint32_t> references;
    TextureDescription description;
};
std::uint32_t add_ref(Computer *self) { return ++self->references; }
std::uint32_t release(Computer *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
std::int32_t texture2d_layout(const TextureDescription *description, void *layout) {
    if (!description || !layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    std::memset(layout, 0, layout_bytes);
    const auto &d = *description;
    const std::uint64_t header[2] = {
        texture_bytes(d.width, d.height, d.mips, d.array_size, d.format), 4096};
    std::memcpy(layout, header, sizeof header);
    const std::uint32_t counts[2] = {d.mips, 1}; // mip levels, planes
    std::memcpy(static_cast<char *>(layout) + sizeof header, counts, sizeof counts);
    const auto size = format_size(d.format);
    const std::uint32_t pitch = size.blocks ? (std::max(d.width, 1u) + 3) / 4 * size.bytes
                                            : std::max(d.width, 1u) * size.bytes;
    std::memcpy(static_cast<char *>(layout) + first_pitch, &pitch, sizeof pitch);
    return 0;
}
std::int32_t resource_layout(Computer *self, void *layout) {
    return texture2d_layout(&self->description, layout);
}
// XG_TEXTURE3D_DESC, and the part of XG_MIPLEVEL_LAYOUT that says where a mip level is.
struct VolumeDescription {
    std::uint32_t width, height, depth, mips, format, usage, bind_flags, cpu_access, misc_flags,
        esram_offset, esram_usage, tile_mode, pitch;
};
struct MipLayout {
    std::uint64_t size, offset, slice_size;
    std::uint32_t pitch_pixels, pitch_bytes, alignment, padded_width, padded_height, padded_depth,
        width, height, depth, samples;
};
constexpr std::size_t plane_size = 0x20, bytes_per_element = 0x38, first_mip = 0x40,
                      mip_bytes = 0x60;
std::int32_t texture3d_layout(const VolumeDescription *description, void *layout) {
    if (!description || !layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    std::memset(layout, 0, layout_bytes);
    const auto &d = *description;
    const auto size = format_size(d.format);
    auto mips = d.mips;
    if (!mips) {
        for (auto side = std::max({d.width, d.height, d.depth}); side; side >>= 1)
            ++mips;
    }
    mips = std::min(mips, 15u);
    const auto base = static_cast<char *>(layout);
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint32_t w = std::max(d.width >> mip, 1u), h = std::max(d.height >> mip, 1u),
                            depth = std::max(d.depth >> mip, 1u);
        const std::uint32_t across = size.blocks ? (w + 3) / 4 : w,
                            down = size.blocks ? (h + 3) / 4 : h;
        MipLayout level{};
        level.slice_size = std::uint64_t{across} * down * size.bytes;
        level.size = level.slice_size * depth;
        level.offset = total;
        level.pitch_pixels = size.blocks ? across * 4 : across;
        level.pitch_bytes = across * size.bytes;
        level.alignment = 1;
        level.padded_width = level.width = across;
        level.padded_height = level.height = down;
        level.padded_depth = level.depth = depth;
        level.samples = 1;
        std::memcpy(base + first_mip + mip * mip_bytes, &level, sizeof level);
        total += level.size;
    }
    total = (total + 4095) & ~std::uint64_t{4095};
    const std::uint64_t header[2] = {total, 4096};
    std::memcpy(base, header, sizeof header);
    const std::uint32_t counts[2] = {mips, 1}; // mip levels, planes
    std::memcpy(base + sizeof header, counts, sizeof counts);
    const std::uint64_t plane[3] = {total, 0, 4096}; // size, offset, alignment
    std::memcpy(base + plane_size, plane, sizeof plane);
    std::memcpy(base + bytes_per_element, &size.bytes, sizeof(std::uint32_t));
    return 0;
}
template <int Slot> std::int32_t unknown(Computer *) {
    std::fprintf(stderr, "[Runtime] unimplemented xg_x.dll texture computer method %d\n", Slot);
    std::fflush(stderr);
    return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
}
void *const computer_vtable[] = {
    reinterpret_cast<void *>(&add_ref),         reinterpret_cast<void *>(&release),
    reinterpret_cast<void *>(&resource_layout), reinterpret_cast<void *>(&unknown<3>),
    reinterpret_cast<void *>(&unknown<4>),      reinterpret_cast<void *>(&unknown<5>),
    reinterpret_cast<void *>(&unknown<6>),      reinterpret_cast<void *>(&unknown<7>),
    reinterpret_cast<void *>(&unknown<8>),      reinterpret_cast<void *>(&unknown<9>),
    reinterpret_cast<void *>(&unknown<10>),     reinterpret_cast<void *>(&unknown<11>),
};

std::int32_t create_texture2d_computer(const TextureDescription *description, Computer **computer) {
    if (!description || !computer)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    *computer = new Computer{computer_vtable, 1, *description};
    return 0;
}
// XGCreateTextureComputer of the Series console's xg_xs.dll: its XG_RESOURCE_DESC begins as
// Direct3D 12's resource description does. The computer's methods are those of the later
// xg_x.dll, in that order: after GetResourceLayout the size in bytes and in pages, the
// alignment, where a mip level and a texel are, and the copies into and out of a
// subresource. The one layout here is the linear one that a placed texture is read with:
// array slice after array slice, each with its mip levels in turn, rows packed.
struct ResourceDescription {
    std::uint32_t dimension, pad;
    std::uint64_t alignment, width;
    std::uint32_t height;
    std::uint16_t depth_or_array_size, mips;
    std::uint32_t format, sample_count, sample_quality, layout, flags;
};
struct Level {
    std::uint64_t offset;
    std::uint32_t pitch, rows, slices;
};
std::uint32_t levels_of(const TextureDescription &d) {
    if (d.mips)
        return d.mips;
    std::uint32_t mips = 0;
    for (auto side = std::max(d.width, d.height); side; side >>= 1)
        ++mips;
    return std::max(mips, 1u);
}
// a volume (misc_flags says so here) has its depth in array_size
Level level_of(const TextureDescription &d, std::uint32_t mip, std::uint32_t slice) {
    const auto size = format_size(d.format);
    const bool volume = d.misc_flags != 0;
    const auto mips = levels_of(d);
    const auto one = [&](std::uint32_t m) {
        const std::uint32_t w = std::max(d.width >> m, 1u), h = std::max(d.height >> m, 1u);
        Level level{};
        level.pitch = size.blocks ? (w + 3) / 4 * size.bytes : w * size.bytes;
        level.rows = size.blocks ? (h + 3) / 4 : h;
        level.slices = volume ? std::max(d.array_size >> m, 1u) : 1;
        return level;
    };
    std::uint64_t chain = 0, before = 0;
    for (std::uint32_t m = 0; m < mips; ++m) {
        const auto level = one(m);
        const auto bytes = std::uint64_t{level.pitch} * level.rows * level.slices;
        if (m < mip)
            before += bytes;
        chain += bytes;
    }
    auto level = one(std::min(mip, mips - 1));
    level.offset = (volume ? 0 : chain * slice) + before;
    return level;
}
std::uint64_t resource_bytes(Computer *self) {
    const auto &d = self->description;
    const auto all = level_of(d, levels_of(d), 0).offset; // one chain
    const auto total = all * (d.misc_flags ? 1 : std::max(d.array_size, 1u));
    return (total + 4095) & ~std::uint64_t{4095};
}
std::uint64_t resource_pages(Computer *self) { return resource_bytes(self) >> 12; }
std::uint64_t resource_alignment(Computer *) { return 0x10000; }
std::uint64_t mip_offset(Computer *self, std::uint32_t, std::uint32_t mip) {
    return level_of(self->description, mip, 0).offset;
}
std::uint64_t texel_offset(Computer *self, std::uint32_t, std::uint32_t mip, std::uint64_t x,
                           std::uint32_t y, std::uint32_t slice, std::uint32_t) {
    const auto &d = self->description;
    const auto size = format_size(d.format);
    const bool volume = d.misc_flags != 0;
    const auto level = level_of(d, mip, volume ? 0 : slice);
    const std::uint64_t across = size.blocks ? x / 4 : x, down = size.blocks ? y / 4 : y;
    return level.offset + (volume ? std::uint64_t{level.pitch} * level.rows * slice : 0) +
           down * level.pitch + across * size.bytes;
}
// rows of `pitch` bytes each on the title's side, packed on the resource's
std::int32_t copy_subresource(Computer *self, std::uint8_t *resource, std::uint32_t subresource,
                              std::uint8_t *data, std::uint32_t pitch, std::uint32_t slice_pitch,
                              bool into) {
    if (!resource || !data)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    const auto &d = self->description;
    const auto mips = levels_of(d);
    const auto level = level_of(d, subresource % mips, subresource / mips);
    const auto bytes = std::min(pitch ? pitch : level.pitch, level.pitch);
    for (std::uint32_t slice = 0; slice < level.slices; ++slice)
        for (std::uint32_t row = 0; row < level.rows; ++row) {
            auto *const packed =
                resource + level.offset + (std::uint64_t{slice} * level.rows + row) * level.pitch;
            auto *const titles = data + std::uint64_t{slice} * slice_pitch +
                                 std::uint64_t{row} * (pitch ? pitch : level.pitch);
            std::memcpy(into ? packed : titles, into ? titles : packed, bytes);
        }
    return 0;
}
std::int32_t copy_into_subresource(Computer *self, void *resource, std::uint32_t,
                                   std::uint32_t subresource, const void *data, std::uint32_t pitch,
                                   std::uint32_t slice_pitch) {
    return copy_subresource(self, static_cast<std::uint8_t *>(resource), subresource,
                            const_cast<std::uint8_t *>(static_cast<const std::uint8_t *>(data)),
                            pitch, slice_pitch, true);
}
std::int32_t copy_from_subresource(Computer *self, void *data, std::uint32_t,
                                   std::uint32_t subresource, const void *resource,
                                   std::uint32_t pitch, std::uint32_t slice_pitch) {
    return copy_subresource(
        self, const_cast<std::uint8_t *>(static_cast<const std::uint8_t *>(resource)), subresource,
        static_cast<std::uint8_t *>(data), pitch, slice_pitch, false);
}
void *const resource_computer_vtable[] = {
    reinterpret_cast<void *>(&add_ref),
    reinterpret_cast<void *>(&release),
    reinterpret_cast<void *>(&resource_layout),
    reinterpret_cast<void *>(&resource_bytes),
    reinterpret_cast<void *>(&resource_pages),
    reinterpret_cast<void *>(&resource_alignment),
    reinterpret_cast<void *>(&mip_offset),
    reinterpret_cast<void *>(&texel_offset),
    reinterpret_cast<void *>(&copy_into_subresource),
    reinterpret_cast<void *>(&copy_from_subresource),
    reinterpret_cast<void *>(&unknown<10>),
    reinterpret_cast<void *>(&unknown<11>),
    reinterpret_cast<void *>(&unknown<12>),
};
std::int32_t create_texture_computer(const ResourceDescription *description, Computer **computer) {
    if (!description || !computer)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    const auto &d = *description;
    TextureDescription texture{};
    texture.width = static_cast<std::uint32_t>(std::max<std::uint64_t>(d.width, 1));
    texture.height = d.dimension >= 3 ? std::max(d.height, 1u) : 1;
    texture.mips = d.mips;
    texture.array_size = std::max<std::uint32_t>(d.depth_or_array_size, 1);
    texture.format = d.format;
    texture.sample_count = 1;
    texture.misc_flags = d.dimension == 4; // a volume
    *computer = new Computer{resource_computer_vtable, 1, texture};
    return 0;
}
// The Series console's own layouts (texture_layout.hpp), for a description whose layout names a
// swizzle mode (0x100 | mode). The methods are those of the Xbox One computer in the same
// order: after GetResourceLayout the size, the alignment, where a mip level starts, where an
// element is and the element at an offset, the copies into and out of a subresource, then the
// tiling in 64 KiB tiles. Arguments and the XG_RESOURCE_LAYOUT shape are those a title built
// with a later kit calls them with; the fields of the layout no title is known to read follow
// the Xbox One structure of the same name.
struct SeriesComputer {
    void *const *vtable;
    std::atomic<std::uint32_t> references;
    ResourceDescription desc;
    gfx10::Texture texture;
};
constexpr std::uint32_t swizzled = 0x100, max_layout_mips = 15, allow_depth_stencil = 2;
// the console's texture for a description that names a swizzle mode, if it has a layout here
gfx10::Texture series_texture(const ResourceDescription &d) {
    if ((d.layout & ~0xffu) != swizzled || d.width > 0xffffffffu)
        return {};
    gfx10::Dimension dimension;
    switch (d.dimension) {
    case 2:
        dimension = gfx10::Dimension::texture1d;
        break;
    case 3:
        dimension = gfx10::Dimension::texture2d;
        break;
    case 4:
        dimension = gfx10::Dimension::texture3d;
        break;
    default:
        return {};
    }
    auto texture =
        gfx10::texture(dimension, static_cast<std::uint32_t>(d.width), d.height,
                       d.depth_or_array_size, d.mips, d.format, d.sample_count, d.layout & 0xff);
    // the layout structure has room for 15 mips
    return texture.valid && texture.mips <= max_layout_mips ? texture : gfx10::Texture{};
}
std::uint32_t series_add_ref(SeriesComputer *self) { return ++self->references; }
std::uint32_t series_release(SeriesComputer *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
// XG_RESOURCE_LAYOUT of xg_xs.dll: an XG_PLANE_LAYOUT for each of up to four planes, each with
// an XG_MIPLEVEL_LAYOUT for each of up to 15 mips
struct SeriesMip {
    std::uint64_t size, offset, slice_size;
    std::uint32_t pitch_texels, pitch_bytes, alignment;
    std::uint32_t padded[3]; // width and height in elements, depth or array size
    std::uint32_t elements[2], texels[2];
    std::uint32_t depth_or_array, samples, mode;
    std::uint64_t bank_rotation[2];
    std::uint32_t slice_depth, in_tail;
    std::uint32_t tail_position; // where a tail mip starts in elements: x bits 0-10, y 11-21
};
struct SeriesPlane {
    std::uint32_t usage; // 1 data, 7 depth, 8 stencil
    std::uint64_t size, slice_size, offset, alignment;
    std::uint32_t element_bytes, standard_mips, tail_mips;
    SeriesMip mips[max_layout_mips];
};
struct SeriesLayout {
    std::uint64_t size, alignment;
    std::uint32_t mips, planes;
    SeriesPlane plane[4];
    std::uint32_t dimension;
};
static_assert(sizeof(SeriesMip) == 0x70 && sizeof(SeriesPlane) == 0x6c8 &&
              sizeof(SeriesLayout) == 0x1b40);
// the mode a tail mip is given in: the 256-byte one of its kind (3, render, for the render
// modes, which GFX10 has no 256-byte form of), a depth mode itself
std::uint32_t tail_mode(std::uint32_t mode) {
    switch (mode) {
    case 2:
    case 6:
    case 10:
    case 18:
    case 22:
    case 26:
        return gfx10::sw_256b_d;
    case 23:
    case 27:
        return 3;
    case 24:
        return mode;
    default:
        return gfx10::sw_256b_s;
    }
}
SeriesMip series_mip(const ResourceDescription &d, const gfx10::Layout &l, std::uint32_t i) {
    const auto &m = l.mips[i];
    const bool three_d = d.dimension == 4;
    const std::uint32_t array = std::max<std::uint32_t>(d.depth_or_array_size, 1);
    SeriesMip out{};
    out.pitch_texels = m.pitch * l.desc.block;
    out.pitch_bytes = m.pitch * l.desc.element_bytes;
    out.padded[0] = m.pitch;
    out.padded[1] = m.rows;
    out.padded[2] = three_d ? m.depth_pitch : array;
    out.elements[0] = m.width;
    out.elements[1] = m.height;
    out.texels[0] = std::max(static_cast<std::uint32_t>(d.width >> i), 1u);
    out.texels[1] = std::max(d.height >> i, 1u);
    out.depth_or_array = three_d ? m.depth : array;
    out.samples = l.desc.samples;
    out.slice_depth = l.block[2];
    const auto slice = std::uint64_t{m.pitch} * m.rows * l.desc.element_bytes;
    const std::uint64_t depth = three_d ? m.depth_pitch : 1;
    if (!m.in_tail) {
        out.offset = m.offset;
        out.slice_size = slice * l.desc.samples;
        out.alignment = static_cast<std::uint32_t>(l.alignment);
        out.mode = l.desc.mode;
    } else {
        out.in_tail = 1;
        out.alignment = 256;
        out.mode = tail_mode(l.desc.mode);
        if (l.thick) { // a 3D tail mip is placed by its position, not by an offset
            out.slice_size = l.alignment / l.block[2];
            out.tail_position = m.tail.x | m.tail.y << 11;
        } else {
            out.offset = m.tail.offset;
            out.slice_size = slice;
        }
    }
    out.size = out.slice_size * depth;
    return out;
}
std::int32_t series_layout(SeriesComputer *self, SeriesLayout *out) {
    if (!out)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    const auto &t = self->texture;
    std::memset(out, 0, sizeof *out);
    out->size = t.size;
    out->alignment = t.alignment;
    out->mips = t.mips;
    out->planes = static_cast<std::uint32_t>(t.planes.size());
    out->dimension = self->desc.dimension;
    // a depth plane is called one beside stencil or without mips
    const bool depth =
        (self->desc.flags & allow_depth_stencil) && (t.planes.size() > 1 || t.mips == 1);
    for (std::size_t p = 0; p < t.planes.size() && p < 4; ++p) {
        const auto &l = t.planes[p].layout;
        auto &plane = out->plane[p];
        plane.usage = t.planes[p].stencil ? 8 : depth ? 7 : 1;
        plane.size = l.size;
        plane.slice_size = l.slice_size;
        plane.offset = t.planes[p].offset;
        plane.alignment = l.alignment;
        plane.element_bytes = l.desc.element_bytes;
        plane.standard_mips = l.first_tail_mip;
        plane.tail_mips = t.mips - l.first_tail_mip;
        for (std::uint32_t i = 0; i < l.mips.size(); ++i)
            plane.mips[i] = series_mip(self->desc, l, i);
    }
    return 0;
}
std::uint64_t series_bytes(SeriesComputer *self) { return self->texture.size; }
std::uint64_t series_alignment(SeriesComputer *self) { return self->texture.alignment; }
// (plane, mip): where it starts, from the start of the texture; 0 for one there is not
std::uint64_t series_mip_offset(SeriesComputer *self, std::uint32_t plane, std::uint32_t mip) {
    const auto &planes = self->texture.planes;
    if (plane >= planes.size() || mip >= planes[plane].layout.mips.size())
        return 0;
    const auto &m = planes[plane].layout.mips[mip];
    return planes[plane].offset + (m.in_tail ? m.tail.offset : m.offset);
}
// (plane, mip, x, y, z or array slice, sample, bit position): an element's offset from the
// start of the texture, 0 outside it. Elements are whole bytes here, so the bit is 0.
std::uint64_t series_element_offset(SeriesComputer *self, std::uint32_t plane, std::uint32_t mip,
                                    std::uint64_t x, std::uint32_t y, std::uint32_t z,
                                    std::uint32_t, std::uint32_t *bit) {
    if (bit)
        *bit = 0;
    const auto &planes = self->texture.planes;
    std::uint64_t at = 0;
    if (plane >= planes.size() || x > 0xffffffffu ||
        !planes[plane].layout.offset(mip, static_cast<std::uint32_t>(x), y, z, at))
        return 0;
    return planes[plane].offset + at;
}
std::int32_t series_copy(SeriesComputer *self, std::uint8_t *tiled, std::uint32_t plane,
                         std::uint32_t subresource, std::uint8_t *linear, std::uint32_t row_pitch,
                         std::uint32_t depth_pitch, bool into) {
    const auto &t = self->texture;
    if (!tiled || !linear || plane >= t.planes.size() || subresource / t.mips >= 0x10000)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    const auto &p = t.planes[plane];
    const auto mip = subresource % t.mips;
    if (!p.layout.addressable())                      // a pipe or bank XOR mode, or several samples
        return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
    // the rows' extent: the title's memory holds them, as it holds the whole texture
    const auto &m = p.layout.mips[mip];
    const auto bytes = std::size_t{m.depth - 1} * depth_pitch +
                       std::size_t{m.height - 1} * row_pitch +
                       std::size_t{m.width} * p.layout.desc.element_bytes;
    return p.layout.copy(tiled + p.offset, static_cast<std::size_t>(p.layout.size), mip,
                         subresource / t.mips, linear, bytes, row_pitch, depth_pitch, into)
               ? 0
               : static_cast<std::int32_t>(0x80070057);
}
// (texture, plane, subresource, rows, row pitch, depth pitch)
std::int32_t series_copy_into(SeriesComputer *self, void *resource, std::uint32_t plane,
                              std::uint32_t subresource, const void *data, std::uint32_t pitch,
                              std::uint32_t depth_pitch) {
    return series_copy(self, static_cast<std::uint8_t *>(resource), plane, subresource,
                       const_cast<std::uint8_t *>(static_cast<const std::uint8_t *>(data)), pitch,
                       depth_pitch, true);
}
// (rows, plane, subresource, sample, texture, row pitch, depth pitch)
std::int32_t series_copy_from(SeriesComputer *self, void *data, std::uint32_t plane,
                              std::uint32_t subresource, std::uint32_t, const void *resource,
                              std::uint32_t pitch, std::uint32_t depth_pitch) {
    return series_copy(
        self, const_cast<std::uint8_t *>(static_cast<const std::uint8_t *>(resource)), plane,
        subresource, static_cast<std::uint8_t *>(data), pitch, depth_pitch, false);
}
// GetResourceTiling as Direct3D 12 has it (D3D12_PACKED_MIP_INFO, D3D12_TILE_SHAPE,
// D3D12_SUBRESOURCE_TILING) in 64 KiB tiles of plane 0, for the 64 KiB 2D modes; other layouts
// have no such tiles and fail. The tail is the packed part, the first tile of each slice. With
// a tail the console lists two slices at most and zeroes all but the first.
struct PackedMips {
    std::uint8_t standard, packed;
    std::uint32_t tiles, start;
};
struct SubresourceTiling {
    std::uint32_t width;
    std::uint16_t height, depth;
    std::uint32_t start;
};
static_assert(sizeof(PackedMips) == 12 && sizeof(SubresourceTiling) == 12);
std::int32_t series_tiling(SeriesComputer *self, std::uint32_t *tile_count, PackedMips *packed,
                           std::uint32_t *shape, std::uint32_t *tiling_count, std::uint32_t first,
                           SubresourceTiling *tilings) {
    constexpr std::uint64_t tile = 0x10000;
    const auto &t = self->texture;
    const auto &l = t.planes[0].layout;
    if (l.alignment != tile)
        return static_cast<std::int32_t>(0x80004005); // E_FAIL
    if (l.thick)
        return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
    const auto standard = l.first_tail_mip;
    const bool tail = standard < t.mips;
    const std::uint64_t tail_tiles = standard == 0       ? l.slice_size / tile
                                     : standard < t.mips ? l.mips[standard - 1].offset / tile
                                                         : 0;
    if (tile_count)
        *tile_count = static_cast<std::uint32_t>(l.size / tile);
    if (packed)
        *packed = {static_cast<std::uint8_t>(standard),
                   static_cast<std::uint8_t>(t.mips - standard),
                   static_cast<std::uint32_t>(tail_tiles), 0};
    if (shape) {
        shape[0] = l.block[0] * l.desc.block;
        shape[1] = l.block[1] * l.desc.block;
        shape[2] = l.block[2];
    }
    if (tiling_count) {
        const auto slices = tail ? std::min(l.slices, 2u) : l.slices;
        const auto all = standard * slices;
        const auto wanted = std::min(all > first ? all - first : 0, *tiling_count);
        for (std::uint32_t i = 0; tilings && i < wanted; ++i) {
            const auto slice = (first + i) / standard, mip = (first + i) % standard;
            const auto &m = l.mips[mip];
            if (tail && slice > 0)
                tilings[i] = {0, 0, 0, 0};
            else
                tilings[i] = {m.pitch / l.block[0], static_cast<std::uint16_t>(m.rows / l.block[1]),
                              1,
                              static_cast<std::uint32_t>(
                                  (std::uint64_t{slice} * l.slice_size + m.offset) / tile)};
        }
        *tiling_count = wanted;
    }
    return 0;
}
template <int Slot> std::int32_t series_unknown(SeriesComputer *) {
    std::fprintf(stderr, "[Runtime] unimplemented xg_xs.dll texture computer method %d\n", Slot);
    std::fflush(stderr);
    return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
}
void *const series_computer_vtable[] = {
    reinterpret_cast<void *>(&series_add_ref),
    reinterpret_cast<void *>(&series_release),
    reinterpret_cast<void *>(&series_layout),
    reinterpret_cast<void *>(&series_bytes),
    reinterpret_cast<void *>(&series_alignment),
    reinterpret_cast<void *>(&series_mip_offset),
    reinterpret_cast<void *>(&series_element_offset),
    // the element at an offset: its arguments are not known here
    reinterpret_cast<void *>(&series_unknown<7>),
    reinterpret_cast<void *>(&series_copy_into),
    reinterpret_cast<void *>(&series_copy_from),
    reinterpret_cast<void *>(&series_tiling),
    reinterpret_cast<void *>(&series_unknown<11>),
    reinterpret_cast<void *>(&series_unknown<12>),
};
// XGCreateTextureComputer of xg_xs.dll: the console's layout where the description names a
// swizzle mode GFX10 has and its format has a layout here; the plain one otherwise, as before
std::int32_t create_series_texture_computer(const ResourceDescription *description,
                                            void **computer) {
    if (!description || !computer)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    if (auto texture = series_texture(*description); texture.valid) {
        *computer = new SeriesComputer{series_computer_vtable, 1, *description, std::move(texture)};
        return 0;
    }
    return create_texture_computer(description, reinterpret_cast<Computer **>(computer));
}
} // namespace
void *series_texture_computer(const void *description) {
    if (!description)
        return nullptr;
    const auto &d = *static_cast<const ResourceDescription *>(description);
    auto texture = series_texture(d);
    return texture.valid ? new SeriesComputer{series_computer_vtable, 1, d, std::move(texture)}
                         : nullptr;
}
namespace {
// There is one layout, so one tile mode: 0.
std::uint32_t optimal_tile_mode(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                std::uint32_t, std::uint32_t, std::uint32_t) {
    return 0;
}
// XGSuggestSwizzleMode of later kits' xg_xs.dll, whose arguments are in no header here: a
// title takes an XG_SWIZZLE_MODE from it, and the one layout there is is the linear one
std::uint32_t suggest_swizzle_mode() { return 0; }
// format, width, height, depth or layers, samples, then whether it is compressed, a tiled
// resource, and read as a texture
void optimal_depth_stencil_tile_modes(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                      std::uint32_t, std::int32_t, std::int32_t, std::int32_t,
                                      std::uint32_t *depth, std::uint32_t *stencil) {
    if (depth)
        *depth = 0;
    if (stencil)
        *stencil = 0;
}
} // namespace

void install_xg(Registry &registry, std::string_view module) {
    registry.add(module, "XGCreateTexture2DComputer",
                 reinterpret_cast<void *>(&create_texture2d_computer));
    registry.add(module, "XGCreateTextureComputer",
                 module == "xg_xs.dll" ? reinterpret_cast<void *>(&create_series_texture_computer)
                                       : reinterpret_cast<void *>(&create_texture_computer));
    registry.add(module, "XGComputeTexture2DLayout", reinterpret_cast<void *>(&texture2d_layout));
    registry.add(module, "XGComputeTexture3DLayout", reinterpret_cast<void *>(&texture3d_layout));
    registry.add(module, "XGComputeOptimalTileMode", reinterpret_cast<void *>(&optimal_tile_mode));
    registry.add(module, "XGSuggestSwizzleMode", reinterpret_cast<void *>(&suggest_swizzle_mode));
    registry.add(module, "XGComputeOptimalDepthStencilTileModes",
                 reinterpret_cast<void *>(&optimal_depth_stencil_tile_modes));
}
} // namespace xv::runtime::modules
