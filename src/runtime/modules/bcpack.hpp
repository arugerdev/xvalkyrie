#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// BCPack, the console's lossless packing of BC1 and BC3 to BC7 block streams, which its
// DirectStorage unpacks after zlib (DSTORAGE_BCPACK_MODE). Ported code: see THIRD-PARTY.md.
// The format's constant tables (entropy models, index ranks, BC7 control codes and
// dictionaries) are not part of this code: whoever decodes gives them.
namespace xv::runtime::modules::bcpack {
struct Tables;
// the tables' size, as bcpack-tables.bin lays them out
constexpr std::size_t tables_size = 77982;
// the tables from those bytes; null, with the reason in `why`, if they are not tables
std::shared_ptr<const Tables> parse_tables(std::span<const std::uint8_t> bytes, std::string *why);
// unpacks `source` (BC number `bc`) into `destination`, whose size gives the block count;
// false, with the reason in `why`, if it cannot
bool decode(const Tables &tables, unsigned bc, std::span<const std::uint8_t> source,
            std::span<std::uint8_t> destination, std::string *why);
// the bytes of each lane of a stream of `blocks` blocks, in the order the decoder takes them:
// the stream they make (empty if they do not make one), for tests that build streams
std::vector<std::uint8_t> interleave_lanes(const Tables &tables, unsigned bc, std::size_t blocks,
                                           std::vector<std::vector<std::uint8_t>> lanes);
} // namespace xv::runtime::modules::bcpack
