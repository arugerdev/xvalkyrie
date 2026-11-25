#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <d3d12.h>
#include <utility>
#include <vector>

// Placement resources: the title owns their memory and writes it directly, as the console's
// GPU reads it. Here the host's resource is a copy, created from that memory and brought up
// to date before draws and presents once the title has written it again: the memory is kept
// read-only, and the first write to it marks every resource in it dirty.
namespace xv::runtime::modules {
// `resource` lives at `address`: linear (tile mode 0 or 1, the layout our XG computes) or in
// a tiling the console's xg_x.dll copies out of, when the title's system folder has it
void track_placement(ID3D11Resource *resource, const void *address, UINT tile_mode = 0,
                     UINT pitch = 0);
// the data at `address` for a resource described by `desc`, if that memory can be read
bool placement_data(const D3D11_TEXTURE2D_DESC &desc, const void *address,
                    D3D11_SUBRESOURCE_DATA *data, UINT count);
void upload_placements(ID3D11DeviceContext *context);
// The same for a Direct3D 12 texture, in the tiling the console picks for it. Its uploads are
// recorded by whoever submits work: each dirty one's subresources, linear and tightly packed.
// `layout` is the description a Series title gave (with its swizzle mode), for the Series
// console's xg_xs.dll. `format` is the one the title's memory holds, where the host's
// resource has another for it.
void track_placement(ID3D12Resource *resource, const void *address,
                     const D3D12_RESOURCE_DESC *layout = nullptr,
                     DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);
struct PlacementUpload {
    ID3D12Resource *resource; // one reference, the taker's
    std::vector<std::vector<std::byte>> subresources;
    std::vector<UINT> pitches;
    DXGI_FORMAT format; // what the subresources hold, which may not be the resource's format
};
std::vector<PlacementUpload> take_d3d12_uploads();
// Every placement let go of, and the handler that watches their memory taken out: the next
// track_placement starts over. Exposed for the tests, each of which stands for a title.
void forget_placements();
// before the host writes [address, address + bytes) (ReadFile into it): a write there from
// the kernel fails instead of faulting, so the placements in it are let go and marked dirty
void prepare_title_write(const void *address, std::size_t bytes);
// The same for as long as it lives, for a write that takes a while (a file read): the memory
// is not guarded again, nor uploaded from, until the write is done, as a guard put back
// meanwhile would fail the rest of it.
class TitleWrite {
  public:
    TitleWrite(const void *address, std::size_t bytes);
    ~TitleWrite();
    TitleWrite(const TitleWrite &) = delete;
    TitleWrite &operator=(const TitleWrite &) = delete;

  private:
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges_;
};
// every page of [address, address + bytes) is committed and readable
bool readable_memory(std::uint64_t address, std::size_t bytes);
} // namespace xv::runtime::modules
