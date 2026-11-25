#include "runtime/common/runtime.hpp"
#include "runtime/modules/acphal.hpp"
#include <algorithm>
#include <catch2/catch.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <opus.h>
#include <utility>
#include <vector>
#include <windows.h>

// xapu.dll as a title's audio middleware drives it on a Series console: XApuConnect for
// DecodeOpus, then Activate and Process commands for its streams, each result dequeued. What it
// decodes must be what libopus makes of the same packets.
namespace {
using namespace xv::runtime;

// the structures of xapu.h (Microsoft's XAPU reference for the GDK), packed as titles lay them out
#pragma pack(push, 1)
struct CommandId {
    std::uint32_t type, stream_index, sequence, options;
    void *context;
};
struct ActivateCommand {
    CommandId id;
    std::uint32_t channel_count, initialize_data_length;
    void *initialize_data;
    float start_pitch;
    std::uint32_t processing_buffer_length;
    void *processing_buffer;
};
struct ProcessCommand {
    CommandId id;
    float pitch_ramp_rate, target_pitch;
    std::uint32_t first_frame_index, frame_count, input_data_length;
    const void *input_data;
    std::uint32_t max_output_data_length;
    void *output_data;
};
struct Result {
    CommandId id;
    std::uint32_t state;
    std::uint64_t state_data, total_execution_time;
    HRESULT error_code;
    std::uint32_t output_frame_count, output_data_length;
    void *output_data;
    float current_pitch;
    std::uint32_t input_data_length;
    const void *input_data;
};
#pragma pack(pop)
static_assert(sizeof(ActivateCommand) == 56 && sizeof(ProcessCommand) == 64 &&
              sizeof(Result) == 80);
struct ConnectInput {
    std::uint32_t process_type, max_stream_count, max_queued_commands_per_stream, base_data_length,
        max_frame_count_per_output, options;
};
struct ConnectOutput {
    void *base_data;
    HANDLE result_ready_event;
};

constexpr std::uint32_t decode_opus = 1, convert_pcm = 3;
constexpr std::uint32_t activate = 1, deactivate = 2, process = 3, notify = 4, reset = 5;
constexpr std::uint32_t signal_on_completion = 1;
constexpr std::uint32_t disable_fifo_ordering = 1, deinterleaved_output = 4,
                        enable_result_ready_event = 8;
constexpr std::uint32_t output_ready = 1, need_more_input = 2;
constexpr HRESULT queue_full = static_cast<HRESULT>(0x8ac81000),
                  pending_results = static_cast<HRESULT>(0x8ac81001),
                  invalid_command = static_cast<HRESULT>(0x8ac81002),
                  invalid_stream_index = static_cast<HRESULT>(0x8ac81003),
                  already_active = static_cast<HRESULT>(0x8ac81005),
                  buffer_too_small = static_cast<HRESULT>(0x8ac81006),
                  bad_opus_packet = static_cast<HRESULT>(0x8ac81008);
constexpr std::uint32_t no_truncation = 0xffffffff;
constexpr std::uint32_t stereo_packet_bytes = 960 * 2 * sizeof(float);

// the functions as a title imports them
struct Xapu {
    HRESULT(WINAPI *connect)(const ConnectInput *, ConnectOutput *, void **);
    HRESULT(WINAPI *disconnect)(void *);
    HRESULT(WINAPI *enqueue_command)(void *, const void *, std::uint32_t *);
    HRESULT(WINAPI *dequeue_result)(void *, Result *);
    std::uint32_t(WINAPI *get_result_count)(void *, std::uint32_t *);
    HRESULT(WINAPI *get_utilization)(std::uint32_t, std::uint32_t, std::uint32_t *);
};
const Xapu &xapu() {
    static const Xapu functions = [] {
        Registry registry;
        modules::install_acphal(registry);
        const auto find = [&](const char *name) { return registry.find("xapu.dll", name); };
        Xapu found{};
        found.connect = reinterpret_cast<decltype(found.connect)>(find("XApuConnect"));
        found.disconnect = reinterpret_cast<decltype(found.disconnect)>(find("XApuDisconnect"));
        found.enqueue_command =
            reinterpret_cast<decltype(found.enqueue_command)>(find("XApuEnqueueCommand"));
        found.dequeue_result =
            reinterpret_cast<decltype(found.dequeue_result)>(find("XApuDequeueResult"));
        found.get_result_count =
            reinterpret_cast<decltype(found.get_result_count)>(find("XApuGetResultCount"));
        found.get_utilization =
            reinterpret_cast<decltype(found.get_utilization)>(find("XApuGetUtilization"));
        return found;
    }();
    REQUIRE((functions.connect && functions.disconnect && functions.enqueue_command &&
             functions.dequeue_result && functions.get_result_count && functions.get_utilization));
    return functions;
}

using Packet = std::vector<unsigned char>;
using Pcm = std::vector<float>;
// 20 ms packets of a signal that tells the channels apart: 440 Hz on the left, 660 Hz on the
// right. Each is padded to a multiple of 16 bytes with Opus's own padding, as xapu.h asks of an
// input.
std::vector<Packet> encode(int channels, int count) {
    int error = 0;
    auto *const encoder = opus_encoder_create(48000, channels, OPUS_APPLICATION_AUDIO, &error);
    REQUIRE(encoder);
    std::vector<Packet> packets;
    Pcm pcm(std::size_t{960} * channels);
    for (int packet = 0; packet < count; ++packet) {
        for (int i = 0; i < 960; ++i) {
            const double time = (packet * 960 + i) / 48000.0;
            pcm[i * channels] =
                static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 440 * time));
            if (channels == 2)
                pcm[i * 2 + 1] =
                    static_cast<float>(0.3 * std::sin(2 * 3.14159265358979 * 660 * time));
        }
        Packet bytes(1280);
        const int length = opus_encode_float(encoder, pcm.data(), 960, bytes.data(), 1264);
        REQUIRE(length > 0);
        const int padded = (length + 15) & ~15;
        REQUIRE(opus_packet_pad(bytes.data(), length, padded) == OPUS_OK);
        bytes.resize(padded);
        packets.push_back(bytes);
    }
    opus_encoder_destroy(encoder);
    return packets;
}
// what libopus itself makes of the packets, one decoder for all of them in turn; nothing for a
// packet it refuses
std::vector<Pcm> reference(const std::vector<Packet> &packets, int channels) {
    int error = 0;
    auto *const decoder = opus_decoder_create(48000, channels, &error);
    REQUIRE(decoder);
    std::vector<Pcm> decoded;
    for (const auto &packet : packets) {
        Pcm pcm(std::size_t{5760} * channels);
        const int frames = opus_decode_float(
            decoder, packet.data(), static_cast<opus_int32>(packet.size()), pcm.data(), 5760, 0);
        pcm.resize(frames < 0 ? 0 : std::size_t{static_cast<unsigned>(frames)} * channels);
        decoded.push_back(pcm);
    }
    opus_decoder_destroy(decoder);
    return decoded;
}
Pcm part(const Pcm &pcm, std::size_t first, std::size_t count) {
    return Pcm(pcm.begin() + first, pcm.begin() + first + count);
}

// A connection as a title makes it: 256 streams of two commands each. It lays out the memory
// it is given as a packet and an output per stream.
struct Connection {
    void *handle = nullptr;
    ConnectOutput output{};
    explicit Connection(std::uint32_t options, std::uint32_t process_type = decode_opus,
                        std::uint32_t frames_per_output = 0) {
        const ConnectInput input{process_type, 256, 2, 0x104000, frames_per_output, options};
        REQUIRE(xapu().connect(&input, &output, &handle) == S_OK);
        REQUIRE(handle);
        REQUIRE(output.base_data);
        CHECK(reinterpret_cast<std::uintptr_t>(output.base_data) % 16 == 0);
    }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection() { xapu().disconnect(handle); }
    unsigned char *input(std::uint32_t stream) const {
        return static_cast<unsigned char *>(output.base_data) + stream * 0x1000;
    }
    float *output_of(std::uint32_t stream) const {
        return reinterpret_cast<float *>(static_cast<unsigned char *>(output.base_data) + 0x80000 +
                                         stream * 0x2000);
    }
    template <class Command>
    HRESULT enqueue(const Command &command, std::uint32_t *total = nullptr) const {
        return xapu().enqueue_command(handle, &command, total);
    }
    Result dequeue() const {
        Result result{};
        REQUIRE(xapu().dequeue_result(handle, &result) == S_OK);
        return result;
    }
    // a command, then its result
    template <class Command> Result run(const Command &command) const {
        REQUIRE(enqueue(command) == S_OK);
        return dequeue();
    }
    ActivateCommand activation(std::uint32_t stream, std::uint32_t channels) const {
        return {{activate, stream, 0, 0, nullptr}, channels, 0, nullptr, 1.0f, 0, nullptr};
    }
    ProcessCommand processing(std::uint32_t stream, const Packet &packet,
                              std::uint32_t max_output = stereo_packet_bytes) const {
        std::memcpy(input(stream), packet.data(), packet.size());
        return {{process, stream, 0, 0, nullptr},
                0.0f,
                1.0f,
                0,
                no_truncation,
                static_cast<std::uint32_t>(packet.size()),
                input(stream),
                max_output,
                output_of(stream)};
    }
};
Pcm output_of(const Result &result, std::size_t channels) {
    const auto *const pcm = static_cast<const float *>(result.output_data);
    return Pcm(pcm, pcm + std::size_t{result.output_frame_count} * channels);
}
} // namespace

TEST_CASE("xapu decodes a title's Opus packets as libopus does") {
    const auto stereo = encode(2, 8), mono = encode(1, 8);
    const auto stereo_pcm = reference(stereo, 2), mono_pcm = reference(mono, 1);
    const Connection connection(disable_fifo_ordering);
    const auto stereo_stream = connection.activation(3, 2),
               mono_stream = connection.activation(5, 1);
    std::uint32_t total = 0;
    REQUIRE(connection.enqueue(stereo_stream, &total) == S_OK);
    CHECK(total == 1);
    REQUIRE(connection.enqueue(mono_stream, &total) == S_OK);
    CHECK(total == 2);
    for (const auto expected : {3u, 5u}) {
        const auto activated = connection.dequeue();
        CHECK(activated.id.type == activate);
        CHECK(activated.id.stream_index == expected);
        CHECK(activated.error_code == S_OK);
    }
    // the streams' packets in turn: each stream has a decoder of its own
    for (std::size_t i = 0; i < stereo.size(); ++i) {
        auto command = connection.processing(3, stereo[i]);
        command.id.sequence = static_cast<std::uint32_t>(i);
        command.id.context = &command;
        const auto result = connection.run(command);
        CHECK(result.id.type == process);
        CHECK(result.id.stream_index == 3);
        CHECK(result.id.sequence == i);
        CHECK(result.id.context == &command);
        CHECK(result.error_code == S_OK);
        CHECK(result.state == 0);
        CHECK(result.output_frame_count == 960);
        CHECK(result.output_data_length == stereo_packet_bytes);
        CHECK(result.output_data == command.output_data);
        CHECK(result.input_data == command.input_data);
        CHECK(result.input_data_length == stereo[i].size());
        CHECK(result.current_pitch == 1.0f);
        REQUIRE(stereo_pcm[i].size() == 960 * 2);
        CHECK(output_of(result, 2) == stereo_pcm[i]);

        const auto mono_result = connection.run(connection.processing(5, mono[i], 960 * 4));
        CHECK(mono_result.error_code == S_OK);
        CHECK(mono_result.output_frame_count == 960);
        CHECK(mono_result.output_data_length == 960 * 4);
        CHECK(output_of(mono_result, 1) == mono_pcm[i]);
    }
    // it is the signal: the left channel is the louder one
    const auto &last = stereo_pcm.back();
    double left = 0, right = 0;
    for (std::size_t i = 0; i < 960; ++i) {
        left += last[2 * i] * last[2 * i];
        right += last[2 * i + 1] * last[2 * i + 1];
    }
    CHECK(left / 960 > 0.08);
    CHECK(right / 960 > 0.02);
    CHECK(left > right);
}

TEST_CASE("deinterleaved output has the right channel after the left one's frames") {
    const auto stereo = encode(2, 3), mono = encode(1, 1);
    const auto expected = reference(stereo, 2), mono_pcm = reference(mono, 1);
    const Connection connection(deinterleaved_output);
    REQUIRE(connection.run(connection.activation(0, 2)).error_code == S_OK);
    REQUIRE(connection.run(connection.activation(1, 1)).error_code == S_OK);
    const auto check = [&](const Result &result, const Pcm &pcm, std::size_t first,
                           std::size_t frames) {
        REQUIRE(result.output_frame_count == frames);
        Pcm left, right;
        for (std::size_t i = first; i < first + frames; ++i) {
            left.push_back(pcm[2 * i]);
            right.push_back(pcm[2 * i + 1]);
        }
        const auto *const output = static_cast<const float *>(result.output_data);
        CHECK(Pcm(output, output + frames) == left);
        CHECK(Pcm(output + frames, output + 2 * frames) == right);
    };
    check(connection.run(connection.processing(0, stereo[0])), expected[0], 0, 960);
    // with a part of the packet, the right channel starts where the left one's frames end
    auto command = connection.processing(0, stereo[1]);
    command.first_frame_index = 100;
    command.frame_count = 500;
    check(connection.run(command), expected[1], 100, 500);
    // one channel is the same either way
    const auto result = connection.run(connection.processing(1, mono[0], 960 * 4));
    CHECK(output_of(result, 1) == mono_pcm[0]);
}

TEST_CASE("a Process keeps the frames from first_frame_index on, at most frame_count of them") {
    const auto stereo = encode(2, 4);
    const auto expected = reference(stereo, 2);
    const Connection connection(0);
    REQUIRE(connection.run(connection.activation(0, 2)).error_code == S_OK);
    // frame-accurate seeking: frames 100 to 600 of the packet
    auto command = connection.processing(0, stereo[0]);
    command.first_frame_index = 100;
    command.frame_count = 500;
    auto result = connection.run(command);
    CHECK(result.error_code == S_OK);
    CHECK(result.output_frame_count == 500);
    CHECK(result.output_data_length == 500 * 8);
    CHECK(output_of(result, 2) == part(expected[0], 200, 1000));
    // the end of a packet is all there is from a frame near it
    command = connection.processing(0, stereo[1]);
    command.first_frame_index = 900;
    result = connection.run(command);
    CHECK(result.output_frame_count == 60);
    CHECK(output_of(result, 2) == part(expected[1], 1800, 120));
    // a preroll packet gives no output and still moves the decoder on
    command = connection.processing(0, stereo[2]);
    command.first_frame_index = 960;
    result = connection.run(command);
    CHECK(result.error_code == S_OK);
    CHECK(result.output_frame_count == 0);
    CHECK(result.output_data_length == 0);
    // an output too small for the frames holds what fits, and the result says so
    result = connection.run(connection.processing(0, stereo[3], 400 * 8));
    CHECK(result.error_code == buffer_too_small);
    CHECK(result.output_frame_count == 400);
    CHECK(result.output_data_length == 400 * 8);
    CHECK(output_of(result, 2) == part(expected[3], 0, 800));
}

TEST_CASE("a damaged packet fails in its result, and its stream decodes on") {
    const auto good = encode(2, 3);
    // its first byte says a count of frames follows, and the count is zero
    Packet damaged(16);
    damaged[0] = 0xff;
    const std::vector<Packet> packets{good[0], damaged, good[1], good[2]};
    const auto expected = reference(packets, 2);
    REQUIRE(expected[1].empty()); // libopus refuses it as well
    const Connection connection(0);
    REQUIRE(connection.run(connection.activation(2, 2)).error_code == S_OK);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto result = connection.run(connection.processing(2, packets[i]));
        CHECK(result.error_code == (i == 1 ? bad_opus_packet : S_OK));
        CHECK(result.output_frame_count == expected[i].size() / 2);
        CHECK(result.output_data_length == expected[i].size() * sizeof(float));
        CHECK(output_of(result, 2) == expected[i]);
    }
}

TEST_CASE("Reset puts a stream's decoder back to its activation") {
    const auto stereo = encode(2, 5);
    const auto expected = reference(stereo, 2);
    const auto fresh = reference({stereo[4]}, 2);
    REQUIRE(fresh[0] != expected[4]); // the packets before change what the fifth one decodes to
    const Connection connection(0);
    for (const std::uint32_t stream : {0u, 1u})
        REQUIRE(connection.run(connection.activation(stream, 2)).error_code == S_OK);
    for (std::size_t i = 0; i < 4; ++i)
        for (const std::uint32_t stream : {0u, 1u})
            REQUIRE(output_of(connection.run(connection.processing(stream, stereo[i])), 2) ==
                    expected[i]);
    const CommandId reset_command{reset, 0, 0, 0, nullptr};
    const auto reset_result = connection.run(reset_command);
    CHECK(reset_result.id.type == reset);
    CHECK(reset_result.error_code == S_OK);
    CHECK(output_of(connection.run(connection.processing(0, stereo[4])), 2) == fresh[0]);
    CHECK(output_of(connection.run(connection.processing(1, stereo[4])), 2) == expected[4]);
}

TEST_CASE("each stream holds as many results as the connection allows until they are dequeued") {
    const auto stereo = encode(2, 2);
    const Connection connection(0);
    std::uint32_t total = 0, dequeued = 0;
    REQUIRE(connection.enqueue(connection.activation(0, 2), &total) == S_OK);
    CHECK(total == 1);
    REQUIRE(connection.enqueue(connection.processing(0, stereo[0]), &total) == S_OK);
    CHECK(total == 2);
    // two results wait for stream 0: its queue is full, and another stream's is not
    CHECK(connection.enqueue(connection.processing(0, stereo[1])) == queue_full);
    REQUIRE(connection.enqueue(connection.activation(1, 1), &total) == S_OK);
    CHECK(total == 3);
    CHECK(xapu().get_result_count(connection.handle, &dequeued) == 3);
    CHECK(dequeued == 0);
    // a result dequeued makes room in its stream's queue
    const auto first = connection.dequeue();
    CHECK(first.id.type == activate);
    CHECK(first.id.stream_index == 0);
    CHECK(xapu().get_result_count(connection.handle, &dequeued) == 2);
    CHECK(dequeued == 1);
    REQUIRE(connection.enqueue(connection.processing(0, stereo[1]), &total) == S_OK);
    CHECK(total == 4);
    // the results come in the order of the commands
    const std::pair<std::uint32_t, std::uint32_t> order[] = {
        {process, 0}, {activate, 1}, {process, 0}};
    for (const auto &[type, stream] : order) {
        const auto result = connection.dequeue();
        CHECK(result.id.type == type);
        CHECK(result.id.stream_index == stream);
        CHECK(result.error_code == S_OK);
    }
    Result none{};
    CHECK(xapu().dequeue_result(connection.handle, &none) == pending_results);
    CHECK(xapu().get_result_count(connection.handle, nullptr) == 0);
    CHECK(xapu().get_result_count(connection.handle, &dequeued) == 0);
    CHECK(dequeued == 4);
}

TEST_CASE("commands a stream cannot carry out fail at once or in their result") {
    const auto stereo = encode(2, 1);
    const Connection connection(0);
    // not a command, or not a stream of the connection
    for (const std::uint32_t type : {0u, 6u}) {
        const CommandId command{type, 0, 0, 0, nullptr};
        CHECK(connection.enqueue(command) == invalid_command);
    }
    CHECK(connection.enqueue(connection.activation(256, 2)) == invalid_stream_index);
    // a stream that is not active
    CHECK(connection.run(connection.processing(7, stereo[0])).error_code == invalid_stream_index);
    // one or two channels
    for (const std::uint32_t channels : {0u, 3u})
        CHECK(connection.run(connection.activation(7, channels)).error_code == E_INVALIDARG);
    CHECK(connection.run(connection.activation(7, 2)).error_code == S_OK);
    CHECK(connection.run(connection.activation(7, 2)).error_code == already_active);
    const CommandId notify_command{notify, 7, 0, 0, nullptr};
    CHECK(connection.run(notify_command).error_code == S_OK);
    // no packet, or no output
    auto command = connection.processing(7, stereo[0]);
    command.input_data_length = 0;
    CHECK(connection.run(command).error_code == E_INVALIDARG);
    command = connection.processing(7, stereo[0]);
    command.input_data = nullptr;
    CHECK(connection.run(command).error_code == E_INVALIDARG);
    command = connection.processing(7, stereo[0]);
    command.output_data = nullptr;
    CHECK(connection.run(command).error_code == E_INVALIDARG);
    // a deactivated stream takes no packet until it is activated again
    const CommandId deactivate_command{deactivate, 7, 0, 0, nullptr};
    CHECK(connection.run(deactivate_command).error_code == S_OK);
    CHECK(connection.run(connection.processing(7, stereo[0])).error_code == invalid_stream_index);
    CHECK(connection.run(connection.activation(7, 2)).error_code == S_OK);
    const auto result = connection.run(connection.processing(7, stereo[0]));
    CHECK(result.error_code == S_OK);
    CHECK(output_of(result, 2) == reference(stereo, 2)[0]);
}

TEST_CASE("a command that asks for it sets the result ready event") {
    const Connection connection(enable_result_ready_event);
    const auto event = connection.output.result_ready_event;
    REQUIRE(event);
    REQUIRE(connection.run(connection.activation(0, 2)).error_code == S_OK);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    const CommandId command{notify, 0, 0, signal_on_completion, nullptr};
    REQUIRE(connection.enqueue(command) == S_OK);
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    connection.dequeue();
    // without the connect option, there is no event to set
    const Connection quiet(0);
    CHECK(quiet.output.result_ready_event == nullptr);
    CHECK(quiet.run(command).error_code == S_OK);
}

TEST_CASE("XApuConnect takes what xapu.h allows, and null pointers are refused") {
    void *handle = nullptr;
    ConnectOutput output{};
    const ConnectInput valid{decode_opus, 256, 2, 0x104000, 0, 0};
    CHECK(xapu().connect(nullptr, &output, &handle) == E_INVALIDARG);
    CHECK(xapu().connect(&valid, &output, nullptr) == E_INVALIDARG);
    CHECK(output.base_data == nullptr);
    CHECK(xapu().connect(&valid, nullptr, &handle) == E_INVALIDARG);
    const ConnectInput refused[] = {
        {0, 256, 2, 0x104000, 0, 0},           {4, 256, 2, 0x104000, 0, 0},
        {decode_opus, 0, 2, 0x104000, 0, 0},   {decode_opus, 501, 2, 0x104000, 0, 0},
        {decode_opus, 256, 0, 0x104000, 0, 0}, {decode_opus, 256, 17, 0x104000, 0, 0},
        {decode_opus, 256, 2, 0, 0, 0},        {decode_opus, 256, 2, 0x104000, 960, 0},
        {convert_pcm, 256, 2, 0x104000, 0, 0}, {convert_pcm, 256, 2, 0x104000, 500, 0}};
    for (const auto &input : refused) {
        output = {&output, &output};
        CHECK(xapu().connect(&input, &output, &handle) == E_INVALIDARG);
        CHECK(output.base_data == nullptr);
        CHECK(output.result_ready_event == nullptr);
    }
    const ConnectInput largest{decode_opus, 500, 16, 0x1000, 0, 0};
    REQUIRE(xapu().connect(&largest, &output, &handle) == S_OK);
    const CommandId command{notify, 499, 0, 0, nullptr};
    for (int i = 0; i < 16; ++i)
        REQUIRE(xapu().enqueue_command(handle, &command, nullptr) == S_OK);
    CHECK(xapu().enqueue_command(handle, &command, nullptr) == queue_full);
    // disconnecting with results left frees everything and says they were left
    CHECK(xapu().disconnect(handle) == pending_results);

    Result result{};
    CHECK(xapu().disconnect(nullptr) == E_INVALIDARG);
    CHECK(xapu().enqueue_command(nullptr, &command, nullptr) == E_INVALIDARG);
    CHECK(xapu().dequeue_result(nullptr, &result) == E_INVALIDARG);
    CHECK(xapu().get_result_count(nullptr, nullptr) == 0);
    REQUIRE(xapu().connect(&valid, &output, &handle) == S_OK);
    CHECK(xapu().enqueue_command(handle, nullptr, nullptr) == E_INVALIDARG);
    CHECK(xapu().dequeue_result(handle, nullptr) == E_INVALIDARG);
    CHECK(xapu().disconnect(handle) == S_OK);

    std::uint32_t utilization[2] = {};
    CHECK(xapu().get_utilization(decode_opus, 2, utilization) == S_OK);
    CHECK(utilization[0] > 0);
    CHECK(utilization[1] >= utilization[0]);
    CHECK(xapu().get_utilization(decode_opus, 2, nullptr) == E_INVALIDARG);
}

TEST_CASE("a rate conversion client gets an output's length of silence for each Process") {
    const Connection connection(0, convert_pcm, 480);
    REQUIRE(connection.run(connection.activation(0, 2)).error_code == S_OK);
    std::fill_n(connection.output_of(0), 480 * 2, 1.0f);
    std::fill_n(reinterpret_cast<float *>(connection.input(0)), 480 * 2, 0.25f);
    const ProcessCommand command{
        {process, 0, 0, 0, nullptr}, 0.0f, 0.5f, 0, 480, 480 * 8, connection.input(0), 480 * 8,
        connection.output_of(0)};
    const auto result = connection.run(command);
    CHECK(result.error_code == S_OK);
    CHECK(result.state == (output_ready | need_more_input));
    CHECK(result.output_frame_count == 480);
    CHECK(result.output_data_length == 480 * 8);
    CHECK(result.current_pitch == 0.5f);
    CHECK(output_of(result, 2) == Pcm(480 * 2, 0.0f));
}
