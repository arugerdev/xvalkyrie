#define NOMINMAX
#include "runtime/modules/xg.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/texture_layout.hpp"
#include <catch2/catch.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <vector>

// The Series console's texture layouts, and xg_xs.dll's texture computer over them called
// through its slots as a title calls them. Some values are checked by hand; the sweep
// compares every layout with a second computation written as addrlib's own code is (per-mip
// info, then each address bit from the swizzle equation).
namespace {
using namespace xv::runtime::modules;
using gfx10::Description;
using gfx10::Dimension;

Description desc(std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t bytes,
                 std::uint32_t mode) {
    Description d;
    d.width = w;
    d.height = h;
    d.mips = mips;
    d.element_bytes = bytes;
    d.mode = mode;
    return d;
}
std::uint64_t at(const gfx10::Layout &l, std::uint32_t mip, std::uint32_t x, std::uint32_t y,
                 std::uint32_t z = 0) {
    std::uint64_t offset = ~0ull;
    l.offset(mip, x, y, z, offset);
    return offset;
}

// addrlib's way: GFX10_SW_PATTERN_NIBBLE01 (address bits 0-7), NIBBLE2 (8-11) and NIBBLE3
// (12-15) rows as GFX10_SW_{256,4K,64K}_{S,D}_RBPLUS_PATINFO name them for 1-16 bytes per
// element; "0" a bit inside the element
constexpr const char *nibble01[2][5] = {
    {"X0 X1 X2 X3 Y0 Y1 Y2 Y3", "0 X0 X1 X2 Y0 Y1 Y2 X3", "0 0 X0 X1 Y0 Y1 Y2 X2",
     "0 0 0 X0 Y0 Y1 X1 X2", "0 0 0 0 Y0 Y1 X0 X1"},
    {"X0 X1 X2 Y1 Y0 Y2 X3 Y3", "0 X0 X1 X2 Y0 Y1 Y2 X3", "0 0 X0 X1 Y0 Y1 X2 Y2",
     "0 0 0 X0 Y0 X1 X2 Y1", "0 0 0 0 X0 Y0 X1 Y1"},
};
constexpr const char *nibble2[5] = {"Y4 X4 Y5 X5", "Y3 X4 Y4 X5", "Y3 X3 Y4 X4", "Y2 X3 Y3 X4",
                                    "Y2 X2 Y3 X3"};
constexpr const char *nibble3[5] = {"Y6 X6 Y7 X7", "Y5 X6 Y6 X7", "Y5 X5 Y6 X6", "Y4 X5 Y5 X6",
                                    "Y4 X4 Y5 X5"};
constexpr std::uint32_t micro[5][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
struct Bit {
    char axis = 0;
    std::uint32_t index = 0;
};
struct MipInfo {
    std::uint32_t pitch = 0, height = 0;
    std::uint64_t offset = 0;
    bool in_tail = false;
    std::uint32_t tail_x = 0, tail_y = 0;
};
struct Info {
    std::uint32_t block_w = 1, block_h = 1, block_log2 = 0, bpe = 1, first_tail = 0;
    std::uint64_t slice_size = 0;
    std::vector<MipInfo> mips;
    std::vector<Bit> eq;
};
std::uint32_t log2u(std::uint32_t v) {
    std::uint32_t r = 0;
    while (v > 1) {
        v >>= 1;
        ++r;
    }
    return r;
}
// ComputeSurfaceInfo{Linear,MicroTiled,MacroTiled}; width and height in elements
Info compute(std::uint32_t width, std::uint32_t height, std::uint32_t mips, std::uint32_t bpe,
             std::uint32_t mode) {
    Info out;
    const auto align = [](std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; };
    const auto size = [](std::uint32_t v, std::uint32_t i) {
        return std::max<std::uint32_t>((v + (1u << i) - 1) >> i, 1u);
    };
    out.bpe = bpe;
    out.mips.resize(mips);
    out.first_tail = mips;
    const auto index = log2u(bpe);
    if (mode == 0) {
        for (auto i = static_cast<int>(mips) - 1; i >= 0; --i) {
            const auto pitch = static_cast<std::uint32_t>(align(size(width, i), 256 / bpe));
            out.mips[i] = {pitch, size(height, i), out.slice_size};
            out.slice_size += std::uint64_t{pitch} * size(height, i) * bpe;
        }
        return out;
    }
    const bool display = mode == 2 || mode == 6 || mode == 10;
    const std::uint32_t log2 = mode <= 2 ? 8 : mode <= 6 ? 12 : 16;
    out.block_log2 = log2;
    out.block_w = micro[index][0] << ((log2 - 8) / 2);
    out.block_h = micro[index][1] << ((log2 - 8) - (log2 - 8) / 2);
    const auto read = [&](const char *row) {
        for (const char *p = row; *p;) {
            Bit bit;
            if (*p == 'X' || *p == 'Y') {
                bit.axis = *p++;
                bit.index = static_cast<std::uint32_t>(std::strtoul(p, nullptr, 10));
            }
            while (*p && *p != ' ')
                ++p;
            while (*p == ' ')
                ++p;
            out.eq.push_back(bit);
        }
    };
    read(nibble01[display][index]);
    if (log2 >= 12)
        read(nibble2[index]);
    if (log2 >= 16)
        read(nibble3[index]);
    out.eq.resize(log2);
    const std::uint64_t block = 1ull << log2;
    if (mips == 1 || log2 == 8) {
        for (auto i = static_cast<int>(mips) - 1; i >= 0; --i) {
            const auto pitch = static_cast<std::uint32_t>(align(size(width, i), out.block_w));
            const auto rows = static_cast<std::uint32_t>(align(size(height, i), out.block_h));
            out.mips[i] = {pitch, rows, out.slice_size};
            out.slice_size += std::uint64_t{pitch} * rows * bpe;
        }
        return out;
    }
    const auto tail_w = out.block_w >> 1, tail_h = out.block_h;
    const std::uint32_t max_tail = log2 <= 11 ? 1 + (1u << (log2 - 9)) : log2 - 4;
    std::vector<std::uint64_t> sizes(mips);
    for (std::uint32_t i = 0; i < mips; ++i) {
        const auto w = size(width, i), h = size(height, i);
        if (w <= tail_w && h <= tail_h && mips - i <= max_tail) {
            out.first_tail = i;
            out.slice_size += block;
            break;
        }
        out.mips[i].pitch = static_cast<std::uint32_t>(align(w, out.block_w));
        out.mips[i].height = static_cast<std::uint32_t>(align(h, out.block_h));
        sizes[i] = std::uint64_t{out.mips[i].pitch} * out.mips[i].height * bpe;
        out.slice_size += sizes[i];
    }
    std::uint64_t offset = out.first_tail == mips ? 0 : block;
    for (auto i = static_cast<int>(out.first_tail) - 1; i >= 0; --i) {
        out.mips[i].offset = offset;
        offset += sizes[i];
    }
    std::uint32_t pitch = tail_w, rows = tail_h;
    for (auto i = out.first_tail; i < mips; ++i) {
        const auto m = max_tail - 1 - (i - out.first_tail);
        const std::uint32_t mip_offset = m > 6 ? 16u << m : m << 8;
        std::uint32_t x = 0, y = 0;
        for (std::uint32_t b = 0; b < 6; ++b) {
            x |= ((mip_offset >> (9 + 2 * b)) & 1) << b;
            y |= ((mip_offset >> (8 + 2 * b)) & 1) << b;
        }
        out.mips[i] = {pitch, rows, mip_offset, true, x * micro[index][0], y * micro[index][1]};
        pitch = std::max(pitch >> 1, micro[index][0]);
        rows = std::max(rows >> 1, micro[index][1]);
    }
    return out;
}
// ComputeSurfaceAddrFromCoord{Linear,MicroTiled,MacroTiled}
std::uint64_t reference(const Info &info, std::uint32_t mip, std::uint32_t slice, std::uint32_t x,
                        std::uint32_t y) {
    const auto &m = info.mips[mip];
    const auto base = info.slice_size * slice;
    if (!info.block_log2)
        return base + m.offset + (std::uint64_t{y} * m.pitch + x) * info.bpe;
    if (m.in_tail) {
        x += m.tail_x;
        y += m.tail_y;
    }
    const std::uint64_t blocks =
        std::uint64_t{y / info.block_h} * (m.pitch / info.block_w) + x / info.block_w;
    std::uint64_t in_block = 0;
    for (std::uint32_t bit = 0; bit < info.eq.size(); ++bit)
        if (info.eq[bit].axis)
            in_block |=
                std::uint64_t{((info.eq[bit].axis == 'X' ? x : y) >> info.eq[bit].index) & 1}
                << bit;
    return base + (m.in_tail ? 0 : m.offset) + (blocks << info.block_log2) + in_block;
}

D3D12_RESOURCE_DESC
resource(std::uint64_t width, UINT height, UINT16 mips, DXGI_FORMAT format, UINT layout,
         UINT16 depth = 1,
         D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = dimension;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = depth;
    d.MipLevels = mips;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(layout);
    return d;
}
// method `slot` of a computer, as a title calls it
template <typename F> F slot(void *computer, int index) {
    return reinterpret_cast<F>((*static_cast<void ***>(computer))[index]);
}
using Release = std::uint32_t(WINAPI *)(void *);
} // namespace

TEST_CASE("the layouts' blocks hold their size in elements") {
    for (const std::uint32_t mode : {gfx10::sw_256b_s, gfx10::sw_4kb_s, gfx10::sw_64kb_d})
        for (std::uint32_t e = 0; e < 5; ++e) {
            const auto l = gfx10::layout(desc(1, 1, 1, 1u << e, mode));
            REQUIRE(l.valid);
            CHECK((std::uint64_t{l.block[0]} * l.block[1] * l.block[2] << e) == l.alignment);
            CHECK(l.pattern[0].size() == l.block[0]);
            CHECK(l.pattern[1].size() == l.block[1]);
        }
    const auto l = gfx10::layout(desc(64, 64, 1, 16, gfx10::sw_64kb_d));
    CHECK(l.block[0] == 64);
    CHECK(l.block[1] == 64);
    CHECK(l.size == 65536);
}

TEST_CASE("the layouts' addresses are the hand-checked ones") {
    // a 64 KiB D block of 4-byte elements: x bits at 2, 3, 6, 9, 11, 13, 15, y at 4, 5, 7, 8,
    // 10, 12, 14
    auto l = gfx10::layout(desc(128, 128, 1, 4, gfx10::sw_64kb_d));
    CHECK(at(l, 0, 1, 0) == 1 << 2);
    CHECK(at(l, 0, 4, 0) == 1 << 6);
    CHECK(at(l, 0, 0, 4) == 1 << 7);
    CHECK(at(l, 0, 0, 8) == 1 << 8);
    CHECK(at(l, 0, 64, 64) == 0xc000);
    // a 4 KiB S block of 16-byte elements is 16x16, y first; blocks row by row
    l = gfx10::layout(desc(32, 32, 1, 16, gfx10::sw_4kb_s));
    CHECK(l.block[0] == 16);
    CHECK(at(l, 0, 0, 1) == 16);
    CHECK(at(l, 0, 1, 0) == 64);
    CHECK(at(l, 0, 16, 0) == 4096);
    CHECK(at(l, 0, 0, 16) == 8192);
}

TEST_CASE("the mip tail comes first") {
    // a 256x256 BC7 chain in 64 KiB D: one tail block with mips 1-8, then mip 0; mip 1 at
    // x = 32 elements, 32 KiB into the tail
    auto l = gfx10::layout(desc(64, 64, 9, 16, gfx10::sw_64kb_d));
    CHECK(l.first_tail_mip == 1);
    CHECK(l.size == 2 * 65536);
    CHECK(l.mips[0].offset == 65536);
    CHECK(l.mips[1].in_tail);
    CHECK(l.mips[1].tail.offset == 32768);
    CHECK(l.mips[1].tail.x == 32);
    CHECK(l.mips[1].tail.y == 0);
    CHECK(at(l, 1, 0, 0) == 32768);
    // 4 KiB blocks hold fewer tail mips
    l = gfx10::layout(desc(64, 64, 9, 16, gfx10::sw_4kb_s));
    CHECK(l.first_tail_mip == 3);
    CHECK(l.mips[2].offset == 4096);
    CHECK(l.mips[1].offset == 8192);
    CHECK(l.mips[0].offset == 8192 + 16384);
    // linear: rows of 256 bytes at least, the smallest mip first
    l = gfx10::layout(desc(30, 20, 2, 4, gfx10::sw_linear));
    CHECK(l.mips[1].offset == 0);
    CHECK(l.mips[1].pitch == 64);
    CHECK(l.mips[0].offset == 64 * 10 * 4);
}

TEST_CASE("every element has its own address and copies give the rows back") {
    auto with = [](Description d, std::uint32_t depth, Dimension dimension, std::uint32_t block) {
        d.depth = depth;
        d.dimension = dimension;
        d.block = block;
        return d;
    };
    const Description cases[] = {
        desc(64, 64, 7, 16, gfx10::sw_64kb_d),
        desc(100, 60, 7, 4, gfx10::sw_64kb_d),
        desc(250, 175, 8, 8, gfx10::sw_4kb_s),
        desc(33, 17, 6, 1, gfx10::sw_4kb_d),
        desc(40, 9, 3, 2, gfx10::sw_64kb_s),
        desc(30, 20, 5, 4, gfx10::sw_linear),
        desc(83, 31, 6, 1, gfx10::sw_256b_s),
        with(desc(109, 71, 6, 16, gfx10::sw_256b_d), 1, Dimension::texture2d, 4),
        with(desc(70, 70, 4, 16, gfx10::sw_4kb_s), 3, Dimension::texture2d, 1),
        with(desc(64, 32, 7, 4, gfx10::sw_4kb_s), 16, Dimension::texture3d, 1),
        with(desc(48, 40, 5, 2, gfx10::sw_64kb_s), 20, Dimension::texture3d, 1),
        with(desc(20, 12, 3, 8, gfx10::sw_linear), 5, Dimension::texture3d, 1),
    };
    for (const auto &d : cases) {
        const auto l = gfx10::layout(d);
        REQUIRE(l.valid);
        REQUIRE(l.addressable());
        CHECK(l.thick == (d.dimension == Dimension::texture3d && d.mode != gfx10::sw_linear));
        std::vector<bool> seen(static_cast<std::size_t>(l.size));
        std::vector<std::uint8_t> tiled(static_cast<std::size_t>(l.size));
        const std::size_t n = d.element_bytes;
        const auto slices = d.dimension == Dimension::texture3d ? 1u : d.depth;
        for (std::uint32_t slice = 0; slice < slices; ++slice)
            for (std::uint32_t mip = 0; mip < d.mips; ++mip) {
                const auto &m = l.mips[mip];
                const std::size_t active = m.width * n, row = active + 13,
                                  plane = row * m.height + 11;
                std::vector<std::uint8_t> linear(plane * m.depth);
                for (std::size_t i = 0; i < linear.size(); ++i)
                    linear[i] = static_cast<std::uint8_t>(i * 7 + mip * 13 + slice);
                REQUIRE(l.copy(tiled.data(), tiled.size(), mip, slice, linear.data(), linear.size(),
                               row, plane, true));
                std::vector<std::uint8_t> back(linear.size(), 0xcc);
                REQUIRE(l.copy(tiled.data(), tiled.size(), mip, slice, back.data(), back.size(),
                               row, plane, false));
                bool same = true, alone = true, inside = true;
                for (std::uint32_t z = 0; z < m.depth; ++z)
                    for (std::uint32_t y = 0; y < m.height; ++y) {
                        const auto start = z * plane + y * row;
                        same = same && !std::memcmp(&back[start], &linear[start], active) &&
                               back[start + active] == 0xcc;
                        for (std::uint32_t x = 0; x < m.width; ++x) {
                            const auto address = at(l, mip, x, y, slice + z);
                            if (address + n > seen.size() || address % n) {
                                inside = false;
                                continue;
                            }
                            same = same && !std::memcmp(&tiled[address], &linear[start + x * n], n);
                            alone = alone && !seen[address];
                            seen[address] = true;
                        }
                    }
                CHECK(same);
                CHECK(alone);
                CHECK(inside);
            }
    }
}

TEST_CASE("the layouts are addrlib's, for every 2D mode and element size") {
    const std::uint32_t formats[] = {61, 49, 28, 10, 2, 71, 80, 77, 83, 98}; // 1-16 bytes, BCn
    const std::uint32_t modes[] = {0, 1, 2, 5, 6, 9, 10};
    const std::uint32_t sizes[][2] = {{1, 1},       {3, 5},      {17, 9},     {64, 64},
                                      {100, 60},    {250, 175},  {333, 1024}, {1024, 33},
                                      {1920, 1080}, {3840, 2160}};
    std::uint32_t random = 12345;
    const auto next = [&] {
        random = random * 1664525u + 1013904223u;
        return random >> 8;
    };
    std::size_t layouts = 0, mismatches = 0;
    for (const auto format : formats)
        for (const auto mode : modes)
            for (const auto &size : sizes)
                for (const std::uint32_t mips : {1u, 0u, 3u})
                    for (const std::uint32_t slices : {1u, 3u}) {
                        const auto t = gfx10::texture(Dimension::texture2d, size[0], size[1],
                                                      slices, mips, format, 1, mode);
                        REQUIRE(t.valid);
                        const auto &l = t.planes[0].layout;
                        ++layouts;
                        const auto block = l.desc.block;
                        const auto info =
                            compute((size[0] + block - 1) / block, (size[1] + block - 1) / block,
                                    t.mips, l.desc.element_bytes, mode);
                        if (l.slice_size != info.slice_size || l.size != info.slice_size * slices ||
                            l.first_tail_mip != info.first_tail)
                            ++mismatches;
                        for (std::uint32_t slice = 0; slice < slices; ++slice)
                            for (std::uint32_t mip = 0; mip < t.mips; ++mip) {
                                const auto &m = l.mips[mip];
                                if (m.pitch != info.mips[mip].pitch ||
                                    m.rows != info.mips[mip].height)
                                    ++mismatches;
                                const auto compare = [&](std::uint32_t x, std::uint32_t y) {
                                    if (at(l, mip, x, y, slice) !=
                                        reference(info, mip, slice, x, y))
                                        ++mismatches;
                                };
                                if (std::uint64_t{m.width} * m.height <= 4096) {
                                    for (std::uint32_t y = 0; y < m.height; ++y)
                                        for (std::uint32_t x = 0; x < m.width; ++x)
                                            compare(x, y);
                                } else {
                                    for (int i = 0; i < 512; ++i)
                                        compare(next() % m.width, next() % m.height);
                                    compare(m.width - 1, m.height - 1);
                                }
                            }
                    }
    CHECK(layouts == 10 * 7 * 10 * 3 * 2);
    CHECK(mismatches == 0);
}

TEST_CASE("the XOR modes have sizes but no addresses, and bad descriptions no layout") {
    // depth and stencil are two planes: 4-byte depth in 128x128 blocks, stencil in 256x256
    const auto t =
        gfx10::texture(Dimension::texture2d, 1920, 1080, 1, 1, 20, 1, gfx10::sw_64kb_z_x);
    REQUIRE(t.valid);
    REQUIRE(t.planes.size() == 2);
    CHECK(t.planes[0].layout.size == 1920ull * 1152 * 4);
    CHECK(t.planes[1].offset == t.planes[0].layout.size);
    CHECK(t.planes[1].layout.size == 2048ull * 1280);
    CHECK_FALSE(t.planes[0].layout.addressable());
    CHECK_FALSE(gfx10::layout(desc(0, 4, 1, 4, gfx10::sw_4kb_s)).valid);
    CHECK_FALSE(gfx10::layout(desc(4, 4, 1, 12, gfx10::sw_4kb_s)).valid);
    CHECK_FALSE(gfx10::layout(desc(4, 4, 1, 4, 3)).valid); // 256B_R: not on GFX10
    CHECK_FALSE(gfx10::layout(desc(16385, 1, 1, 16, gfx10::sw_linear)).valid);
    const auto l = gfx10::layout(desc(64, 64, 1, 4, gfx10::sw_4kb_s));
    std::vector<std::uint8_t> tiled(100), rows(64 * 64 * 4);
    CHECK_FALSE(l.copy(tiled.data(), tiled.size(), 0, 0, rows.data(), rows.size(), 256, 0, false));
    std::uint64_t offset = 0;
    CHECK_FALSE(l.offset(0, 64, 0, 0, offset));
    std::vector<std::uint8_t> whole(static_cast<std::size_t>(l.size));
    CHECK_FALSE(l.copy(whole.data(), whole.size(), 0, 0, rows.data(), rows.size(), ~std::size_t{0},
                       0, false));
}

TEST_CASE("xg_xs.dll's texture computer, called as a title calls it") {
    // a 256x256 BC7 chain in 64 KiB D
    auto d = resource(256, 256, 0, DXGI_FORMAT_BC7_UNORM, 0x10a);
    void *const computer = series_texture_computer(&d);
    REQUIRE(computer);
    std::vector<std::uint8_t> layout(0x1b40, 0xcc);
    REQUIRE(slot<std::int32_t(WINAPI *)(void *, void *)>(computer, 2)(computer, layout.data()) ==
            0);
    const auto u64 = [&](std::size_t offset) {
        std::uint64_t value;
        std::memcpy(&value, &layout[offset], sizeof value);
        return value;
    };
    const auto u32 = [&](std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, &layout[offset], sizeof value);
        return value;
    };
    CHECK(u64(0) == 131072); // size
    CHECK(u64(8) == 65536);  // alignment
    CHECK(u32(0x10) == 9);   // mips
    CHECK(u32(0x14) == 1);   // planes
    constexpr std::size_t plane = 0x18, mip = plane + 0x38;
    CHECK(u32(plane + 0x28) == 16); // bytes per element
    CHECK(u32(plane + 0x2c) == 1);  // mips outside the tail
    CHECK(u32(plane + 0x30) == 8);  // mips in it
    CHECK(u64(mip + 8) == 65536);   // mip 0's offset
    CHECK(u32(mip + 0x1c) == 1024); // and pitch in bytes
    CHECK(u64(mip + 0x70 + 8) == 32768);
    CHECK(u32(mip + 0x70 + 0x64) == 1); // mip 1 in the tail
    CHECK(slot<std::uint64_t(WINAPI *)(void *)>(computer, 3)(computer) == 131072);
    CHECK(slot<std::uint64_t(WINAPI *)(void *)>(computer, 4)(computer) == 65536);
    CHECK(slot<std::uint64_t(WINAPI *)(void *, UINT, UINT)>(computer, 5)(computer, 0, 0) == 65536);
    CHECK(slot<std::uint64_t(WINAPI *)(void *, UINT, UINT)>(computer, 5)(computer, 0, 1) == 32768);

    // copies into and out of each mip
    using Into = std::int32_t(WINAPI *)(void *, void *, UINT, UINT, const void *, UINT, UINT);
    using From = std::int32_t(WINAPI *)(void *, void *, UINT, UINT, UINT, const void *, UINT, UINT);
    const auto into = slot<Into>(computer, 8);
    const auto from = slot<From>(computer, 9);
    std::vector<std::uint8_t> tiled(131072);
    for (UINT level = 0; level < 9; ++level) {
        const UINT blocks = std::max(64u >> level, 1u), row = blocks * 16;
        std::vector<std::uint8_t> rows(std::size_t{row} * blocks), back(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i)
            rows[i] = static_cast<std::uint8_t>(i ^ level);
        CHECK(into(computer, tiled.data(), 0, level, rows.data(), row, 0) == 0);
        CHECK(from(computer, back.data(), 0, level, 0, tiled.data(), row, 0) == 0);
        CHECK(back == rows);
    }
    CHECK(into(computer, nullptr, 0, 0, tiled.data(), 1024, 0) == E_INVALIDARG);
    CHECK(into(computer, tiled.data(), 1, 0, tiled.data(), 1024, 0) == E_INVALIDARG);

    // the 64 KiB tiling: one tile for the tail, one for mip 0
    using Tiling = std::int32_t(WINAPI *)(void *, UINT *, void *, UINT *, UINT *, UINT, void *);
    UINT tiles = 0, shape[3] = {}, count = 4;
    std::uint8_t packed[12] = {};
    UINT tilings[4 * 3] = {};
    REQUIRE(slot<Tiling>(computer, 10)(computer, &tiles, packed, shape, &count, 0, tilings) == 0);
    CHECK(tiles == 2);
    CHECK(packed[0] == 1); // standard mips
    CHECK(packed[1] == 8); // packed ones
    UINT packed_tiles;
    std::memcpy(&packed_tiles, packed + 4, sizeof packed_tiles);
    CHECK(packed_tiles == 1);
    CHECK(shape[0] == 256);
    CHECK(shape[1] == 256);
    CHECK(count == 1);
    CHECK(tilings[2] == 1); // mip 0 starts at tile 1
    CHECK(slot<Release>(computer, 1)(computer) == 0);

    // an element's offset: linear rows of 1 KiB (128 8-byte elements), 0 outside
    d = resource(128, 4, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0x100);
    void *const linear = series_texture_computer(&d);
    REQUIRE(linear);
    using Offset =
        std::uint64_t(WINAPI *)(void *, UINT, UINT, std::uint64_t, UINT, UINT, UINT, UINT *);
    UINT bit = 7;
    CHECK(slot<Offset>(linear, 6)(linear, 0, 0, 3, 1, 0, 0, &bit) == (128 + 3) * 8);
    CHECK(bit == 0);
    CHECK(slot<Offset>(linear, 6)(linear, 0, 0, 128, 0, 0, 0, nullptr) == 0);
    CHECK(slot<Release>(linear, 1)(linear) == 0);

    // a 4 KiB mode has no 64 KiB tiles; an XOR mode no copies
    d = resource(256, 256, 0, DXGI_FORMAT_BC7_UNORM, 0x105);
    void *const four_kib = series_texture_computer(&d);
    REQUIRE(four_kib);
    CHECK(slot<Tiling>(four_kib, 10)(four_kib, &tiles, nullptr, nullptr, nullptr, 0, nullptr) ==
          E_FAIL);
    CHECK(slot<Release>(four_kib, 1)(four_kib) == 0);
    d = resource(64, 64, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0x11a);
    void *const xored = series_texture_computer(&d);
    REQUIRE(xored);
    CHECK(slot<std::uint64_t(WINAPI *)(void *)>(xored, 3)(xored) == 65536);
    CHECK(into(xored, tiled.data(), 0, 0, tiled.data(), 256, 0) == E_NOTIMPL);
    CHECK(slot<Release>(xored, 1)(xored) == 0);

    // descriptions without a swizzle mode, or with one GFX10 has not, keep the plain layout
    d = resource(256, 256, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    CHECK_FALSE(series_texture_computer(&d));
    d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x103);
    CHECK_FALSE(series_texture_computer(&d));
    d = resource(256, 256, 1, DXGI_FORMAT_R32G32B32_FLOAT, 0x105);
    CHECK_FALSE(series_texture_computer(&d));
    CHECK_FALSE(series_texture_computer(nullptr));
}

TEST_CASE("xg_xs.dll picks a texture's mode where its description names none") {
    xv::runtime::Registry registry;
    install_xg(registry, "xg_xs.dll");
    using Suggest = std::int32_t(WINAPI *)(const D3D12_RESOURCE_DESC *, UINT, UINT *);
    using Create = std::int32_t(WINAPI *)(const D3D12_RESOURCE_DESC *, void **);
    const auto suggest =
        reinterpret_cast<Suggest>(registry.find("xg_xs.dll", "XGSuggestSwizzleMode"));
    const auto create =
        reinterpret_cast<Create>(registry.find("xg_xs.dll", "XGCreateTextureComputer"));
    REQUIRE(suggest);
    REQUIRE(create);
    // as a title calls it: the mode goes to the description's own layout field
    auto d = resource(256, 256, 0, DXGI_FORMAT_BC7_UNORM, 0);
    UINT layout = 0;
    REQUIRE(suggest(&d, 0, &layout) == 0);
    CHECK(layout == (0x100 | gfx10::sw_4kb_s));
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    REQUIRE(suggest(&d, 0, &layout) == 0);
    CHECK(layout == (0x100 | gfx10::sw_64kb_r_x));
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    REQUIRE(suggest(&d, 0, &layout) == 0);
    CHECK(layout == (0x100 | gfx10::sw_64kb_z_x));
    auto line =
        resource(256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, D3D12_RESOURCE_DIMENSION_TEXTURE1D);
    REQUIRE(suggest(&line, 0, &layout) == 0);
    CHECK(layout == 0x100);
    // a description with no mode is laid out as with the one picked for it
    using Bytes = std::uint64_t(WINAPI *)(void *);
    const auto bytes = [&](UINT mode) {
        auto texture = resource(256, 256, 0, DXGI_FORMAT_BC7_UNORM, mode);
        void *computer = nullptr;
        REQUIRE(create(&texture, &computer) == 0);
        REQUIRE(computer);
        const auto size = slot<Bytes>(computer, 3)(computer);
        CHECK(slot<Release>(computer, 1)(computer) == 0);
        return size;
    };
    CHECK(bytes(0) == bytes(0x100 | gfx10::sw_4kb_s));
    CHECK(bytes(1) == bytes(0x100 | gfx10::sw_linear));
}
