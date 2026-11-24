// BCPack. Ported code: see THIRD-PARTY.md.
//
// A packed stream has no header. It is 12 or 16 interleaved rANS lanes; each lane is a run
// of fragments (header byte, length, payload) coded with one of 128 static models, and a
// fixed schedule decides which lane takes the next input byte. The lanes' symbols form one
// record per block: two back references (to one of the last 256 blocks, or to a per-format
// dictionary that stands in for blocks -1 to -255) and the residuals of the endpoints and
// indices against them. The models, the index rank tables, the BC7 control codes and the
// dictionaries are constants of the format, which this code does not have: they are given
// to it (see bcpack.hpp).
#include "runtime/modules/bcpack.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace xv::runtime::modules::bcpack {
constexpr std::size_t models = 128, buckets = 9, history_size = 256, dictionary_size = 255;
constexpr std::uint32_t low = 1u << 24; // rANS: decode while above, read while below
constexpr unsigned round_bytes = 8;     // input bytes per schedule round
static_assert(tables_size ==
              models * buckets * 2 + 256 + 64 + 256 + 64 + 64 + 512 * 2 +
                  dictionary_size * (6 + 16 + 8 + 32 + 2 + 16 + 4 + 32 + 25 + 33 + 50 + 66));

// endpoints and indices of BC1, BC3, BC4 and BC5: small unsigned values, summed per element
// modulo the element's width
struct Plain {
    std::array<std::uint8_t, 8> ep{};
    std::array<std::uint8_t, 32> ix{};
};
struct Bc6Ep {
    std::array<std::array<std::uint16_t, 3>, 4> e{}; // endpoint 0 R G B, then 1-3
};
struct Bc6Ix {
    std::uint8_t mode = 0;
    std::array<std::int16_t, 16> ix{};
};
struct Bc7Ep {
    std::uint16_t ctrl = 0;
    std::array<std::array<std::uint16_t, 8>, 3> rows{}; // per subset R0 R1 G0 G1 B0 B1 A0 A1
};
struct Bc7Ix {
    std::array<std::uint8_t, 2> bits{};
    std::array<std::array<std::int16_t, 16>, 2> ix{};
};

struct Tables {
    std::array<std::array<std::uint32_t, buckets>, models> freq{};
    std::array<std::array<std::uint32_t, buckets + 1>, models> start{};
    std::array<std::array<std::uint8_t, 256>, models> bucket{};
    std::array<std::array<std::uint64_t, buckets>, models> recip{};
    std::array<std::uint8_t, 256> rank1{}, rank3c{};
    std::array<std::uint8_t, 64> rank3a{}, rank4{}, rank5{};
    std::array<std::uint16_t, 512> ctrl7{};
    std::vector<Plain> bc1, bc3, bc4, bc5;
    std::vector<std::pair<Bc6Ep, Bc6Ix>> bc6;
    std::vector<std::pair<Bc7Ep, Bc7Ix>> bc7;
};

namespace {
struct Reader {
    const std::uint8_t *at;
    std::uint8_t u8() { return *at++; }
    std::uint16_t u16() {
        const std::uint16_t v = static_cast<std::uint16_t>(at[0] | at[1] << 8);
        at += 2;
        return v;
    }
};

} // namespace

// 128 x 9 u16 model frequencies, the rank tables, 512 u16 BC7 control words, then 255
// dictionary entries per format in format order
std::shared_ptr<const Tables> parse_tables(std::span<const std::uint8_t> bytes,
                                           std::string *reason) {
    const char *why = nullptr;
    const auto parsed = [&]() -> std::shared_ptr<Tables> {
        if (bytes.size() != tables_size) {
            why = "the tables are not 77,982 bytes";
            return nullptr;
        }
        auto t = std::make_shared<Tables>();
        Reader r{bytes.data()};
        for (std::size_t m = 0; m < models; ++m) {
            for (auto &f : t->freq[m])
                f = r.u16();
            // model 16 g + shape has g + 2 buckets; bucket b > 0 holds 2^(b-1) symbols
            const std::size_t used = m / 16 + 2;
            std::uint32_t at = 0;
            for (std::size_t b = 0; b < used; ++b) {
                if (!t->freq[m][b]) {
                    why = "a model has an empty bucket";
                    return nullptr;
                }
                t->start[m][b] = at;
                t->recip[m][b] = ((1ull << 32) + t->freq[m][b] - 1) / t->freq[m][b];
                at += t->freq[m][b] << (b ? b - 1 : 0);
            }
            if (at != 1u << 16) {
                why = "a model does not tile 65536";
                return nullptr;
            }
            for (std::size_t b = used; b <= buckets; ++b)
                t->start[m][b] = 1u << 16;
            for (std::size_t window = 0; window < 256; ++window) {
                const std::uint32_t first = static_cast<std::uint32_t>(window) << 8;
                std::uint8_t count = 0;
                for (std::size_t b = 1; b <= buckets; ++b)
                    count += t->start[m][b] <= first;
                t->bucket[m][window] = count;
            }
        }
        for (auto &v : t->rank1)
            v = r.u8();
        for (auto &v : t->rank3a)
            v = r.u8();
        for (auto &v : t->rank3c)
            v = r.u8();
        for (auto &v : t->rank4)
            v = r.u8();
        for (auto &v : t->rank5)
            v = r.u8();
        for (auto &c : t->ctrl7)
            c = r.u16();
        const auto plain = [&](std::size_t ep, std::size_t ix) {
            std::vector<Plain> entries(dictionary_size);
            for (auto &p : entries) {
                for (std::size_t k = 0; k < ep; ++k)
                    p.ep[k] = r.u8();
                for (std::size_t k = 0; k < ix; ++k)
                    p.ix[k] = r.u8();
            }
            return entries;
        };
        t->bc1 = plain(6, 16);
        t->bc3 = plain(8, 32);
        t->bc4 = plain(2, 16);
        t->bc5 = plain(4, 32);
        t->bc6.resize(dictionary_size);
        for (auto &[ep, ix] : t->bc6) {
            r.u8(); // the endpoint entry's mode byte plays no part in decoding
            for (auto &row : ep.e)
                for (auto &v : row)
                    v = r.u16();
            ix.mode = r.u8();
            for (auto &v : ix.ix)
                v = static_cast<std::int16_t>(r.u16());
        }
        t->bc7.resize(dictionary_size);
        for (auto &[ep, ix] : t->bc7) {
            ep.ctrl = r.u16();
            for (auto &row : ep.rows)
                for (auto &v : row)
                    v = r.u16();
            ix.bits = {r.u8(), r.u8()};
            for (auto &set : ix.ix)
                for (auto &v : set)
                    v = static_cast<std::int16_t>(r.u16());
        }
        return t;
    }();
    if (!parsed && reason)
        *reason = why;
    return parsed;
}

namespace {
// Lanes.
struct Lane {
    std::uint8_t width = 0; // symbol bits - 1 at the full alphabet
    std::uint8_t phase = 0; // 0 header, 1 length (low byte), 2 length high byte, 3 payload
    bool is_long = false, reset = false;
    std::uint8_t model = 0;
    bool need = false;
    std::optional<std::uint8_t> latch, out;
    bool stalled = false;
    std::uint32_t x = 0, left = 0;

    std::size_t bucket_of(const Tables &t, std::uint32_t slot) const {
        const auto &start = t.start[model];
        std::size_t b = t.bucket[model][slot >> 8];
        while (start[b + 1] <= slot)
            ++b;
        return b;
    }
    // one step: take the latched byte (header, length or payload) and decode at most one
    // symbol
    void step(const Tables &t) {
        if (phase != 3) {
            if (!latch)
                return;
            const std::uint8_t b = *latch;
            latch.reset();
            need = true;
            switch (phase) {
            case 0: {
                // the top two bits shrink the alphabet; width >= 3
                const std::uint8_t group = static_cast<std::uint8_t>(width - (b >> 6));
                model = static_cast<std::uint8_t>(group * 16 + (b & 15));
                reset = b & 0x10;
                is_long = b & 0x20;
                left = 0;
                phase = 1;
                break;
            }
            case 1:
                if (is_long) {
                    left = b;
                    phase = 2;
                } else {
                    left = std::uint32_t{b} + 1;
                    phase = 3;
                }
                break;
            default:
                left = (std::uint32_t{b} << 8 | left) + 257;
                phase = 3;
                break;
            }
            return;
        }
        std::uint32_t state = x;
        if (state < low && latch) {
            --left;
            state = state << 8 | *latch;
            latch.reset();
        }
        need = left != 0 && !latch;
        if (state > low) {
            const auto &f = t.freq[model];
            const auto &start = t.start[model];
            const std::uint32_t slot = state & 0xffff;
            const auto b = bucket_of(t, slot);
            // bucket 0 holds symbol 0, which owns slots 1 .. f[0] - 1; bucket b > 0 holds
            // 2^(b-1) symbols of frequency f[b] from start[b] on
            std::uint32_t symbol, frequency, rest;
            if (b == 0) {
                symbol = 0;
                frequency = f[0] - 1;
                rest = slot - 1;
            } else {
                const std::uint32_t offset = slot - start[b];
                const auto q =
                    static_cast<std::uint32_t>((std::uint64_t{offset} * t.recip[model][b]) >> 32);
                symbol = (1u << (b - 1)) + q;
                frequency = f[b];
                rest = offset - q * f[b];
            }
            x = (state >> 16) * frequency + rest;
            out = static_cast<std::uint8_t>(symbol);
        } else if (left != 0) {
            // at exactly 2^24 the lane neither reads nor decodes again
            x = state;
        } else {
            // the fragment ends; a byte read in this step does not reach the carried state
            phase = 0;
            need = !latch;
            if (reset)
                x = 0;
        }
    }
};

// `per` symbols per block on every lane into `records`, input in the schedule's order: each
// round the lanes step in order and take at most 8 bytes between them
template <class Fetch>
bool lanes(const Tables &t, std::span<const std::uint8_t> widths, std::size_t per,
           std::size_t blocks, Fetch fetch, std::uint8_t *records) {
    std::array<Lane, 16> all{};
    const auto count = widths.size();
    for (std::size_t j = 0; j < count; ++j) {
        all[j].width = widths[j];
        all[j].need = true;
    }
    const std::size_t stride = count * per;
    const std::uint32_t full = (1u << count) - 1;
    std::size_t pos = 0, at = 0;
    std::uint32_t done = 0;
    int idle = 0;
    while (pos < blocks * per) {
        unsigned taken = 0;
        bool moved = false;
        for (std::size_t j = 0; j < count; ++j) {
            auto &lane = all[j];
            const std::uint32_t bit = 1u << j;
            const auto before = taken;
            if (!lane.stalled)
                lane.step(t);
            const bool waiting = lane.out && (done & bit);
            if (lane.need && !waiting && taken < round_bytes)
                if (const auto byte = fetch(j)) {
                    lane.latch = *byte;
                    ++taken;
                }
            lane.stalled = (lane.need && before >= round_bytes) || waiting;
            if (!lane.stalled && lane.out) {
                records[at + j * per] = *lane.out;
                lane.out.reset();
                done |= bit;
                moved = true;
            }
        }
        if (done == full) {
            ++pos;
            at += pos % per == 0 ? stride - per + 1 : 1;
            done = 0;
            moved = true;
        }
        // nothing read and nothing delivered for a few rounds: no lane can go on
        idle = taken > 0 || moved ? 0 : idle + 1;
        if (idle > 3)
            return false;
    }
    return true;
}

// Blocks.

// two 4-bit symbols, the first on the even bits
std::uint8_t interleave(std::uint8_t a, std::uint8_t b) {
    const auto spread = [](std::uint8_t v) {
        std::uint8_t r = 0;
        for (int k = 0; k < 4; ++k)
            r |= static_cast<std::uint8_t>((v >> k & 1) << (2 * k));
        return r;
    };
    return static_cast<std::uint8_t>(spread(a) | spread(b) << 1);
}
// the record's endpoint and index references
std::pair<std::size_t, std::size_t> references(const std::uint8_t *s, bool paired) {
    const std::uint8_t d = paired ? interleave(s[0], s[1]) : s[0];
    const std::uint8_t ix = paired ? interleave(s[2], s[3]) : s[1];
    const std::uint8_t delta = static_cast<std::uint8_t>((d >> 1) ^ (d & 1 ? 0xff : 0));
    return {static_cast<std::uint8_t>(ix + delta), ix};
}
// block `i`'s reference `r`: 0 none, else `r` blocks back, before the first block into the
// dictionary
template <class T>
const T &pick(std::size_t i, std::size_t r, const std::vector<T> &history,
              const std::vector<T> &dictionary, const T &zero) {
    if (r == 0)
        return zero;
    if (r <= i)
        return history[(i - r) % history_size];
    return dictionary[r - i - 1];
}
// zigzag residual (0, -1, 1, -2, ...) modulo 2^bits
std::uint8_t zz(std::uint8_t v, unsigned bits) {
    const auto mask = static_cast<std::uint8_t>((1u << bits) - 1);
    return static_cast<std::uint8_t>(((v >> 1) ^ (v & 1 ? mask : 0)) & mask);
}

struct Widths {
    std::array<std::uint8_t, 8> ep;
    std::array<std::uint8_t, 32> ix;
    bool paired;
};
constexpr Widths widths_of(std::array<std::uint8_t, 8> ep, std::size_t ix3, std::size_t ix2,
                           bool paired) {
    std::array<std::uint8_t, 32> ix{};
    for (std::size_t k = 0; k < ix3 + ix2; ++k)
        ix[k] = k < ix3 ? 3 : 2;
    return {ep, ix, paired};
}
constexpr Widths bc1_widths = widths_of({5, 6, 5, 5, 6, 5, 0, 0}, 0, 16, false);
constexpr Widths bc3_widths = widths_of({5, 6, 5, 5, 6, 5, 8, 8}, 16, 16, true);
constexpr Widths bc4_widths = widths_of({8, 8, 0, 0, 0, 0, 0, 0}, 16, 0, false);
constexpr Widths bc5_widths = widths_of({8, 8, 8, 8, 0, 0, 0, 0}, 32, 0, true);

// 2-bit index residuals: one symbol per 2x2 quad
void quads(const std::array<std::uint8_t, 256> &rank, const std::uint8_t *s, std::size_t n,
           std::uint8_t *ix) {
    for (std::size_t q = 0; q < n; ++q) {
        const auto v = rank[s[q]];
        const auto base = (q >> 1) * 8 + (q & 1) * 2;
        constexpr std::size_t places[] = {0, 1, 4, 5};
        for (std::size_t k = 0; k < 4; ++k)
            ix[base + places[k]] = v >> (2 * k) & 3;
    }
}
// 3-bit index residuals: one symbol per two pixels in raster order
void pairs(const std::array<std::uint8_t, 64> &rank, const std::uint8_t *s, std::size_t n,
           std::uint8_t *ix) {
    for (std::size_t k = 0; k < n; ++k) {
        const auto v = rank[s[k] & 63];
        ix[2 * k] = v & 7;
        ix[2 * k + 1] = v >> 4 & 7;
    }
}
Plain bc1_residual(const Tables &t, const std::uint8_t *s) {
    Plain v;
    constexpr unsigned bits[] = {5, 6, 5, 5, 6, 5};
    for (std::size_t k = 0; k < 6; ++k)
        v.ep[k] = zz(s[2 + k], bits[k]);
    quads(t.rank1, s + 8, 4, v.ix.data());
    return v;
}
Plain bc3_residual(const Tables &t, const std::uint8_t *s) {
    // colour lanes carry each channel's two endpoints together
    Plain v;
    v.ep = {zz(s[14], 5), zz(s[16], 6), zz(s[18], 5), zz(s[15], 5),
            zz(s[17], 6), zz(s[19], 5), zz(s[4], 8),  zz(s[5], 8)};
    pairs(t.rank3a, s + 6, 8, v.ix.data());
    quads(t.rank3c, s + 20, 4, v.ix.data() + 16);
    return v;
}
Plain bc4_residual(const Tables &t, const std::uint8_t *s) {
    Plain v;
    v.ep[0] = zz(s[2], 8);
    v.ep[1] = zz(s[3], 8);
    pairs(t.rank4, s + 4, 8, v.ix.data());
    return v;
}
Plain bc5_residual(const Tables &t, const std::uint8_t *s) {
    Plain v;
    for (std::size_t k = 0; k < 4; ++k)
        v.ep[k] = zz(s[4 + k], 8);
    pairs(t.rank5, s + 8, 16, v.ix.data());
    return v;
}

// indices along the endpoint line -> BC1 and BC4 codes
constexpr std::uint32_t bc1_code[4] = {0, 2, 3, 1};
constexpr std::uint64_t bc4_code[8] = {0, 2, 3, 4, 5, 6, 7, 1};
void put_le(std::uint8_t *out, std::uint64_t value, std::size_t bytes) {
    for (std::size_t k = 0; k < bytes; ++k)
        out[k] = static_cast<std::uint8_t>(value >> (8 * k));
}
void bc1_color(const std::uint8_t *ep, const std::uint8_t *ix, std::uint8_t *out) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(ep[0] << 11 | ep[1] << 5 | ep[2]);
    const std::uint16_t c1 = static_cast<std::uint16_t>(ep[3] << 11 | ep[4] << 5 | ep[5]);
    std::uint32_t bits = 0;
    for (std::size_t p = 0; p < 16; ++p)
        bits |= bc1_code[ix[p] & 3] << (2 * p);
    put_le(out, c0, 2);
    put_le(out + 2, c1, 2);
    put_le(out + 4, bits, 4);
}
// BC4 half: endpoints are kept 128 below their stored value
void bc4_alpha(std::uint8_t e0, std::uint8_t e1, const std::uint8_t *ix, std::uint8_t *out) {
    std::uint64_t bits = 0;
    for (std::size_t p = 0; p < 16; ++p)
        bits |= bc4_code[ix[p] & 7] << (16 + 3 * p);
    put_le(out,
           std::uint64_t{static_cast<std::uint8_t>(e0 ^ 0x80)} |
               std::uint64_t{static_cast<std::uint8_t>(e1 ^ 0x80)} << 8 | bits,
           8);
}
void bc1_pack(const Plain &v, std::uint8_t *out) { bc1_color(v.ep.data(), v.ix.data(), out); }
void bc3_pack(const Plain &v, std::uint8_t *out) {
    bc4_alpha(v.ep[6], v.ep[7], v.ix.data(), out);
    bc1_color(v.ep.data(), v.ix.data() + 16, out + 8);
}
void bc4_pack(const Plain &v, std::uint8_t *out) { bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out); }
void bc5_pack(const Plain &v, std::uint8_t *out) {
    bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out);
    bc4_alpha(v.ep[2], v.ep[3], v.ix.data() + 16, out + 8);
}

void plain(const std::uint8_t *records, std::size_t stride, std::size_t blocks, std::uint8_t *out,
           std::size_t block_bytes, const std::vector<Plain> &dictionary, const Widths &w,
           Plain (*residual)(const Tables &, const std::uint8_t *),
           void (*pack)(const Plain &, std::uint8_t *), const Tables &t) {
    std::vector<Plain> history(history_size);
    const Plain zero{};
    for (std::size_t i = 0; i < blocks; ++i) {
        const auto *s = records + i * stride;
        const auto [ep_ref, ix_ref] = references(s, w.paired);
        const auto &epr = pick(i, ep_ref, history, dictionary, zero).ep;
        const auto &ixr = pick(i, ix_ref, history, dictionary, zero).ix;
        auto v = residual(t, s);
        for (std::size_t k = 0; k < 8; ++k)
            v.ep[k] = static_cast<std::uint8_t>((v.ep[k] + epr[k]) & ((1u << w.ep[k]) - 1));
        for (std::size_t k = 0; k < 32; ++k)
            v.ix[k] = static_cast<std::uint8_t>((v.ix[k] + ixr[k]) & ((1u << w.ix[k]) - 1));
        history[i % history_size] = v;
        pack(v, out + i * block_bytes);
    }
}

// a byte's even and odd bits as two zigzag-coded 4-bit residuals
std::array<std::int16_t, 2> nibbles(std::uint8_t b, bool odd_first) {
    const auto half = [](std::uint8_t v) {
        std::uint8_t r = 0;
        for (int k = 0; k < 4; ++k)
            r |= static_cast<std::uint8_t>((v >> (2 * k) & 1) << k);
        return r;
    };
    const auto z = [](std::uint8_t u) {
        return static_cast<std::int16_t>((u >> 1) ^ -static_cast<std::int16_t>(u & 1));
    };
    const auto even = z(half(b)), odd = z(half(static_cast<std::uint8_t>(b >> 1)));
    return odd_first ? std::array<std::int16_t, 2>{odd, even}
                     : std::array<std::int16_t, 2>{even, odd};
}
// index residual plus reference, both left-aligned to 16 bits at their own widths, back to
// `bits` bits (signed)
std::int16_t add_index(std::int16_t current, std::uint8_t bits, std::int16_t reference,
                       std::uint8_t reference_bits) {
    const auto up = [](std::int16_t v, std::uint8_t b) -> std::uint16_t {
        return b == 0 || b > 16
                   ? 0
                   : static_cast<std::uint16_t>(static_cast<std::uint16_t>(v) << (16 - b));
    };
    const auto sum = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(up(current, bits) + up(reference, reference_bits)));
    return bits == 0 ? 0 : static_cast<std::int16_t>(sum >> (16 - bits));
}
// little-endian bit writer for one 128-bit block
struct Bits {
    std::uint64_t lo = 0, hi = 0;
    unsigned at = 0;
    void put(std::uint64_t value, unsigned n) {
        if (n > 0 && at < 128) {
            const std::uint64_t masked = n >= 64 ? value : value & ((1ull << n) - 1);
            if (at < 64) {
                lo |= masked << at;
                if (at + n > 64 && at > 0)
                    hi |= masked >> (64 - at);
            } else {
                hi |= masked << (at - 64);
            }
        }
        at += n;
    }
    void write(std::uint8_t *out) const {
        put_le(out, lo, 8);
        put_le(out + 8, hi, 8);
    }
};

// BC7.
constexpr std::uint8_t anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2, 2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8, 2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15};
constexpr std::uint8_t anchor3a[64] = {3, 3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                                       3, 3,  8,  15, 3, 3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                                       8, 15, 3,  5,  6, 10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                       3, 15, 5,  5,  5, 8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
constexpr std::uint8_t anchor3b[64] = {
    15, 8, 8, 3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8, 3, 15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6, 6, 8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
// endpoint bits of R G B A per mode, before p-bits and rotation
constexpr std::array<std::uint8_t, 4> bc7_bits[8] = {{4, 4, 4, 0}, {6, 6, 6, 0}, {5, 5, 5, 0},
                                                     {7, 7, 7, 0}, {5, 5, 5, 6}, {7, 7, 7, 8},
                                                     {7, 7, 7, 7}, {5, 5, 5, 5}};
// index bits of the primary and secondary sets per mode
constexpr std::array<std::uint8_t, 2> bc7_index[8] = {{3, 0}, {3, 0}, {2, 0}, {2, 0},
                                                      {3, 2}, {2, 2}, {4, 0}, {2, 0}};
bool pbits(unsigned mode) { return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7; }
// channel precisions: the p-bit is red's lowest bit; rotation swaps alpha with the rotated
// channel
std::array<std::uint8_t, 4> bc7_precision(std::uint16_t ctrl) {
    const unsigned mode = ctrl & 7, rot = ctrl >> 3 & 3;
    auto p = bc7_bits[mode];
    if (rot != 0)
        std::swap(p[3], p[rot - 1]);
    else if (pbits(mode))
        ++p[0];
    return p;
}
void bc7_pack(const Bc7Ep &ep, const Bc7Ix &ix, const std::array<std::uint8_t, 4> &prec,
              std::uint8_t *out) {
    const unsigned ctrl = ep.ctrl, mode = ctrl & 7, subsets = (ctrl & 0xff) >> 6,
                   part = ctrl >> 8 & 63, rot = ctrl >> 3 & 3;
    Bits w;
    w.put(1ull << mode, mode + 1);
    switch (mode) {
    case 0:
        w.put(part, 4);
        break;
    case 1:
    case 2:
    case 3:
    case 7:
        w.put(part, 6);
        break;
    case 4:
        w.put(rot, 2);
        w.put(ctrl >> 5 & 1, 1);
        break;
    case 5:
        w.put(rot, 2);
        break;
    default:
        break;
    }
    const unsigned ends = 2 * subsets;
    const auto field = [&](unsigned row, unsigned col, unsigned p) -> std::uint64_t {
        return ep.rows[row][col] >> (16 - p);
    };
    std::array<unsigned, 4> order = {0, 1, 2, 3};
    if (rot != 0)
        std::swap(order[3], order[rot - 1]);
    for (const auto ch : order) {
        const unsigned p = prec[ch];
        if (p == 0)
            continue;
        const unsigned drop = ch == 0 && pbits(mode);
        for (unsigned e = 0; e < ends; ++e)
            w.put(field(e / 2, 2 * ch + e % 2, p) >> drop, p - drop);
    }
    if (mode == 1) {
        for (unsigned row = 0; row < subsets; ++row)
            w.put(field(row, 0, prec[0]) & 1, 1);
    } else if (pbits(mode)) {
        for (unsigned e = 0; e < ends; ++e)
            w.put(field(e / 2, e % 2, prec[0]) & 1, 1);
    }
    std::array<std::uint8_t, 3> anchors{0, 255, 255};
    if (subsets == 2)
        anchors = {0, anchor2[part], 255};
    else if (subsets == 3)
        anchors = {0, anchor3a[part], anchor3b[part]};
    const auto indices = [&](unsigned set) {
        const unsigned bits = ix.bits[set];
        for (unsigned p = 0; p < 16; ++p) {
            const bool anchor = std::find(anchors.begin(), anchors.end(), p) != anchors.end();
            w.put(static_cast<std::uint16_t>(ix.ix[set][p]), bits - anchor);
        }
    };
    if (mode == 4) {
        indices(1);
        indices(0);
    } else if (mode == 5) {
        indices(0);
        indices(1);
    } else {
        indices(0);
    }
    w.write(out);
}
bool bc7(const std::uint8_t *records, std::size_t stride, std::size_t blocks, std::uint8_t *out,
         const Tables &t, std::string *why) {
    std::vector<std::pair<Bc7Ep, Bc7Ix>> history(history_size);
    const std::pair<Bc7Ep, Bc7Ix> zero{};
    for (std::size_t i = 0; i < blocks; ++i) {
        const auto *s = records + i * stride;
        const auto [ep_ref, ix_ref] = references(s, true);
        const std::uint16_t ctrl = t.ctrl7[(s[31] & 15u) << 5 | (s[30] & 31u)];
        if (ctrl == 0) {
            if (why)
                *why = "unused BC7 control code";
            return false;
        }
        const unsigned mode = ctrl & 7, subsets = (ctrl & 0xff) >> 6;
        const auto prec = bc7_precision(ctrl);
        const bool three = subsets == 3;
        const auto or0 = [&](std::uint8_t v) -> std::uint8_t { return three ? 0 : v; };
        const auto if3 = [&](std::uint8_t v) -> std::uint8_t { return three ? v : 0; };
        const std::uint8_t stored[3][8] = {
            {s[4], s[5], s[6], s[7], s[8], s[9], or0(s[10]), or0(s[11])},
            {s[12], s[13], s[14], s[15], s[16], s[17], or0(s[18]), or0(s[19])},
            {s[20], s[21], if3(s[10]), if3(s[11]), if3(s[18]), if3(s[19]), 0, 0}};
        const auto &r = pick(i, ep_ref, history, t.bc7, zero).first;
        Bc7Ep ep;
        ep.ctrl = ctrl;
        // rows the reference lacks build on its first row
        const unsigned common = std::min<unsigned>(subsets, (r.ctrl & 0xff) >> 6);
        const unsigned columns = prec[3] > 0 ? 8 : 6;
        for (unsigned c = 0; c < columns; ++c) {
            const unsigned p = prec[c / 2];
            if (p == 0)
                continue;
            const auto mask = static_cast<std::uint16_t>(0xffffu << (16 - p));
            for (unsigned row = 0; row < subsets && row < 3; ++row) {
                const std::uint8_t v = stored[row][c];
                const auto residual = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>((v >> 1) ^ (v & 1 ? 0xffffu : 0u)) << (16 - p));
                const auto base =
                    static_cast<std::uint16_t>(r.rows[row < common ? row : 0][c] & mask);
                ep.rows[row][c] = static_cast<std::uint16_t>(base + residual);
            }
        }
        const auto bits = bc7_index[mode];
        const auto &rx = pick(i, ix_ref, history, t.bc7, zero).second;
        Bc7Ix ix;
        ix.bits = bits;
        constexpr std::size_t offsets[2][4] = {{22, 24, 26, 28}, {12, 14, 16, 20}};
        for (std::size_t set = 0; set < 2; ++set) {
            if (bits[set] == 0)
                continue;
            for (std::size_t k = 0; k < 4; ++k)
                for (std::size_t b = 0; b < 2; ++b) {
                    const auto pair = nibbles(s[offsets[set][k] + b], k < 3);
                    for (std::size_t h = 0; h < 2; ++h) {
                        const auto p = 4 * k + 2 * b + h;
                        ix.ix[set][p] = add_index(pair[h], bits[set], rx.ix[set][p], rx.bits[set]);
                    }
                }
        }
        history[i % history_size] = {ep, ix};
        bc7_pack(ep, ix, prec, out + i * 16);
    }
    return true;
}

// BC6H.
// bits of endpoint 0, then of the other endpoints' R, G, B, per mode 1-14
constexpr std::uint8_t bc6_bits[15][4] = {
    {0, 0, 0, 0},  {10, 5, 5, 5},    {7, 6, 6, 6},  {11, 5, 4, 4}, {11, 4, 5, 4},
    {11, 4, 4, 5}, {9, 5, 5, 5},     {8, 6, 5, 5},  {8, 5, 6, 5},  {8, 5, 5, 6},
    {6, 6, 6, 6},  {10, 10, 10, 10}, {11, 9, 9, 9}, {12, 8, 8, 8}, {16, 4, 4, 4}};
// mode code and its width, modes 1-14
constexpr std::pair<std::uint64_t, unsigned> bc6_code[15] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 5}, {6, 5}, {10, 5}, {14, 5}, {18, 5},
    {22, 5}, {26, 5}, {30, 5}, {3, 5}, {7, 5}, {11, 5}, {15, 5}};
// a run of the public BC6H layout: `len` bits of channel `ch` (0-2; 3 is the partition) of
// endpoint `ep`, from bit `first` up
struct Run {
    std::uint8_t ch, ep, first, len;
};
constexpr std::uint8_t R = 0, G = 1, B = 2;
constexpr Run D{3, 0, 0, 5};
// the header after the mode bits, per mode 1-14; modes 13 and 14 write the high base bits
// in ascending order (what the console's packer does)
constexpr Run mode1[] = {{G, 2, 4, 1},  {B, 2, 4, 1}, {B, 3, 4, 1}, {R, 0, 0, 10}, {G, 0, 0, 10},
                         {B, 0, 0, 10}, {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4},  {G, 1, 0, 5},
                         {B, 3, 0, 1},  {G, 3, 0, 4}, {B, 1, 0, 5}, {B, 3, 1, 1},  {B, 2, 0, 4},
                         {R, 2, 0, 5},  {B, 3, 2, 1}, {R, 3, 0, 5}, {B, 3, 3, 1},  D};
constexpr Run mode2[] = {
    {G, 2, 5, 1}, {G, 3, 4, 1}, {G, 3, 5, 1}, {R, 0, 0, 7}, {B, 3, 0, 1}, {B, 3, 1, 1},
    {B, 2, 4, 1}, {G, 0, 0, 7}, {B, 2, 5, 1}, {B, 3, 2, 1}, {G, 2, 4, 1}, {B, 0, 0, 7},
    {B, 3, 3, 1}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 6},
    {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 6}, {R, 3, 0, 6}, D};
constexpr Run mode3[] = {{R, 0, 0, 10},
                         {G, 0, 0, 10},
                         {B, 0, 0, 10},
                         {R, 1, 0, 5},
                         {R, 0, 10, 1},
                         {G, 2, 0, 4},
                         {G, 1, 0, 4},
                         {G, 0, 10, 1},
                         {B, 3, 0, 1},
                         {G, 3, 0, 4},
                         {B, 1, 0, 4},
                         {B, 0, 10, 1},
                         {B, 3, 1, 1},
                         {B, 2, 0, 4},
                         {R, 2, 0, 5},
                         {B, 3, 2, 1},
                         {R, 3, 0, 5},
                         {B, 3, 3, 1},
                         D};
constexpr Run mode4[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10},
                         {R, 1, 0, 4},  {R, 0, 10, 1}, {G, 3, 4, 1},
                         {G, 2, 0, 4},  {G, 1, 0, 5},  {G, 0, 10, 1},
                         {G, 3, 0, 4},  {B, 1, 0, 4},  {B, 0, 10, 1},
                         {B, 3, 1, 1},  {B, 2, 0, 4},  {R, 2, 0, 4},
                         {B, 3, 0, 1},  {B, 3, 2, 1},  {R, 3, 0, 4},
                         {G, 2, 4, 1},  {B, 3, 3, 1},  D};
constexpr Run mode5[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10},
                         {R, 1, 0, 4},  {R, 0, 10, 1}, {B, 2, 4, 1},
                         {G, 2, 0, 4},  {G, 1, 0, 4},  {G, 0, 10, 1},
                         {B, 3, 0, 1},  {G, 3, 0, 4},  {B, 1, 0, 5},
                         {B, 0, 10, 1}, {B, 2, 0, 4},  {R, 2, 0, 4},
                         {B, 3, 1, 1},  {B, 3, 2, 1},  {R, 3, 0, 4},
                         {B, 3, 4, 1},  {B, 3, 3, 1},  D};
constexpr Run mode6[] = {{R, 0, 0, 9}, {B, 2, 4, 1}, {G, 0, 0, 9}, {G, 2, 4, 1}, {B, 0, 0, 9},
                         {B, 3, 4, 1}, {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4}, {G, 1, 0, 5},
                         {B, 3, 0, 1}, {G, 3, 0, 4}, {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4},
                         {R, 2, 0, 5}, {B, 3, 2, 1}, {R, 3, 0, 5}, {B, 3, 3, 1}, D};
constexpr Run mode7[] = {{R, 0, 0, 8}, {G, 3, 4, 1}, {B, 2, 4, 1}, {G, 0, 0, 8}, {B, 3, 2, 1},
                         {G, 2, 4, 1}, {B, 0, 0, 8}, {B, 3, 3, 1}, {B, 3, 4, 1}, {R, 1, 0, 6},
                         {G, 2, 0, 4}, {G, 1, 0, 5}, {B, 3, 0, 1}, {G, 3, 0, 4}, {B, 1, 0, 5},
                         {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 6}, {R, 3, 0, 6}, D};
constexpr Run mode8[] = {{R, 0, 0, 8}, {B, 3, 0, 1},
                         {B, 2, 4, 1}, {G, 0, 0, 8},
                         {G, 2, 5, 1}, {G, 2, 4, 1},
                         {B, 0, 0, 8}, {G, 3, 5, 1},
                         {B, 3, 4, 1}, {R, 1, 0, 5},
                         {G, 3, 4, 1}, {G, 2, 0, 4},
                         {G, 1, 0, 6}, {G, 3, 0, 4},
                         {B, 1, 0, 5}, {B, 3, 1, 1},
                         {B, 2, 0, 4}, {R, 2, 0, 5},
                         {B, 3, 2, 1}, {R, 3, 0, 5},
                         {B, 3, 3, 1}, D};
constexpr Run mode9[] = {{R, 0, 0, 8}, {B, 3, 1, 1},
                         {B, 2, 4, 1}, {G, 0, 0, 8},
                         {B, 2, 5, 1}, {G, 2, 4, 1},
                         {B, 0, 0, 8}, {B, 3, 5, 1},
                         {B, 3, 4, 1}, {R, 1, 0, 5},
                         {G, 3, 4, 1}, {G, 2, 0, 4},
                         {G, 1, 0, 5}, {B, 3, 0, 1},
                         {G, 3, 0, 4}, {B, 1, 0, 6},
                         {B, 2, 0, 4}, {R, 2, 0, 5},
                         {B, 3, 2, 1}, {R, 3, 0, 5},
                         {B, 3, 3, 1}, D};
constexpr Run mode10[] = {
    {R, 0, 0, 6}, {G, 3, 4, 1}, {B, 3, 0, 1}, {B, 3, 1, 1}, {B, 2, 4, 1}, {G, 0, 0, 6},
    {G, 2, 5, 1}, {B, 2, 5, 1}, {B, 3, 2, 1}, {G, 2, 4, 1}, {B, 0, 0, 6}, {G, 3, 5, 1},
    {B, 3, 3, 1}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 6},
    {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 6}, {R, 3, 0, 6}, D};
constexpr Run mode11[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10},
                          {R, 1, 0, 10}, {G, 1, 0, 10}, {B, 1, 0, 10}};
constexpr Run mode12[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 9}, {R, 0, 10, 1},
                          {G, 1, 0, 9},  {G, 0, 10, 1}, {B, 1, 0, 9},  {B, 0, 10, 1}};
constexpr Run mode13[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 8}, {R, 0, 10, 2},
                          {G, 1, 0, 8},  {G, 0, 10, 2}, {B, 1, 0, 8},  {B, 0, 10, 2}};
constexpr Run mode14[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 4}, {R, 0, 10, 6},
                          {G, 1, 0, 4},  {G, 0, 10, 6}, {B, 1, 0, 4},  {B, 0, 10, 6}};
const std::span<const Run> bc6_layout[15] = {{},     mode1,  mode2,  mode3,  mode4,
                                             mode5,  mode6,  mode7,  mode8,  mode9,
                                             mode10, mode11, mode12, mode13, mode14};
std::uint8_t bc6_index_bits(std::uint8_t mode) {
    return mode >= 1 && mode <= 10 ? 3 : mode >= 11 && mode <= 14 ? 4 : 0;
}
bool bc6(const std::uint8_t *records, std::size_t stride, std::size_t blocks, std::uint8_t *out,
         const std::vector<std::pair<Bc6Ep, Bc6Ix>> &dictionary, std::string *why) {
    std::vector<std::pair<Bc6Ep, Bc6Ix>> history(history_size);
    std::pair<Bc6Ep, Bc6Ix> zero{};
    zero.second.mode = 11;
    const auto z = [](std::uint16_t v) {
        return static_cast<std::uint16_t>((v >> 1) ^ (v & 1 ? 0xffffu : 0u));
    };
    const auto wide = [](std::uint8_t a, std::uint8_t b) {
        std::uint16_t r = 0;
        for (int k = 0; k < 8; ++k)
            r |= static_cast<std::uint16_t>((a >> k & 1) << (2 * k) | (b >> k & 1) << (2 * k + 1));
        return r;
    };
    for (std::size_t i = 0; i < blocks; ++i) {
        const auto *s = records + i * stride;
        const auto [ep_ref, ix_ref] = references(s, true);
        const auto mode = static_cast<std::uint8_t>(s[30] + 1);
        if (mode < 1 || mode > 14) {
            if (why)
                *why = "BC6H mode out of range";
            return false;
        }
        const bool two = mode < 11;
        unsigned shift[4];
        for (int k = 0; k < 4; ++k)
            shift[k] = 16 - bc6_bits[mode][k];
        const std::array<std::uint16_t, 3> e0 = {z(wide(s[4], s[5])), z(wide(s[6], s[7])),
                                                 z(wide(s[8], s[9]))};
        const std::array<std::uint16_t, 3> e1 = {z(wide(s[10], s[11])), z(wide(s[12], s[13])),
                                                 z(wide(s[14], s[15]))};
        std::array<std::uint16_t, 3> e2 = e0, e3 = e1;
        if (two) {
            e2 = {z(s[16]), z(s[18]), z(s[20])};
            e3 = {z(s[17]), z(s[19]), z(s[21])};
        }
        const auto &r = pick(i, ep_ref, history, dictionary, zero).first;
        Bc6Ep ep;
        // endpoint 0 at the base precision, the others per channel; the reference is cut by
        // the mask after the sum
        const auto mask = [](unsigned sh) { return static_cast<std::uint16_t>(0xffffu << sh); };
        for (int c = 0; c < 3; ++c)
            ep.e[0][c] = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(r.e[0][c] +
                                           static_cast<std::uint16_t>(e0[c] << shift[0])) &
                mask(shift[0]));
        const std::array<std::uint16_t, 3> *rest[3] = {&e1, &e2, &e3};
        for (int k = 0; k < (two ? 3 : 1); ++k)
            for (int c = 0; c < 3; ++c)
                ep.e[k + 1][c] = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(
                        r.e[k + 1][c] + static_cast<std::uint16_t>((*rest[k])[c] << shift[1 + c])) &
                    mask(shift[1 + c]));
        if (!two) {
            for (int c = 0; c < 3; ++c)
                ep.e[2][c] = static_cast<std::uint16_t>((ep.e[0][c] >> shift[0]) << shift[1]);
            ep.e[3] = ep.e[1];
        }
        const auto &rx = pick(i, ix_ref, history, dictionary, zero).second;
        const auto bits = bc6_index_bits(mode);
        Bc6Ix ix;
        ix.mode = mode;
        for (int k = 0; k < 8; ++k) {
            const auto pair = nibbles(s[22 + k], k < 6);
            for (int h = 0; h < 2; ++h) {
                const auto p = 2 * k + h;
                ix.ix[p] = add_index(pair[h], bits, rx.ix[p], bc6_index_bits(rx.mode));
            }
        }
        history[i % history_size] = {ep, ix};
        // fields: the working value has its top bit flipped
        const auto field = [&](unsigned k, unsigned c) -> std::uint64_t {
            const auto sh = k == 0 ? shift[0] : shift[1 + c];
            return static_cast<std::uint16_t>(ep.e[k][c] ^ 0x8000) >> sh;
        };
        Bits w;
        w.put(bc6_code[mode].first, bc6_code[mode].second);
        const unsigned part = s[31] & 31;
        for (const auto &run : bc6_layout[mode]) {
            if (run.ch == 3)
                w.put(part, 5);
            else
                w.put(field(run.ep, run.ch) >> run.first, run.len);
        }
        const unsigned anchor = two ? anchor2[part] : 0;
        for (unsigned p = 0; p < 16; ++p) {
            const bool is_short = p == 0 || p == anchor;
            const auto v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(ix.ix[p]) +
                                                      (1u << (bits - 1)));
            w.put(v, bits - is_short);
        }
        w.write(out + i * 16);
    }
    return true;
}

// each format's lane widths, symbols per lane and block, and block size
struct Layout {
    std::span<const std::uint8_t> widths;
    std::size_t per, block;
};
std::optional<Layout> layout(unsigned bc) {
    static constexpr std::uint8_t w1[] = {7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
    static constexpr std::uint8_t w3[] = {3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7};
    static constexpr std::uint8_t w4[] = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
    static constexpr std::uint8_t w5[] = {3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
    static constexpr std::uint8_t w6[] = {3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4};
    static constexpr std::uint8_t w7[] = {3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};
    switch (bc) {
    case 1:
        return Layout{w1, 1, 8};
    case 3:
        return Layout{w3, 2, 16};
    case 4:
        return Layout{w4, 1, 8};
    case 5:
        return Layout{w5, 2, 16};
    case 6:
        return Layout{w6, 2, 16};
    case 7:
        return Layout{w7, 2, 16};
    default: // BC2: no title's data is known to use it, and its layout is not known here
        return std::nullopt;
    }
}
} // namespace

bool decode(const Tables &tables, unsigned bc, std::span<const std::uint8_t> source,
            std::span<std::uint8_t> destination, std::string *why) {
    const auto *const t = &tables;
    const auto fail = [&](const char *reason) {
        if (why)
            *why = reason;
        return false;
    };
    const auto format = layout(bc);
    if (!format)
        return fail("a BC format whose packing is not known");
    const auto [widths, per, block] = *format;
    if (destination.empty() || destination.size() % block)
        return fail("the destination is not a whole number of blocks");
    const auto blocks = destination.size() / block;
    const auto stride = widths.size() * per;
    std::vector<std::uint8_t> records(blocks * stride);
    std::size_t next = 0;
    const auto fetch = [&](std::size_t) -> std::optional<std::uint8_t> {
        if (next == source.size())
            return std::nullopt;
        return source[next++];
    };
    if (!lanes(*t, widths, per, blocks, fetch, records.data()))
        return fail("the stream ends early or is corrupt");
    auto *const out = destination.data();
    switch (bc) {
    case 1:
        plain(records.data(), stride, blocks, out, 8, t->bc1, bc1_widths, bc1_residual, bc1_pack,
              *t);
        return true;
    case 3:
        plain(records.data(), stride, blocks, out, 16, t->bc3, bc3_widths, bc3_residual, bc3_pack,
              *t);
        return true;
    case 4:
        plain(records.data(), stride, blocks, out, 8, t->bc4, bc4_widths, bc4_residual, bc4_pack,
              *t);
        return true;
    case 5:
        plain(records.data(), stride, blocks, out, 16, t->bc5, bc5_widths, bc5_residual, bc5_pack,
              *t);
        return true;
    case 6:
        return bc6(records.data(), stride, blocks, out, t->bc6, why);
    default:
        return bc7(records.data(), stride, blocks, out, *t, why);
    }
}

std::vector<std::uint8_t> interleave_lanes(const Tables &tables, unsigned bc, std::size_t blocks,
                                           std::vector<std::vector<std::uint8_t>> lanes_bytes) {
    std::vector<std::uint8_t> stream;
    const auto format = layout(bc);
    if (!format || lanes_bytes.size() != format->widths.size())
        return stream;
    std::vector<std::size_t> next(lanes_bytes.size());
    const auto fetch = [&](std::size_t lane) -> std::optional<std::uint8_t> {
        if (next[lane] == lanes_bytes[lane].size())
            return std::nullopt;
        stream.push_back(lanes_bytes[lane][next[lane]++]);
        return stream.back();
    };
    std::vector<std::uint8_t> records(blocks * format->widths.size() * format->per);
    if (!lanes(tables, format->widths, format->per, blocks, fetch, records.data()))
        stream.clear();
    return stream;
}
} // namespace xv::runtime::modules::bcpack
