#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// How the Series consoles' GPU (RDNA2, GFX10.3) lays a texture out in memory: its size and
// alignment, where each mip and array slice starts, the address of each element, and copies
// between that layout and rows. An element is a texel, or a 4x4 block of a block-compressed
// format. A slice's mips are stored smallest first: the mip tail (the small mips that share
// one block) at its start, mip 0 last.
//
// Ported code (see THIRD-PARTY.md), which follows
// AMD's address library (addrlib, as shipped in Mesa under src/amd/addrlib): block dimensions,
// mip placement and mip tails from gfx10addrlib.cpp and addrlib2.cpp, and the 256-byte micro
// block patterns of gfx10SwizzlePattern.h (its RB+ tables, as RDNA2 has). addrlib:
// Copyright (C) 2007-2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
namespace xv::runtime::modules::gfx10 {
// a swizzle mode by its hardware number, as addrlib's AddrSwizzleMode names them (ADDR_SW_*);
// the ones GFX10 has
enum SwizzleMode : std::uint32_t {
    sw_linear = 0,
    sw_256b_s = 1,
    sw_256b_d = 2,
    sw_4kb_s = 5,
    sw_4kb_d = 6,
    sw_64kb_s = 9,
    sw_64kb_d = 10,
    sw_64kb_s_t = 17,
    sw_64kb_d_t = 18,
    sw_4kb_s_x = 21,
    sw_4kb_d_x = 22,
    sw_4kb_r_x = 23,
    sw_64kb_z_x = 24,
    sw_64kb_s_x = 25,
    sw_64kb_d_x = 26,
    sw_64kb_r_x = 27,
};
enum class Dimension { texture1d, texture2d, texture3d };

// one plane of a texture
struct Description {
    Dimension dimension = Dimension::texture2d;
    std::uint32_t width = 1, height = 1; // in texels
    std::uint32_t depth = 1;             // of a 3D texture, else the array size
    std::uint32_t mips = 1;
    std::uint32_t element_bytes = 4; // 1, 2, 4, 8 or 16
    std::uint32_t block = 1;         // texels per element side: 1, or 4 for block compression
    std::uint32_t samples = 1;
    std::uint32_t mode = sw_linear;
};
struct Tail {
    std::uint32_t offset = 0;   // in the tail block
    std::uint32_t x = 0, y = 0; // where the mip starts in it, in elements
};
// where one mip lives in a slice
struct Mip {
    std::uint32_t width = 1, height = 1, depth = 1;     // in elements (depth 1 unless 3D)
    std::uint32_t pitch = 1, rows = 1, depth_pitch = 1; // padded, in elements
    std::uint64_t offset = 0; // of its first block in the slice (0 in the tail)
    bool in_tail = false;
    Tail tail;
};
struct Layout {
    bool valid = false;
    Description desc;
    std::uint32_t block[3] = {1, 1, 1}; // in elements (linear: the pitch alignment, 1, 1)
    bool thick = false;       // 3D blocks span block[2] depth slices (the 3D S and D modes)
    std::uint32_t slices = 1; // array slices, depth slabs (thick) or depth slices
    std::uint64_t slice_size = 0, size = 0, alignment = 1;
    std::uint32_t first_tail_mip = 0; // the number of mips without a tail
    std::vector<Mip> mips;
    // an element coordinate's offset in a block, per axis; empty where not addressable
    std::array<std::vector<std::uint32_t>, 3> pattern;

    // Linear and the modes without pipe or bank XOR, single-sampled. The XOR modes (_T, _X)
    // depend on the GPU's pipe and bank configuration, which differs between Series X and S:
    // their sizes and mip placement are known here, their element addresses are not.
    bool addressable() const;
    // the offset of element (x, y) of mip `mip` in array slice or depth slice `z`
    bool offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                std::uint64_t &at) const;
    // one subresource between this layout at `tiled` (`size` bytes) and rows `row_pitch`
    // apart, depth slices `depth_pitch` apart: mip `mip` of array slice `slice` (3D: every
    // depth slice of the mip, `slice` 0). `into` writes the layout.
    bool copy(std::uint8_t *tiled, std::size_t tiled_bytes, std::uint32_t mip, std::uint32_t slice,
              std::uint8_t *linear, std::size_t linear_bytes, std::size_t row_pitch,
              std::size_t depth_pitch, bool into) const;

  private:
    std::uint64_t row(const Mip &m, std::uint32_t y, std::uint32_t z) const;
    std::uint64_t column(const Mip &m, std::uint32_t x) const;
};
Layout layout(const Description &desc);

// what a DXGI format stores per element, and whether its stencil is a second plane
struct Format {
    std::uint32_t bytes = 0, block = 1;
    bool stencil = false;
};
// false for 96-bit, 1-bit, packed 4:2:2 and video formats, which have no layout here
bool format_of(std::uint32_t dxgi, Format &format);

struct Plane {
    std::uint64_t offset = 0; // from the start of the texture
    bool stencil = false;
    Layout layout;
};
// Every data plane of a texture (depth, then stencil), one after the other. The compression
// metadata planes (HTILE, CMASK, FMASK, DCC) the console adds to render targets and depth
// buffers are not here: the host draws those, and nothing reads that memory.
struct Texture {
    bool valid = false;
    std::vector<Plane> planes;
    std::uint64_t size = 0, alignment = 1;
    std::uint32_t mips = 0;
};
// `mips` 0: the full chain
Texture texture(Dimension dimension, std::uint32_t width, std::uint32_t height,
                std::uint32_t depth_or_array, std::uint32_t mips, std::uint32_t format,
                std::uint32_t samples, std::uint32_t mode);
} // namespace xv::runtime::modules::gfx10
