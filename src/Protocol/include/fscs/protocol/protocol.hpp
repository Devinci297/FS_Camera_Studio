#pragma once
// Panel <-> module protocol v2 (doc/protocol.md §3-§4): envelopes, acknowledgements, events and CommBus chunking.
// Portable and exception-free.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fscs::protocol {

using json = nlohmann::json;

constexpr int kProtocolVersion = 2;
constexpr std::size_t kChunkChars = 4000;
constexpr std::size_t kMaxMessageBytes = 2u << 20;

enum class AckStatus { Accepted, Completed, Rejected, Cancelled, TimedOut, Pending };
std::string_view toString(AckStatus s);

struct Command {
    int protocolVersion = kProtocolVersion;
    std::string requestId;
    std::string uiInstanceId;
    std::string moduleBootId;
    std::uint64_t sessionEpoch = 0;
    std::int64_t draftRevision = -1;  // -1 when not supplied
    std::string action;
    json payload = json::object();

    json toJson() const;
    // nullopt (with a reason) when the document is not a well-formed v2 command.
    static std::optional<Command> fromJson(const json& j, std::string* error = nullptr);
};

json makeAck(const Command& c, AckStatus status, std::string_view reason = {}, json result = nullptr);
json makeEvent(std::string_view name, std::uint64_t seq, const std::string& bootId, std::uint64_t epoch, json payload,
               const std::string& to = {});

// Splits `text` into CommBus-sized pieces: the text itself when short, else chunk envelopes.
std::vector<std::string> chunk(const std::string& text, const std::string& msgId);

// Reassembles chunked messages; whole messages pass straight through. Time is supplied by the caller.
class Reassembler {
public:
    // Returns the complete message text when `piece` completes one.
    std::optional<std::string> feed(const std::string& piece, double now);
    void expire(double now);  // drops incomplete sets older than 5 s

private:
    struct Partial {
        std::vector<std::string> parts;
        std::size_t received = 0;
        std::size_t bytes = 0;
        double started = 0;
    };
    std::map<std::string, Partial> partial_;
};

}  // namespace fscs::protocol
