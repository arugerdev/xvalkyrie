#include "runtime/modules/acphal.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/memory.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <opus.h>
#include <utility>
#include <vector>
#include <windows.h>

// The audio processor without SHAPE: commands are acknowledged and audio frames ticked so
// the title's mixer runs, nothing is decoded. ABI as FMOD Ex calls it.
namespace xv::runtime::modules {
namespace {
std::atomic<bool> early_kit{false};
struct ApuRegion {
    std::uintptr_t address;
    std::uint32_t size, apu;
};
std::mutex apu_mutex;
std::vector<ApuRegion> apu_regions;
std::uint32_t next_apu = 0x10000000;
std::uint32_t apu_address(std::uintptr_t address, std::uint32_t size) {
    for (const auto &region : apu_regions)
        if (address - region.address < region.size)
            return region.apu + static_cast<std::uint32_t>(address - region.address);
    const auto apu = next_apu;
    next_apu += (std::max(size, 1u) + 0xfff) & ~0xfffu;
    apu_regions.push_back({address, size, apu});
    return apu;
}
void *apu_memory(std::size_t size, std::uint64_t alignment) {
    auto *const address = allocate_aligned(size, alignment, nullptr);
    if (address) {
        const std::lock_guard lock(apu_mutex);
        apu_address(reinterpret_cast<std::uintptr_t>(address), static_cast<std::uint32_t>(size));
    }
    return address;
}
void release_apu_memory(void *address) {
    {
        const std::lock_guard lock(apu_mutex);
        std::erase_if(apu_regions, [&](const ApuRegion &region) {
            return region.address == reinterpret_cast<std::uintptr_t>(address);
        });
    }
    VirtualFree(address, 0, MEM_RELEASE);
}
HRESULT WINAPI apu_alloc(void **address, std::uint32_t *apu, std::uint32_t size,
                         std::uint32_t alignment, std::uint32_t) {
    if (!address)
        return E_INVALIDARG;
    *address = apu_memory(size, alignment);
    if (!*address)
        return E_OUTOFMEMORY;
    if (apu) {
        const std::lock_guard lock(apu_mutex);
        *apu = apu_address(reinterpret_cast<std::uintptr_t>(*address), size);
    }
    return S_OK;
}
HRESULT WINAPI apu_free(void *address) {
    if (!address)
        return E_INVALIDARG;
    release_apu_memory(address);
    return S_OK;
}
HRESULT WINAPI apu_map_virtual_address(std::uint32_t *apu, void *address, std::uint32_t size) {
    if (!apu)
        return E_INVALIDARG;
    const std::lock_guard lock(apu_mutex);
    *apu = apu_address(reinterpret_cast<std::uintptr_t>(address), size);
    return S_OK;
}
// ApuMapApuAddress: the memory an address of the audio processor's stands for
void *WINAPI apu_map_apu_address(std::uint32_t apu) {
    const std::lock_guard lock(apu_mutex);
    for (const auto &region : apu_regions)
        if (apu - region.apu < region.size)
            return reinterpret_cast<void *>(region.address + (apu - region.apu));
    return nullptr;
}
BOOL WINAPI apu_is_virtual_address_valid() { return TRUE; }
// ApuHeapGetState: the heap is the host's memory, so there is always room. Free bytes, bytes
// in use, bytes lost, the largest block to be had, and the number of blocks.
HRESULT WINAPI apu_heap_state(std::uint32_t *state, std::uint32_t) {
    if (!state)
        return E_INVALIDARG;
    const std::lock_guard lock(apu_mutex);
    std::uint32_t used = 0;
    for (const auto &region : apu_regions)
        used += region.size;
    state[0] = 256u << 20;
    state[1] = used;
    state[2] = 0;
    state[3] = 64u << 20;
    state[4] = static_cast<std::uint32_t>(apu_regions.size());
    return S_OK;
}
// chad.dll (XDsp, the Series console's audio DSP): its interface is in no public header,
// so a title that asks with XDspConnect is told it is not there
HRESULT WINAPI no_audio_dsp() { return E_NOTIMPL; }
// XDspConnectWithMaximumStreamLimit, as a title calls it: three values of its own, the
// number of streams, a flag, and where the connection goes, which it hands to the other
// four functions as their first argument. A title that is refused stops at a breakpoint,
// so it gets a connection that takes its commands and runs none of them.
struct DspConnection {
    UINT32 streams;
};
HRESULT WINAPI xdsp_connect_with_limit(void *, UINT32, UINT32, UINT32 streams, UINT32,
                                       DspConnection **connection) {
    if (!connection)
        return E_INVALIDARG;
    *connection = new DspConnection{streams};
    std::fprintf(stderr,
                 "[Runtime] audio DSP: connected for %u streams; its commands are taken, "
                 "none is run\n",
                 streams);
    std::fflush(stderr);
    return S_OK;
}
HRESULT WINAPI xdsp_disconnect(DspConnection *connection) {
    if (!connection)
        return E_NOTIMPL;
    delete connection;
    return S_OK;
}
// XDspActivate, XDspDeactivate and XDspSubmitCommand: without a connection, as before
HRESULT WINAPI xdsp_command(DspConnection *connection) { return connection ? S_OK : E_NOTIMPL; }
// IAudioStateMonitor, which says how loud the system lets the title be: always in full. A
// title takes the object without looking at the result of asking for it.
struct StateMonitor {
    const void *const *vtable;
    std::atomic<ULONG> references{1};
};
HRESULT STDMETHODCALLTYPE monitor_query(StateMonitor *self, const GUID *, void **object) {
    if (!object)
        return E_POINTER;
    ++self->references;
    *object = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE monitor_add_ref(StateMonitor *self) { return ++self->references; }
ULONG STDMETHODCALLTYPE monitor_release(StateMonitor *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
HRESULT STDMETHODCALLTYPE monitor_register(StateMonitor *, void *, void *, std::int64_t *token) {
    if (token)
        *token = 1;
    return S_OK;
}
void STDMETHODCALLTYPE monitor_unregister(StateMonitor *, std::int64_t) {}
// SoundLevel: 0 muted, 1 low, 2 full
int STDMETHODCALLTYPE monitor_sound_level(StateMonitor *) { return 2; }
const void *const monitor_vtable[] = {&monitor_query,    &monitor_add_ref,    &monitor_release,
                                      &monitor_register, &monitor_unregister, &monitor_sound_level};
HRESULT WINAPI create_state_monitor(int, int, StateMonitor **monitor) {
    if (!monitor)
        return E_POINTER;
    *monitor = new StateMonitor{monitor_vtable};
    return S_OK;
}
HRESULT WINAPI apu_create_heap() { return S_OK; }

// 0x60 bytes; the rest is unknown
struct ShapeContexts {
    std::uint32_t counts[5], reserved;
    void *contexts[5];
};
static_assert(sizeof(ShapeContexts) == 0x40);
constexpr std::size_t context_bytes = 64; // the largest entry seen
std::mutex shape_mutex;
void *shape_arrays[5] = {};
HRESULT WINAPI allocate_shape_contexts(ShapeContexts *contexts) {
    if (!contexts)
        return E_INVALIDARG;
    const std::lock_guard lock(shape_mutex);
    for (int i = 0; i < 5; ++i) {
        contexts->contexts[i] = nullptr;
        if (!contexts->counts[i])
            continue;
        if (!(shape_arrays[i] = apu_memory(contexts->counts[i] * context_bytes, 256)))
            return E_OUTOFMEMORY;
        contexts->contexts[i] = shape_arrays[i];
    }
    std::fprintf(stderr, "[Runtime] SHAPE contexts: %u, %u, %u, %u, %u (none of them run)\n",
                 contexts->counts[0], contexts->counts[1], contexts->counts[2], contexts->counts[3],
                 contexts->counts[4]);
    std::fflush(stderr);
    return S_OK;
}
HRESULT WINAPI release_shape_contexts() {
    const std::lock_guard lock(shape_mutex);
    for (auto *&array : shape_arrays)
        if (array)
            release_apu_memory(std::exchange(array, nullptr));
    return S_OK;
}

// A message as PopMessage gives it: its type, then what it is about. For a completed command
// that is the command's type, its id and the audio frame it completed in. The first
// development kits end after the id.
struct Message {
    std::uint32_t type, reserved;
    std::uint64_t value, id;
    std::uint32_t frame, unused;
};
static_assert(sizeof(Message) == 32);
constexpr std::size_t early_message_bytes = 24;
constexpr std::uint32_t audio_frame = 2, command_done = 0x10, disconnected = 0x80;
constexpr auto frame = std::chrono::microseconds(2667);

struct Hal;
struct HalVtable {
    HRESULT(STDMETHODCALLTYPE *connect)(Hal *, std::uint32_t, std::uint32_t);
    HRESULT(STDMETHODCALLTYPE *disconnect)(Hal *);
    HRESULT(STDMETHODCALLTYPE *submit)
    (Hal *, std::uint32_t, std::uint64_t, std::uint32_t, void *, std::uint32_t);
    bool(STDMETHODCALLTYPE *pop)(Hal *, Message *);
    std::uint32_t(STDMETHODCALLTYPE *message_count)(Hal *);
    void(STDMETHODCALLTYPE *release)(Hal *);
};
struct Hal {
    const HalVtable *vtable;
    std::mutex mutex;
    std::deque<Message> messages;
    bool connected = false;
    std::chrono::steady_clock::time_point next_frame;
    std::uint32_t seen[8] = {}; // command types logged
    std::uint64_t commands = 0; // completed so far
    std::uint32_t frames = 0;   // audio frames ticked so far
};
HRESULT STDMETHODCALLTYPE connect(Hal *self, std::uint32_t commands, std::uint32_t messages) {
    const std::lock_guard lock(self->mutex);
    self->connected = true;
    self->next_frame = std::chrono::steady_clock::now() + frame;
    std::fprintf(stderr, "[Runtime] audio processor connected (%u commands, %u messages)\n",
                 commands, messages);
    std::fflush(stderr);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE disconnect(Hal *self) {
    const std::lock_guard lock(self->mutex);
    self->connected = false;
    self->messages.push_back({disconnected, 0, 0, 0, 0, 0});
    return S_OK;
}
HRESULT STDMETHODCALLTYPE submit(Hal *self, std::uint32_t type, std::uint64_t id,
                                 std::uint32_t frame_index, void *data, std::uint32_t) {
    const std::lock_guard lock(self->mutex);
    if (!self->connected)
        return E_UNEXPECTED;
    if (type < 256 && !(self->seen[type / 32] & 1u << type % 32)) {
        self->seen[type / 32] |= 1u << type % 32;
        std::fprintf(stderr, "[Runtime] audio processor command 0x%x\n", type);
        std::fflush(stderr);
    }
    if (static std::atomic<int> shown{0}; std::getenv("XV_ACP_TRACE") && shown++ < 40) {
        const auto *words = static_cast<const std::uint32_t *>(data);
        std::fprintf(stderr,
                     "[Runtime] audio command %u id %llx frame %u data %x %x %x %x from %s\n", type,
                     static_cast<unsigned long long>(id), frame_index, words ? words[0] : 0,
                     words ? words[1] : 0, words ? words[2] : 0, words ? words[3] : 0,
                     detail::guest_location(_ReturnAddress()).c_str());
    }
    // Every command is completed. One the title gave no id is known by its place among the
    // commands, counted from 1: a title that sends its first command without an id waits for
    // the completion numbered 1 before it sends another.
    const auto sequence = ++self->commands;
    Message done{command_done, 0, type, id ? id : sequence, self->frames, 0};
    // the first development kits have the command's id right after its type, with no time
    // before them
    if (early_kit)
        done.value = std::uint64_t{static_cast<std::uint32_t>(done.id)} << 32 | type;
    self->messages.push_back(done);
    return S_OK;
}
bool STDMETHODCALLTYPE pop(Hal *self, Message *message) {
    if (!message)
        return false;
    if (static std::atomic<int> shown{0}; std::getenv("XV_ACP_TRACE") && shown++ < 1)
        std::fprintf(stderr, "[Runtime] PopMessage from %s\n",
                     detail::guest_location(_ReturnAddress()).c_str());
    const std::lock_guard lock(self->mutex);
    if (self->messages.empty() && self->connected) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= self->next_frame) {
            self->next_frame = std::max(self->next_frame + frame, now - 8 * frame);
            ++self->frames;
            const Message tick{audio_frame, 0, 0, 0, 0, 0};
            std::memcpy(message, &tick, early_kit ? early_message_bytes : sizeof tick);
            return true;
        }
    }
    if (self->messages.empty())
        return false;
    std::memcpy(message, &self->messages.front(),
                early_kit ? early_message_bytes : sizeof(Message));
    self->messages.pop_front();
    return true;
}
// GetNumMessages: what PopMessage has to give, the audio frame that is due among them
std::uint32_t STDMETHODCALLTYPE message_count(Hal *self) {
    const std::lock_guard lock(self->mutex);
    const bool frame_due = self->connected && std::chrono::steady_clock::now() >= self->next_frame;
    return static_cast<std::uint32_t>(self->messages.size()) + frame_due;
}
void STDMETHODCALLTYPE release(Hal *self) { delete self; }
constexpr HalVtable hal_vtable{&connect, &disconnect, &submit, &pop, &message_count, &release};

HRESULT WINAPI acp_hal_create(Hal **hal) {
    if (!hal)
        return E_INVALIDARG;
    *hal = new Hal{&hal_vtable};
    return S_OK;
}

// xapu.dll, the Series console's audio decoder (XAPU): Opus decoding and sample rate conversion
// on the audio hardware, as commands for a client's streams and their results. The names and
// rules are those of xapu.h (Microsoft's XAPU reference for the GDK); the structures are packed,
// as titles lay them out. A command is carried out when it is queued, so its result is ready at
// once and the results come in the order of the commands, with DisableFifoOrdering or without it.
enum class XApuProcessType : std::uint32_t { decode_opus = 1, decode_convert_opus, convert_pcm };
enum class XApuCommandType : std::uint32_t { activate = 1, deactivate, process, notify, reset };
#pragma pack(push, 1)
struct XApuCommandId {
    XApuCommandType type;
    std::uint32_t stream_index, sequence, options;
    void *context;
};
struct XApuDecodeConvertActivateCommand {
    XApuCommandId id;
    std::uint32_t channel_count, initialize_data_length;
    void *initialize_data;
    float start_pitch;
    std::uint32_t processing_buffer_length;
    void *processing_buffer;
};
struct XApuDecodeConvertCommand {
    XApuCommandId id;
    float pitch_ramp_rate, target_pitch;
    std::uint32_t first_frame_index, frame_count, input_data_length;
    const void *input_data;
    std::uint32_t max_output_data_length;
    void *output_data;
};
struct XApuResult {
    XApuCommandId id;
    std::uint32_t state;
    std::uint64_t state_data, total_execution_time; // the time in microseconds
    HRESULT error_code;
    std::uint32_t output_frame_count, output_data_length;
    void *output_data;
    float current_pitch;
    std::uint32_t input_data_length;
    const void *input_data;
};
struct XApuConnectInputParameters {
    XApuProcessType process_type;
    std::uint32_t max_stream_count, max_queued_commands_per_stream, base_data_length,
        max_frame_count_per_output, options;
};
struct XApuConnectOutputParameters {
    void *base_data;
    HANDLE result_ready_event;
};
struct XApuUtilization {
    std::uint32_t available_stream_count, maximum_stream_count;
};
#pragma pack(pop)
static_assert(sizeof(XApuDecodeConvertActivateCommand) == 56 &&
              sizeof(XApuDecodeConvertCommand) == 64 && sizeof(XApuResult) == 80);
// the flags of XApuCommandOptions, XApuConnectOptions and XApuStateOptions
constexpr std::uint32_t signal_on_completion = 1;
constexpr std::uint32_t deinterleaved_output = 4, enable_result_ready_event = 8;
constexpr std::uint32_t output_ready = 1, need_more_input = 2;
constexpr HRESULT xapu_e_queue_full = static_cast<HRESULT>(0x8ac81000),
                  xapu_e_pending_results = static_cast<HRESULT>(0x8ac81001),
                  xapu_e_invalid_command = static_cast<HRESULT>(0x8ac81002),
                  xapu_e_invalid_stream_index = static_cast<HRESULT>(0x8ac81003),
                  xapu_e_already_active = static_cast<HRESULT>(0x8ac81005),
                  xapu_e_buffer_too_small = static_cast<HRESULT>(0x8ac81006),
                  xapu_e_bad_opus_packet = static_cast<HRESULT>(0x8ac81008);
constexpr int max_packet_frames = 5760; // the longest Opus packet at 48 kHz (120 ms)

struct DestroyOpusDecoder {
    void operator()(OpusDecoder *decoder) const { opus_decoder_destroy(decoder); }
};
struct XApuStream {
    bool active = false;
    std::uint32_t channels = 0;
    std::unique_ptr<OpusDecoder, DestroyOpusDecoder> decoder; // for DecodeOpus
    std::uint32_t queued = 0;                                 // its results not dequeued yet
};
// what an XApuHandle stands for
struct XApuClient {
    XApuConnectInputParameters parameters;
    void *base_data = nullptr;
    HANDLE result_ready_event = nullptr;
    std::mutex mutex;
    std::vector<XApuStream> streams;
    std::deque<XApuResult> results;
    std::uint32_t commands = 0, dequeued = 0; // since the connection
    std::vector<float> pcm;                   // a packet as libopus decodes it
};

// XApuConnect: one process type for every stream of the client, and the memory its inputs,
// outputs and processing buffers are placed in. DecodeOpus has no output length of its own; the
// conversions have one of five.
HRESULT WINAPI xapu_connect(const XApuConnectInputParameters *params,
                            XApuConnectOutputParameters *data, XApuClient **handle) {
    if (data)
        *data = {};
    if (!params || !data || !handle)
        return E_INVALIDARG;
    const auto &input = *params;
    const auto frames = input.max_frame_count_per_output;
    const bool decoding = input.process_type == XApuProcessType::decode_opus;
    const bool converting = input.process_type == XApuProcessType::decode_convert_opus ||
                            input.process_type == XApuProcessType::convert_pcm;
    if (!(decoding ? frames == 0
                   : converting && (frames == 256 || frames == 480 || frames == 512 ||
                                    frames == 960 || frames == 1024)) ||
        input.max_stream_count < 1 || input.max_stream_count > 500 ||
        input.max_queued_commands_per_stream < 1 || input.max_queued_commands_per_stream > 16 ||
        !input.base_data_length)
        return E_INVALIDARG;
    auto *const base_data = allocate_aligned(input.base_data_length, 0x10000, nullptr);
    if (!base_data)
        return E_OUTOFMEMORY;
    auto *const client = new XApuClient;
    client->parameters = input;
    client->base_data = base_data;
    client->streams.resize(input.max_stream_count);
    if (decoding)
        client->pcm.resize(std::size_t{max_packet_frames} * 2);
    if (input.options & enable_result_ready_event)
        client->result_ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    *data = {client->base_data, client->result_ready_event};
    *handle = client;
    std::fprintf(stderr, "[Runtime] audio decoder connected: process type %u, %u streams%s\n",
                 static_cast<std::uint32_t>(input.process_type), input.max_stream_count,
                 decoding ? "" : " (no rate conversion: silence)");
    std::fflush(stderr);
    return S_OK;
}
// XApuDisconnect frees all the client has, and says when it left results it did not dequeue
HRESULT WINAPI xapu_disconnect(XApuClient *client) {
    if (!client)
        return E_INVALIDARG;
    const bool pending = !client->results.empty();
    VirtualFree(client->base_data, 0, MEM_RELEASE);
    if (client->result_ready_event)
        CloseHandle(client->result_ready_event);
    delete client;
    return pending ? xapu_e_pending_results : S_OK;
}

HRESULT activate(const XApuClient &client, XApuStream &stream,
                 const XApuDecodeConvertActivateCommand &command) {
    if (stream.active)
        return xapu_e_already_active;
    if (command.channel_count < 1 || command.channel_count > 2)
        return E_INVALIDARG;
    if (client.parameters.process_type == XApuProcessType::decode_opus) {
        int error = OPUS_OK;
        stream.decoder.reset(
            opus_decoder_create(48000, static_cast<int>(command.channel_count), &error));
        if (!stream.decoder)
            return E_OUTOFMEMORY;
    }
    stream.active = true;
    stream.channels = command.channel_count;
    return S_OK;
}
// DecodeOpus: one packet into the command's output, its frames from first_frame_index on and at
// most frame_count of them. Deinterleaved, the right channel follows the left one's frames. What
// does not fit is said, and what fits is kept.
void decode(XApuClient &client, XApuStream &stream, const XApuDecodeConvertCommand &command,
            XApuResult &result) {
    const int decoded = opus_decode_float(stream.decoder.get(),
                                          static_cast<const unsigned char *>(command.input_data),
                                          static_cast<opus_int32>(command.input_data_length),
                                          client.pcm.data(), max_packet_frames, 0);
    if (decoded < 0) {
        result.error_code = xapu_e_bad_opus_packet;
        return;
    }
    const auto channels = stream.channels;
    const auto frame_bytes = channels * static_cast<std::uint32_t>(sizeof(float));
    const auto first = std::min(command.first_frame_index, static_cast<std::uint32_t>(decoded));
    const auto wanted = std::min(static_cast<std::uint32_t>(decoded) - first, command.frame_count);
    const auto frames = std::min(wanted, command.max_output_data_length / frame_bytes);
    if (frames < wanted)
        result.error_code = xapu_e_buffer_too_small;
    const float *const pcm = client.pcm.data() + std::size_t{first} * channels;
    auto *const output = static_cast<float *>(command.output_data);
    if (channels == 2 && client.parameters.options & deinterleaved_output) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            output[i] = pcm[2 * i];
            output[frames + i] = pcm[2 * i + 1];
        }
    } else {
        std::memcpy(output, pcm, std::size_t{frames} * frame_bytes);
    }
    result.output_frame_count = frames;
    result.output_data_length = frames * frame_bytes;
}
void process(XApuClient &client, XApuStream &stream, const XApuDecodeConvertCommand &command,
             XApuResult &result) {
    result.output_data = command.output_data;
    result.input_data_length = command.input_data_length;
    result.input_data = command.input_data;
    if (!stream.active) {
        result.error_code = xapu_e_invalid_stream_index;
    } else if (!command.input_data || !command.input_data_length || !command.output_data) {
        result.error_code = E_INVALIDARG;
    } else if (client.parameters.process_type == XApuProcessType::decode_opus) {
        decode(client, stream, command, result);
    } else {
        // the rate conversion is not done here: the input is taken whole, and an output's
        // length of silence given for it
        const auto frame_bytes = stream.channels * static_cast<std::uint32_t>(sizeof(float));
        const auto frames = std::min(client.parameters.max_frame_count_per_output,
                                     command.max_output_data_length / frame_bytes);
        std::memset(command.output_data, 0, std::size_t{frames} * frame_bytes);
        result.state = output_ready | need_more_input;
        result.output_frame_count = frames;
        result.output_data_length = frames * frame_bytes;
        result.current_pitch = command.target_pitch;
    }
}
// XApuEnqueueCommand: the command is carried out at once, and its result keeps a place in its
// stream's queue until it is dequeued
HRESULT WINAPI xapu_enqueue_command(XApuClient *client, const XApuCommandId *command,
                                    std::uint32_t *total_command_count) {
    if (!client || !command)
        return E_INVALIDARG;
    if (command->type < XApuCommandType::activate || command->type > XApuCommandType::reset)
        return xapu_e_invalid_command;
    const std::lock_guard lock(client->mutex);
    if (command->stream_index >= client->streams.size())
        return xapu_e_invalid_stream_index;
    auto &stream = client->streams[command->stream_index];
    if (stream.queued >= client->parameters.max_queued_commands_per_stream)
        return xapu_e_queue_full;
    const auto start = std::chrono::steady_clock::now();
    XApuResult result{};
    result.id = *command;
    result.current_pitch = 1;
    switch (command->type) {
    case XApuCommandType::activate:
        result.error_code = activate(
            *client, stream, *reinterpret_cast<const XApuDecodeConvertActivateCommand *>(command));
        break;
    case XApuCommandType::deactivate:
        stream.active = false;
        stream.decoder.reset();
        break;
    case XApuCommandType::process:
        process(*client, stream, *reinterpret_cast<const XApuDecodeConvertCommand *>(command),
                result);
        break;
    case XApuCommandType::notify: // not used
        break;
    case XApuCommandType::reset: // the stream as its activation left it
        if (stream.decoder)
            opus_decoder_ctl(stream.decoder.get(), OPUS_RESET_STATE);
        break;
    }
    result.total_execution_time = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
    ++stream.queued;
    ++client->commands;
    client->results.push_back(result);
    if (total_command_count)
        *total_command_count = client->commands;
    if (command->options & signal_on_completion && client->result_ready_event)
        SetEvent(client->result_ready_event);
    return S_OK;
}
// XApuDequeueResult: the oldest result, which frees its place in its stream's queue
HRESULT WINAPI xapu_dequeue_result(XApuClient *client, XApuResult *command_result) {
    if (!client || !command_result)
        return E_INVALIDARG;
    const std::lock_guard lock(client->mutex);
    if (client->results.empty())
        return xapu_e_pending_results;
    const auto result = client->results.front();
    client->results.pop_front();
    --client->streams[result.id.stream_index].queued;
    ++client->dequeued;
    *command_result = result;
    return S_OK;
}
// XApuGetResultCount: the results waiting, and how many have been dequeued since the connection
std::uint32_t WINAPI xapu_get_result_count(XApuClient *client, std::uint32_t *total_result_count) {
    if (!client)
        return 0;
    const std::lock_guard lock(client->mutex);
    if (total_result_count)
        *total_result_count = client->dequeued;
    return static_cast<std::uint32_t>(client->results.size());
}
// XApuGetUtilization: the streams the decoder can take now, and the most it processes at once
HRESULT WINAPI xapu_get_utilization(XApuProcessType, std::uint32_t, XApuUtilization *utilization) {
    if (!utilization)
        return E_INVALIDARG;
    *utilization = {256, 256};
    return S_OK;
}

template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
} // namespace

void install_acphal(Registry &registry) {
    const std::pair<const char *, void *> functions[] = {
        {"ApuAlloc", erase(apu_alloc)},
        {"ApuFree", erase(apu_free)},
        {"ApuCreateHeap", erase(apu_create_heap)},
        {"ApuMapVirtualAddress", erase(apu_map_virtual_address)},
        {"ApuMapApuAddress", erase(apu_map_apu_address)},
        {"ApuHeapGetState", erase(apu_heap_state)},
        {"ApuIsVirtualAddressValid", erase(apu_is_virtual_address_valid)},
        {"AcpHalCreate", erase(acp_hal_create)},
        {"AcpHalAllocateShapeContexts", erase(allocate_shape_contexts)},
        {"AcpHalReleaseShapeContexts", erase(release_shape_contexts)}};
    for (const auto &[name, function] : functions)
        registry.add("acphal.dll", name, function);
    const std::pair<const char *, void *> decoder[] = {
        {"XApuConnect", erase(xapu_connect)},
        {"XApuDisconnect", erase(xapu_disconnect)},
        {"XApuEnqueueCommand", erase(xapu_enqueue_command)},
        {"XApuDequeueResult", erase(xapu_dequeue_result)},
        {"XApuGetResultCount", erase(xapu_get_result_count)},
        {"XApuGetUtilization", erase(xapu_get_utilization)}};
    for (const auto &[name, function] : decoder)
        registry.add("xapu.dll", name, function);
    registry.add("chad.dll", "XDspConnect", erase(no_audio_dsp));
    registry.add("chad.dll", "XDspConnectWithMaximumStreamLimit", erase(xdsp_connect_with_limit));
    registry.add("chad.dll", "XDspDisconnect", erase(xdsp_disconnect));
    for (const auto *name : {"XDspActivate", "XDspDeactivate", "XDspSubmitCommand"})
        registry.add("chad.dll", name, erase(xdsp_command));
    registry.add("windows.media.mediacontrol.dll",
                 "CreateRenderAudioStateMonitorForCategoryAndDeviceRole",
                 erase(create_state_monitor));
}
void early_audio_processor(bool yes) { early_kit = yes; }
} // namespace xv::runtime::modules
