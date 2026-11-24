#include "runtime/modules/xg.hpp"
#include "runtime/common/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

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
                 reinterpret_cast<void *>(&create_texture_computer));
    registry.add(module, "XGComputeTexture2DLayout", reinterpret_cast<void *>(&texture2d_layout));
    registry.add(module, "XGComputeTexture3DLayout", reinterpret_cast<void *>(&texture3d_layout));
    registry.add(module, "XGComputeOptimalTileMode", reinterpret_cast<void *>(&optimal_tile_mode));
    registry.add(module, "XGSuggestSwizzleMode", reinterpret_cast<void *>(&suggest_swizzle_mode));
    registry.add(module, "XGComputeOptimalDepthStencilTileModes",
                 reinterpret_cast<void *>(&optimal_depth_stencil_tile_modes));
}
} // namespace xv::runtime::modules
