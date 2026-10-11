#include "fscs/profiles/repository.hpp"

#include "fscs/profiles/bindings.hpp"
#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <memory>

namespace fscs::profiles {

namespace {

constexpr const char* kFormat = "fs-camera-studio-store";
constexpr int kFormatVersion = 1;
constexpr std::size_t kRecentRequestIds = 64;

std::string hex32(std::uint32_t v) {
    static const char* h = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i) {
        s[i] = h[v & 15];
        v >>= 4;
    }
    return s;
}

}  // namespace

Repository::Repository(IStorageDriver& driver, std::string slotA, std::string slotB) : driver_(driver) {
    slots_[0] = std::move(slotA);
    slots_[1] = std::move(slotB);
    state_.settings = defaultSettingsDocument();
    state_.bindings = defaultBindingsDocument();
}

std::string Repository::serialize(const StoreState& s, const std::vector<std::string>& recent) {
    json profiles = json::array();
    for (const auto& [id, rec] : s.profiles) profiles.push_back({{"current", rec.current}, {"backups", rec.backups}});
    json payload{{"txn", s.txn},
                 {"profiles", profiles},
                 {"settings", s.settings},
                 {"bindings", s.bindings},
                 {"navigation", s.navigation},
                 {"recentRequestIds", recent}};
    const std::string body = js::dump(payload);
    // The checksum covers the exact payload text, so any torn or altered write fails verification.
    return std::string("{\"format\":\"") + kFormat + "\",\"version\":" + std::to_string(kFormatVersion) +
           ",\"crc\":\"" + hex32(js::crc32(body)) + "\",\"payload\":" + body + "}";
}

std::optional<StoreState> Repository::deserialize(const std::string& bytes, std::vector<std::string>* notes,
                                                  std::vector<std::string>* recent) {
    if (bytes.size() > kMaxStoreBytes || !js::validUtf8(bytes)) return std::nullopt;
    // Locate the payload text exactly as written so the checksum is computed over the same bytes.
    const std::string marker = ",\"payload\":";
    const auto at = bytes.find(marker);
    if (at == std::string::npos || bytes.empty() || bytes.back() != '}') return std::nullopt;
    const std::string body = bytes.substr(at + marker.size(), bytes.size() - (at + marker.size()) - 1);
    const json head = js::parse(bytes.substr(0, at) + "}");
    if (head.is_discarded() || js::str(head, "format") != kFormat || js::integer(head, "version", 0) != kFormatVersion) return std::nullopt;
    if (js::str(head, "crc") != hex32(js::crc32(body))) return std::nullopt;
    json payload = js::parse(body);
    if (payload.is_discarded() || !payload.is_object()) return std::nullopt;

    StoreState s;
    s.txn = js::integer(payload, "txn", -1);
    if (s.txn < 1) return std::nullopt;
    for (const auto& rec : js::array(payload, "profiles")) {
        json current = js::get(rec, "current");
        auto v = validateProfile(current);
        if (!v.ok()) {
            if (notes) notes->push_back("A saved profile could not be read and was skipped: " + v.summary());
            continue;
        }
        ProfileRecord r;
        r.current = std::move(current);
        for (const auto& b : js::array(rec, "backups")) {
            json copy = b;
            if (validateProfile(copy).ok() && r.backups.size() < kBackupsPerProfile) r.backups.push_back(std::move(copy));
        }
        s.profiles[js::str(r.current, "profileId")] = std::move(r);
    }
    s.settings = normalizeSettings(js::get(payload, "settings"));
    const json& bindings = js::get(payload, "bindings");
    s.bindings = parseBindings(bindings).problems.empty() ? bindings : defaultBindingsDocument();
    if (!parseBindings(bindings).problems.empty() && notes) notes->push_back("Saved bindings were damaged; defaults were restored.");
    s.navigation = js::object(payload, "navigation");
    if (recent)
        for (const auto& id : js::array(payload, "recentRequestIds"))
            if (id.is_string()) recent->push_back(id.get<std::string>());
    return s;
}

void Repository::load(LoadDone done) {
    struct Ctx {
        std::optional<std::string> data[2];
        int pending = 2;
    };
    auto ctx = std::make_shared<Ctx>();
    auto finishLoad = [this, ctx, done]() {
        std::vector<std::string> notes;
        std::optional<StoreState> best;
        std::vector<std::string> bestRecent;
        int bestSlot = -1;
        bool anyFile = false;
        for (int i = 0; i < 2; ++i) {
            if (!ctx->data[i]) continue;
            anyFile = true;
            std::vector<std::string> slotNotes, recent;
            auto s = deserialize(*ctx->data[i], &slotNotes, &recent);
            if (!s) continue;
            if (!best || s->txn > best->txn) {
                best = std::move(s);
                bestRecent = std::move(recent);
                bestSlot = i;
                notes = std::move(slotNotes);
            }
        }
        if (best) {
            state_ = std::move(*best);
            recentRequestIds_ = std::move(bestRecent);
            currentSlot_ = bestSlot;
            const int other = 1 - bestSlot;
            if (ctx->data[other] && !deserialize(*ctx->data[other], nullptr, nullptr))
                notes.push_back("An interrupted save was found and ignored; the last complete save was opened.");
        } else if (anyFile) {
            notes.push_back("Saved Camera Studio data could not be read. Starting with an empty library; the damaged files are kept until the next save.");
        }
        loaded_ = true;
        done(std::move(notes));
        startNext();
    };
    for (int i = 0; i < 2; ++i)
        driver_.readFile(slots_[i], [ctx, i, finishLoad](std::optional<std::string> bytes) {
            ctx->data[i] = std::move(bytes);
            if (--ctx->pending == 0) finishLoad();
        });
}

void Repository::commit(CommitRequest req, CommitDone done) {
    queue_.push_back({std::move(req), std::move(done)});
    if (loaded_) startNext();
}

void Repository::startNext() {
    if (active_ || queue_.empty() || !loaded_) return;
    active_ = true;
    Pending& p = queue_.front();
    const CommitRequest& req = p.req;

    if (!req.requestId.empty() &&
        std::find(recentRequestIds_.begin(), recentRequestIds_.end(), req.requestId) != recentRequestIds_.end()) {
        finish({CommitStatus::Committed, "Already saved.", state_.txn, true});
        return;
    }

    StoreState next = state_;
    next.txn = state_.txn + 1;
    for (json doc : req.upsertProfiles) {
        auto v = validateProfile(doc);
        if (!v.ok()) {
            finish({CommitStatus::Failed, "The profile was not saved because it has problems: " + v.summary(), state_.txn});
            return;
        }
        const std::string id = js::str(doc, "profileId");
        auto it = next.profiles.find(id);
        if (it != next.profiles.end()) {
            doc["revision"] = js::integer(it->second.current, "revision", 0) + 1;
            if (req.backup) {
                it->second.backups.insert(it->second.backups.begin(), it->second.current);
                if (it->second.backups.size() > kBackupsPerProfile) it->second.backups.resize(kBackupsPerProfile);
            }
            it->second.current = std::move(doc);
        } else {
            doc["revision"] = 1;
            next.profiles[id] = ProfileRecord{std::move(doc), {}};
        }
    }
    for (const auto& id : req.removeProfiles) next.profiles.erase(id);
    if (req.settings) next.settings = normalizeSettings(*req.settings);
    if (req.bindings) {
        auto parsed = parseBindings(*req.bindings);
        if (!parsed.problems.empty()) {
            finish({CommitStatus::Failed, "Bindings were not saved: " + parsed.problems.front(), state_.txn});
            return;
        }
        next.bindings = *req.bindings;
    }
    if (req.navigation) next.navigation = *req.navigation;

    std::vector<std::string> recent = recentRequestIds_;
    if (!req.requestId.empty()) {
        recent.push_back(req.requestId);
        if (recent.size() > kRecentRequestIds) recent.erase(recent.begin());
    }
    std::string bytes = serialize(next, recent);
    if (bytes.size() > kMaxStoreBytes) {
        finish({CommitStatus::Failed, "Camera Studio's library is full (more than 24 MiB). Delete unused profiles and try again.", state_.txn});
        return;
    }
    const int target = currentSlot_ == 0 ? 1 : 0;  // never overwrite the last valid save
    auto shared = std::make_shared<StoreState>(std::move(next));
    auto expected = std::make_shared<std::string>(bytes);
    driver_.writeFile(slots_[target], std::move(bytes), [this, target, shared, expected, recent](bool ok, std::string err) {
        if (!ok) {
            finish({CommitStatus::Failed, "Your changes were not saved. " + (err.empty() ? std::string("The write failed.") : err), state_.txn});
            return;
        }
        driver_.readFile(slots_[target], [this, target, shared, expected, recent](std::optional<std::string> back) {
            if (!back || *back != *expected || !deserialize(*back, nullptr, nullptr)) {
                finish({CommitStatus::Failed, "Your changes were not saved: the save could not be verified.", state_.txn});
                return;
            }
            state_ = std::move(*shared);
            recentRequestIds_ = recent;
            currentSlot_ = target;
            finish({CommitStatus::Committed, "Saved.", state_.txn});
        });
    });
}

void Repository::finish(CommitResult r) {
    Pending p = std::move(queue_.front());
    queue_.pop_front();
    active_ = false;
    if (p.done) p.done(r);
    startNext();
}

std::string exportProfileCode(const json& profile, bool workingCopy) {
    json envelope{{"format", "fs-camera-studio-profile"}, {"version", 2}, {"workingCopy", workingCopy}, {"profile", profile}};
    return envelope.dump(2, ' ', false, json::error_handler_t::replace);
}

std::optional<ImportReport> importProfileCode(const std::string& code, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<ImportReport> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (code.size() > kMaxProfileBytes) return fail("The profile code is larger than 10 MiB.");
    if (!js::validUtf8(code)) return fail("The profile code is not valid text.");
    json doc = js::parse(code);
    if (doc.is_discarded()) return fail("This is not a Camera Studio profile code (it is not valid JSON).");
    json profile = doc;
    if (js::str(doc, "format") == "fs-camera-studio-profile") profile = js::get(doc, "profile");
    if (!profile.is_object()) return fail("This is not a Camera Studio profile code.");
    auto v = validateProfile(profile);
    if (!v.ok()) return fail("This profile cannot be imported: " + v.summary());
    ImportReport report;
    reassignIds(profile);  // a copy: never collides with or overwrites an existing profile
    report.cameraCount = js::array(profile, "cameras").size();
    for (const auto& cam : js::array(profile, "cameras")) {
        auto mode = parseMode(js::str(cam, "mode"));
        if (!mode || !modeActivatable(*mode)) report.unsupportedCount++;
    }
    if (report.unsupportedCount)
        report.notes.push_back(std::to_string(report.unsupportedCount) +
                               " camera(s) use view types this version cannot show; they stay visible but cannot be activated.");
    if (js::str(js::object(profile, "calibration"), "adapterContract") != "pending-qualification")
        report.notes.push_back("Its calibration was recorded on another setup; check the views before relying on them.");
    if (v.readOnly) report.notes.push_back("It needs a newer version of Camera Studio and opens read-only.");
    if (v.migrated) report.notes.push_back("It was made with an older version and has been upgraded.");
    report.profile = std::move(profile);
    return report;
}

}  // namespace fscs::profiles
