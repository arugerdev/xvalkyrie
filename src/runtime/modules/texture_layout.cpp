#include "runtime/modules/texture_layout.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

// Ported code after addrlib; see THIRD-PARTY.md and texture_layout.hpp for the credits.
namespace xv::runtime::modules::gfx10 {
namespace {
// log2 of the block in bytes (0 for linear), or false for a mode GFX10 does not have
bool block_log2_of(std::uint32_t mode, std::uint32_t &log2) {
    switch (mode) {
    case sw_linear:
        log2 = 0;
        return true;
    case sw_256b_s:
    case sw_256b_d:
        log2 = 8;
        return true;
    case sw_4kb_s:
    case sw_4kb_d:
    case sw_4kb_s_x:
    case sw_4kb_d_x:
    case sw_4kb_r_x:
        log2 = 12;
        return true;
    case sw_64kb_s:
    case sw_64kb_d:
    case sw_64kb_s_t:
    case sw_64kb_d_t:
    case sw_64kb_z_x:
    case sw_64kb_s_x:
    case sw_64kb_d_x:
    case sw_64kb_r_x:
        log2 = 16;
        return true;
    default:
        return false;
    }
}
bool standard(std::uint32_t mode) {
    return mode == 1 || mode == 5 || mode == 9 || mode == 17 || mode == 21 || mode == 25;
}
bool display(std::uint32_t mode) {
    return mode == 2 || mode == 6 || mode == 10 || mode == 18 || mode == 22 || mode == 26;
}
bool xor_mode(std::uint32_t mode) { return mode >= 16; }
bool power_of_two(std::uint32_t value) { return value && !(value & (value - 1)); }
std::uint32_t log2_of(std::uint32_t value) {
    std::uint32_t log2 = 0;
    while (value > 1) {
        value >>= 1;
        ++log2;
    }
    return log2;
}
std::uint32_t div_up(std::uint32_t value, std::uint32_t by) { return (value + by - 1) / by; }
std::uint32_t align_up(std::uint32_t value, std::uint32_t to) { return div_up(value, to) * to; }
// elements along one side of mip `mip` as data and copies see it: texels halve rounding down
// (to at least 1), then round up to whole elements
std::uint32_t dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) {
    return div_up(std::max(texels >> mip, 1u), block);
}
// the same for placement (padding, tail, offsets): addrlib halves mip 0's elements rounding
// up, which can be one more than dim() for odd sizes
std::uint32_t padded_dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) {
    return std::max(div_up(div_up(std::max(texels, 1u), block), 1u << mip), 1u);
}

// for each address bit of the 256-byte micro block, the element coordinate bit it takes ("--":
// a bit inside the element), by log2 of the bytes per element: thin S, thin D, and thick S
// (also the D 3D modes), from addrlib's GFX10_SW_256_S_RBPLUS, GFX10_SW_256_D_RBPLUS and
// GFX10_SW_4K_S3_RBPLUS patterns. The larger blocks add bits Y, X (thin) or Y, X, Z (thick)
// in turn.
constexpr const char *micro[3][5] = {
    {"x0x1x2x3y0y1y2y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1y2x2", "------x0y0y1x1x2",
     "--------y0y1x0x1"},
    {"x0x1x2y1y0y2x3y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1x2y2", "------x0y0x1x2y1",
     "--------x0y0x1y1"},
    {"x0x1z0y0z1y1x2z2", "--x0z0y0z1y1x1z2", "----x0y0z0y1x1z1", "------x0z0y0x1z1",
     "--------z0y0x0z1"},
};
// per axis, the address bit each coordinate bit lands on
std::array<std::vector<std::uint32_t>, 3>
pattern_bits(std::uint32_t kind, std::uint32_t element_log2, std::uint32_t block_log2) {
    std::array<std::vector<std::uint32_t>, 3> axes;
    const char *const bits = micro[kind][element_log2];
    for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const char axis = bits[2 * bit];
        if (axis == '-')
            continue;
        auto &to = axes[axis == 'x' ? 0 : axis == 'y' ? 1 : 2];
        const auto index = static_cast<std::uint32_t>(bits[2 * bit + 1] - '0');
        if (to.size() <= index)
            to.resize(index + 1, 0);
        to[index] = bit;
    }
    const std::uint32_t thin[] = {1, 0}, thick[] = {1, 0, 2};
    const auto *const order = kind == 2 ? thick : thin;
    const std::uint32_t count = kind == 2 ? 3 : 2;
    for (std::uint32_t bit = 8, i = 0; bit < block_log2; ++bit, ++i)
        axes[order[i % count]].push_back(bit);
    return axes;
}
// offsets in a block for coordinates 0 .. 2^bits of one axis
std::vector<std::uint32_t> spread(const std::vector<std::uint32_t> &bits) {
    std::vector<std::uint32_t> offsets(std::size_t{1} << bits.size());
    for (std::uint32_t c = 0; c < offsets.size(); ++c)
        for (std::uint32_t i = 0; i < bits.size(); ++i)
            offsets[c] |= ((c >> i) & 1) << bits[i];
    return offsets;
}

constexpr std::uint32_t max_mips = 16;
// GFX10's image descriptors hold at most 16384 texels a side, so no texture the GPU can use is
// refused, and every size below stays in range for values a title passes unchecked
constexpr std::uint32_t max_extent = 1u << 14;
// 256-byte micro block dimensions by log2 of the element size, 2D and 3D, and the 1 KiB 3D one
constexpr std::uint32_t block_256_2d[5][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
constexpr std::uint32_t block_256_3d[5][3] = {
    {8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}};
constexpr std::uint32_t block_1k_3d[5][3] = {
    {16, 8, 8}, {8, 8, 8}, {8, 8, 4}, {8, 4, 4}, {4, 4, 4}};

// HwlComputeSurfaceInfoLinear: rows aligned to 256 bytes, mips smallest first, each depth slice
// of a 3D texture with its own chain
Layout linear_layout(const Description &d) {
    Layout l;
    l.desc = d;
    const std::uint64_t bpe = d.element_bytes;
    const auto align = 256 / d.element_bytes;
    const bool three_d = d.dimension == Dimension::texture3d;
    l.mips.resize(d.mips);
    std::uint64_t offset = 0;
    for (auto i = static_cast<std::int32_t>(d.mips) - 1; i >= 0; --i) {
        const auto mip = static_cast<std::uint32_t>(i);
        auto &m = l.mips[mip];
        m.width = dim(d.width, mip, d.block);
        m.height = dim(d.height, mip, d.block);
        m.depth = three_d ? dim(d.depth, mip, 1) : 1;
        m.pitch = align_up(padded_dim(d.width, mip, d.block), align);
        m.rows = padded_dim(d.height, mip, d.block);
        m.depth_pitch = three_d ? d.depth : 1;
        m.offset = offset;
        offset += std::uint64_t{m.pitch} * m.rows * bpe;
    }
    l.block[0] = align;
    l.slices = d.depth;
    l.slice_size = offset;
    l.size = offset * d.depth;
    l.alignment = 256;
    l.first_tail_mip = d.mips;
    l.valid = true;
    return l;
}

Layout tiled_layout(const Description &d, std::uint32_t block_log2) {
    Layout l;
    l.desc = d;
    const auto element_log2 = log2_of(d.element_bytes);
    const std::uint64_t bpe = d.element_bytes, block_bytes = 1ull << block_log2;
    const bool three_d = d.dimension == Dimension::texture3d;
    const bool thick = three_d && (standard(d.mode) || display(d.mode));
    auto &block = l.block;
    if (thick) {
        // ComputeThickBlockDimension
        const auto amp = (block_log2 - 10) / 3, rest = (block_log2 - 10) % 3;
        const auto *const b = block_1k_3d[element_log2];
        block[0] = b[0] << amp;
        block[1] = b[1] << (amp + rest / 2);
        block[2] = b[2] << (amp + (rest != 0 ? 1 : 0));
    } else {
        // ComputeThinBlockDimension
        const auto samples_log2 = log2_of(d.samples);
        const auto elements_log2 = block_log2 - element_log2 - samples_log2;
        const std::uint32_t wide = (samples_log2 & 1) == 0 || (block_log2 & 1) != 0 ? 1 : 0;
        const auto w = (elements_log2 + wide) / 2;
        block[0] = 1u << w;
        block[1] = 1u << (elements_log2 - w);
        block[2] = 1;
    }
    const auto slices = align_up(d.depth, block[2]);
    // the mip depths a thick layout places (thin ones store depth as slices)
    const auto mip_depth = [&](std::uint32_t i) { return thick ? padded_dim(d.depth, i, 1) : 1; };
    const auto depth_of = [&](std::uint32_t i) { return three_d ? dim(d.depth, i, 1) : 1; };
    l.first_tail_mip = d.mips;
    std::uint64_t slice_size = 0;
    if (d.mips == 1) {
        Mip m;
        m.width = dim(d.width, 0, d.block);
        m.height = dim(d.height, 0, d.block);
        m.depth = depth_of(0);
        m.pitch = align_up(m.width, block[0]);
        m.rows = align_up(m.height, block[1]);
        m.depth_pitch = three_d ? slices : 1;
        l.mips.push_back(m);
        slice_size = std::uint64_t{m.pitch} * m.rows * bpe * d.samples;
    } else {
        // GetMipTailDim: the largest mip the tail takes
        std::uint32_t tail_max[3] = {block[0], block[1], block[2]};
        if (thick)
            tail_max[block_log2 % 3 == 0 ? 1 : block_log2 % 3 == 1 ? 0 : 2] >>= 1;
        else
            tail_max[0] >>= 1;
        // GetMaxNumMipsInTail; 256-byte blocks have no tail (ComputeSurfaceInfoMicroTiled)
        const auto effective = thick ? block_log2 - (block_log2 - 8) / 3 : block_log2;
        const std::uint32_t max_in_tail = effective == 8    ? 0
                                          : effective <= 11 ? 1 + (1u << (effective - 9))
                                                            : effective - 4;
        // dsMipmapHtileFix: 8- and 16-bit depth enter the tail as 32-bit
        std::uint32_t fixed[2] = {tail_max[0], tail_max[1]};
        if (d.mode == sw_64kb_z_x && element_log2 <= 1) {
            fixed[0] /= block_256_2d[element_log2][0] / block_256_2d[2][0];
            fixed[1] /= block_256_2d[element_log2][1] / block_256_2d[2][1];
        }
        std::vector<std::uint64_t> sizes;
        for (std::uint32_t i = 0; i < d.mips; ++i) {
            const auto w = padded_dim(d.width, i, d.block), h = padded_dim(d.height, i, d.block);
            if (w <= fixed[0] && h <= fixed[1] && d.mips - i <= max_in_tail) {
                l.first_tail_mip = i;
                slice_size += block_bytes / block[2];
                break;
            }
            Mip m;
            m.width = dim(d.width, i, d.block);
            m.height = dim(d.height, i, d.block);
            m.depth = depth_of(i);
            m.pitch = align_up(w, block[0]);
            m.rows = align_up(h, block[1]);
            m.depth_pitch = three_d ? align_up(mip_depth(i), block[2]) : 1;
            const auto one = std::uint64_t{m.pitch} * m.rows * bpe;
            slice_size += one;
            sizes.push_back(one * block[2]);
            l.mips.push_back(m);
        }
        // the larger mips from the smallest up, after the tail's block
        std::uint64_t offset = l.first_tail_mip < d.mips ? block_bytes : 0;
        for (auto i = static_cast<std::int32_t>(l.first_tail_mip) - 1; i >= 0; --i) {
            l.mips[i].offset = offset;
            offset += sizes[i];
        }
        const std::uint32_t micro_w = thick ? block_256_3d[element_log2][0]
                                            : block_256_2d[element_log2][0],
                            micro_h = thick ? block_256_3d[element_log2][1]
                                            : block_256_2d[element_log2][1];
        auto pitch = tail_max[0], rows = tail_max[1];
        for (auto i = l.first_tail_mip; i < d.mips; ++i) {
            const auto n = max_in_tail - 1 - (i - l.first_tail_mip);
            const std::uint32_t offset_in_tail = n > 6 ? 16u << n : n << 8;
            // address bits 8 and up of the offset: odd ones x, even ones y
            std::uint32_t x = 0, y = 0;
            for (std::uint32_t b = 0; b < 6; ++b) {
                x |= ((offset_in_tail >> (9 + 2 * b)) & 1) << b;
                y |= ((offset_in_tail >> (8 + 2 * b)) & 1) << b;
            }
            if (block_log2 & 1) {
                std::swap(x, y);
                if (element_log2 & 1) {
                    y = (y << 1) | (x & 1);
                    x >>= 1;
                }
            }
            Mip m;
            m.width = dim(d.width, i, d.block);
            m.height = dim(d.height, i, d.block);
            m.depth = depth_of(i);
            m.pitch = pitch;
            m.rows = rows;
            m.depth_pitch = thick ? align_up(mip_depth(i), block_256_3d[element_log2][2]) : 1;
            m.in_tail = true;
            m.tail = {offset_in_tail, x * micro_w, y * micro_h};
            l.mips.push_back(m);
            pitch = std::max(pitch >> 1, micro_w);
            rows = std::max(rows >> 1, micro_h);
        }
    }
    // a thick layout stores each depth slab's chain together
    l.thick = thick;
    l.slice_size = slice_size * (thick ? block[2] : 1);
    l.slices = slices / block[2];
    l.size = l.slice_size * l.slices;
    l.alignment = block_bytes;
    if (!xor_mode(d.mode) && d.samples == 1) {
        const std::uint32_t kind = thick ? 2 : display(d.mode) ? 1 : 0;
        const auto bits = pattern_bits(kind, element_log2, block_log2);
        for (std::size_t axis = 0; axis < 3; ++axis)
            l.pattern[axis] = spread(bits[axis]);
    }
    l.valid = true;
    return l;
}
} // namespace

Layout layout(const Description &d) {
    std::uint32_t block_log2 = 0;
    const auto within = [](std::uint32_t value, std::uint32_t last) {
        return value >= 1 && value <= last;
    };
    const bool three_d_mode = d.mode == 0 || d.mode == 5 || d.mode == 9 || d.mode == 17 ||
                              d.mode == 21 || (d.mode >= 24 && d.mode <= 27);
    const bool valid = block_log2_of(d.mode, block_log2) && within(d.width, max_extent) &&
                       within(d.height, max_extent) && within(d.depth, max_extent) &&
                       within(d.mips, max_mips) && power_of_two(d.element_bytes) &&
                       d.element_bytes <= 16 && (d.block == 1 || d.block == 4) &&
                       power_of_two(d.samples) && d.samples <= 8 &&
                       (d.samples == 1 || (d.dimension == Dimension::texture2d && d.mips == 1)) &&
                       (d.dimension != Dimension::texture1d || d.height == 1) &&
                       (d.dimension != Dimension::texture3d || three_d_mode);
    if (!valid)
        return {};
    return block_log2 ? tiled_layout(d, block_log2) : linear_layout(d);
}

bool Layout::addressable() const {
    return valid && (desc.mode == sw_linear || !pattern[0].empty());
}

std::uint64_t Layout::row(const Mip &m, std::uint32_t y, std::uint32_t z) const {
    // in the tail, the pattern of the shifted position gives the mip's place in the tail block
    y += m.in_tail ? m.tail.y : 0;
    const std::uint64_t blocks = std::uint64_t{y / block[1]} * (m.pitch / block[0]);
    return (blocks << log2_of(static_cast<std::uint32_t>(alignment))) +
           (pattern[1][y % block[1]] | pattern[2][z]);
}
std::uint64_t Layout::column(const Mip &m, std::uint32_t x) const {
    x += m.in_tail ? m.tail.x : 0;
    return (std::uint64_t{x / block[0]} << log2_of(static_cast<std::uint32_t>(alignment))) +
           pattern[0][x % block[0]];
}

bool Layout::offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                    std::uint64_t &at) const {
    if (!addressable() || mip >= mips.size())
        return false;
    const auto &m = mips[mip];
    const auto depth = desc.dimension == Dimension::texture3d ? m.depth : slices;
    if (x >= m.width || y >= m.height || z >= depth)
        return false;
    const auto slice = thick ? z / block[2] : z, inner = thick ? z % block[2] : 0;
    const auto base = std::uint64_t{slice} * slice_size + m.offset;
    if (desc.mode == sw_linear) {
        at = base + (std::uint64_t{y} * m.pitch + x) * desc.element_bytes;
        return true;
    }
    at = base + row(m, y, inner) + column(m, x);
    return true;
}

bool Layout::copy(std::uint8_t *tiled, std::size_t tiled_bytes, std::uint32_t mip,
                  std::uint32_t slice, std::uint8_t *linear, std::size_t linear_bytes,
                  std::size_t row_pitch, std::size_t depth_pitch, bool into) const {
    if (!addressable() || mip >= mips.size() || !tiled || !linear)
        return false;
    const auto &m = mips[mip];
    const bool three_d = desc.dimension == Dimension::texture3d;
    if ((three_d && slice != 0) || slice >= slices)
        return false;
    const std::size_t n = desc.element_bytes, bytes = std::size_t{m.width} * n;
    // the rows' extent, refused where it would not fit in a size_t
    constexpr auto most = ~std::size_t{0};
    const std::size_t depths = m.depth - 1, rows_before = m.height - 1;
    if ((depths && depth_pitch > most / depths) || (rows_before && row_pitch > most / rows_before))
        return false;
    const auto depth_part = depths * depth_pitch, row_part = rows_before * row_pitch;
    if (depth_part > most - row_part || depth_part + row_part > most - bytes)
        return false;
    const auto need = depth_part + row_part + bytes;
    if (tiled_bytes < size || linear_bytes < need || row_pitch < bytes)
        return false;
    const auto move = [&](std::size_t at_tiled, std::size_t at_linear, std::size_t length) {
        if (into)
            std::memcpy(tiled + at_tiled, linear + at_linear, length);
        else
            std::memcpy(linear + at_linear, tiled + at_tiled, length);
    };
    const bool is_linear = desc.mode == sw_linear;
    // the row and depth give every column one common base, so the same runs of elements that
    // lie together repeat on every row, the tail's mips included
    struct Run {
        std::size_t tiled, linear, bytes;
    };
    std::vector<Run> runs;
    if (!is_linear) {
        std::size_t first_linear = 0, first_tiled = static_cast<std::size_t>(column(m, 0));
        for (std::uint32_t x = 1; x < m.width; ++x) {
            const auto at = static_cast<std::size_t>(column(m, x));
            const auto linear_at = std::size_t{x} * n;
            if (at != first_tiled + (linear_at - first_linear)) {
                runs.push_back({first_tiled, first_linear, linear_at - first_linear});
                first_tiled = at;
                first_linear = linear_at;
            }
        }
        runs.push_back({first_tiled, first_linear, bytes - first_linear});
    }
    const auto first = three_d ? 0 : slice;
    for (std::uint32_t z = 0; z < m.depth; ++z) {
        const auto at_z = first + z;
        const auto s = thick ? at_z / block[2] : at_z, inner = thick ? at_z % block[2] : 0;
        const auto base = std::uint64_t{s} * slice_size + m.offset;
        for (std::uint32_t y = 0; y < m.height; ++y) {
            const auto linear_row = z * depth_pitch + y * row_pitch;
            if (is_linear) {
                move(static_cast<std::size_t>(base + std::uint64_t{y} * m.pitch * n), linear_row,
                     bytes);
                continue;
            }
            const auto tiled_row = static_cast<std::size_t>(base + row(m, y, inner));
            for (const auto &run : runs)
                move(tiled_row + run.tiled, linear_row + run.linear, run.bytes);
        }
    }
    return true;
}

bool format_of(std::uint32_t dxgi, Format &format) {
    const auto within = [&](std::uint32_t first, std::uint32_t last) {
        return dxgi >= first && dxgi <= last;
    };
    if (within(1, 4))
        format = {16, 1, false};
    else if (within(9, 18))
        format = {8, 1, false};
    else if (within(19, 22) || within(44, 47)) // depth with stencil
        format = {4, 1, true};
    else if (within(23, 43) || dxgi == 67 || within(87, 93) || dxgi == 116 || dxgi == 117)
        format = {4, 1, false};
    else if (within(48, 59) || dxgi == 85 || dxgi == 86 || dxgi == 115)
        format = {2, 1, false};
    else if (within(118, 120)) // the consoles' 16-bit depth with stencil
        format = {2, 1, true};
    else if (within(60, 65) || dxgi == 190) // and the consoles' own R4G4
        format = {1, 1, false};
    else if (within(70, 72) || within(79, 81)) // BC1, BC4
        format = {8, 4, false};
    else if (within(73, 78) || within(82, 84) || within(94, 99)) // BC2, BC3, BC5, BC6H, BC7
        format = {16, 4, false};
    else
        return false;
    return true;
}

Texture texture(Dimension dimension, std::uint32_t width, std::uint32_t height,
                std::uint32_t depth_or_array, std::uint32_t mips, std::uint32_t format,
                std::uint32_t samples, std::uint32_t mode) {
    Format f;
    if (!format_of(format, f))
        return {};
    const auto depth3d = dimension == Dimension::texture3d ? depth_or_array : 1;
    if (!mips)
        mips = log2_of(std::max({width, height, depth3d, 1u})) + 1;
    Description plane{dimension, width,   height,  depth_or_array,
                      mips,      f.bytes, f.block, std::max(samples, 1u),
                      mode};
    Texture t;
    t.mips = mips;
    t.planes.push_back({0, false, layout(plane)});
    if (!t.planes[0].layout.valid)
        return {};
    if (f.stencil) {
        plane.element_bytes = 1;
        auto stencil = layout(plane);
        if (!stencil.valid)
            return {};
        const auto offset = (t.planes[0].layout.size + stencil.alignment - 1) / stencil.alignment *
                            stencil.alignment;
        t.planes.push_back({offset, true, std::move(stencil)});
    }
    const auto &last = t.planes.back();
    t.size = last.offset + last.layout.size;
    for (const auto &p : t.planes)
        t.alignment = std::max(t.alignment, p.layout.alignment);
    t.valid = true;
    return t;
}
} // namespace xv::runtime::modules::gfx10
