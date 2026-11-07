#include "fscs/protocol/protocol.hpp"

#include "fscs/profiles/json_util.hpp"

namespace fscs::protocol {

std::string_view toString(AckStatus s) {
    switch (s) {
        case AckStatus::Accepted: return "Accepted";
        case AckStatus::Completed: return "Completed";
        case AckStatus::Rejected: return "Rejected";
        case AckStatus::Cancelled: return "Cancelled";
        case AckStatus::TimedOut: return "TimedOut";
        case AckStatus::Pending: return "Pending";
    }
    return "Rejected";
}

json Command::toJson() const {
    json j{{"protocolVersion", protocolVersion}, {"requestId", requestId}, {"uiInstanceId", uiInstanceId},
           {"moduleBootId", moduleBootId},       {"sessionEpoch", sessionEpoch}, {"action", action},
           {"payload", payload}};
    if (draftRevision >= 0) j["draftRevision"] = draftRevision;
    return j;
}

std::optional<Command> Command::fromJson(const json& j, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<Command> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!j.is_object()) return fail("command is not an object");
    if (js::integer(j, "protocolVersion", -1) != kProtocolVersion) return fail("protocolMismatch");
    Command c;
    c.action = js::str(j, "action");
    c.requestId = js::str(j, "requestId");
    c.uiInstanceId = js::str(j, "uiInstanceId");
    c.moduleBootId = js::str(j, "moduleBootId");
    c.sessionEpoch = static_cast<std::uint64_t>(std::max<std::int64_t>(0, js::integer(j, "sessionEpoch", 0)));
    c.draftRevision = js::integer(j, "draftRevision", -1);
    if (c.action.empty() || c.action.size() > 64) return fail("missing action");
    if (c.requestId.size() > 96 || c.uiInstanceId.size() > 96 || c.moduleBootId.size() > 96) return fail("identifier too long");
    const json& payload = js::get(j, "payload");
    if (payload.is_object()) c.payload = payload;
    else if (!payload.is_null()) return fail("payload is not an object");
    return c;
}

json makeAck(const Command& c, AckStatus status, std::string_view reason, json result) {
    return {{"type", "ack"},
            {"to", c.uiInstanceId},
            {"requestId", c.requestId},
            {"status", std::string(toString(status))},
            {"reason", reason.empty() ? json(nullptr) : json(std::string(reason))},
            {"result", std::move(result)}};
}

json makeEvent(std::string_view name, std::uint64_t seq, const std::string& bootId, std::uint64_t epoch, json payload,
               const std::string& to) {
    json e{{"type", "event"}, {"name", std::string(name)}, {"seq", seq}, {"moduleBootId", bootId},
           {"sessionEpoch", epoch}, {"payload", std::move(payload)}};
    if (!to.empty()) e["to"] = to;
    return e;
}

std::vector<std::string> chunk(const std::string& text, const std::string& msgId) {
    if (text.size() <= kChunkChars) return {text};
    std::vector<std::string> pieces;
    const std::string id=js::dump(json(msgId));
    std::size_t pos=0;
    while(pos<text.size()) {
        std::size_t length=std::min<std::size_t>(3000,text.size()-pos);
        std::string escaped;
        for(;;) {
            auto end=pos+length;
            while(end<text.size() && end>pos && (static_cast<unsigned char>(text[end])&0xC0)==0x80)--end;
            escaped=js::dump(json(text.substr(pos,end-pos)));
            // Include JSON escaping and the envelope, not just raw payload bytes.
            if(escaped.size()+id.size()+80<=kChunkChars || length<4) {pos=end;break;}
            length=length*4/5;
        }
        pieces.push_back(std::move(escaped));
    }
    std::vector<std::string> out;
    for(std::size_t i=0;i<pieces.size();++i)
        out.push_back("{\"part\":"+std::to_string(i)+",\"of\":"+std::to_string(pieces.size())+",\"msgId\":"+id+",\"chunk\":"+pieces[i]+"}");
    return out;
}

std::optional<std::string> Reassembler::feed(const std::string& piece, double now) {
    if (piece.size() > kMaxMessageBytes) return std::nullopt;
    // Cheap check before parsing large whole messages.
    if (piece.compare(0, 8, "{\"part\":") != 0) return piece;
    json j = js::parse(piece);
    if (j.is_discarded()) return std::nullopt;
    const std::string id = js::str(j, "msgId");
    const auto part = js::integer(j, "part", -1), of = js::integer(j, "of", -1);
    const json& chunkText = js::get(j, "chunk");
    if (id.empty() || id.size() > 96 || of < 1 || of > 8192 || part < 0 || part >= of || !chunkText.is_string()) return std::nullopt;
    auto& p = partial_[id];
    if (p.parts.empty()) {
        p.parts.resize(static_cast<std::size_t>(of));
        p.started = now;
    }
    if (p.parts.size() != static_cast<std::size_t>(of)) {
        partial_.erase(id);
        return std::nullopt;
    }
    auto& slot = p.parts[static_cast<std::size_t>(part)];
    if (slot.empty()) {
        slot = chunkText.get<std::string>();
        p.received++;
        p.bytes += slot.size();
    }
    if (p.bytes > kMaxMessageBytes) {
        partial_.erase(id);
        return std::nullopt;
    }
    if (p.received < p.parts.size()) return std::nullopt;
    std::string whole;
    for (auto& s : p.parts) whole += s;
    partial_.erase(id);
    return whole;
}

void Reassembler::expire(double now) {
    for (auto it = partial_.begin(); it != partial_.end();) it = now - it->second.started > 5.0 ? partial_.erase(it) : std::next(it);
}

}  // namespace fscs::protocol
