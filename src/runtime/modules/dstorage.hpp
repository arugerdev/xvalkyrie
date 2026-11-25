#pragma once
#include <cstdint>
#include <span>

namespace xv::runtime {
class Registry;
namespace modules {
// DirectStorage for Series titles: dstorage_xs.dll and XCRDAPI.dll
void install_dstorage(Registry &registry);
// the tables BCPack requests are unpacked with, in place of the built-in ones; false (and
// BCPack requests fail) if they are not tables
bool use_bcpack_tables(std::span<const std::uint8_t> bytes);
} // namespace modules
} // namespace xv::runtime
