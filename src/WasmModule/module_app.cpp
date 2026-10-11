#include "module_app.hpp"

#include "fscs/profiles/bindings.hpp"
#include "fscs/profiles/json_util.hpp"

#include <algorithm>

namespace fscs::host {

using profiles::CommitRequest;
using profiles::CommitResult;
using profiles::CommitStatus;
using protocol::AckStatus;
using protocol::Command;

namespace {

constexpr const char* kSlotA = "\\work\\fscs-store-a.json";
constexpr const char* kSlotB = "\\work\\fscs-store-b.json";

// Latest-state events: only the newest payload matters, so they are coalesced and flushed at <= 10 Hz.
bool isStateEvent(const std::string& n) {
    return n == "SessionChanged" || n == "AircraftChanged" || n == "OwnershipChanged" || n == "CapabilitiesChanged" ||
           n == "ActiveCameraChanged" || n == "SelectionChanged" || n == "TemporaryLookChanged" || n == "ProfilesChanged";
}

// Pinning the mini panel is a global UI preference, saved on its own and never an unsaved edit (design 2.0 §4.3).
json withoutPreferences(json settings) {
    if (settings.is_object() && settings.contains("ui") && settings["ui"].is_object()) settings["ui"].erase("miniPinned");
    return settings;
}

json withoutRevision(json doc) {
    if (doc.is_object()) doc.erase("revision");
    return doc;
}

core::ManualInput add(core::ManualInput a, const core::ManualInput& b) {
    a.lookRight += b.lookRight;
    a.lookUp += b.lookUp;
    a.moveForward += b.moveForward;
    a.moveRight += b.moveRight;
    a.moveUp += b.moveUp;
    a.zoomIn += b.zoomIn;
    return a;
}

core::ManualInput manualFromJson(const json& j) {
    core::ManualInput m;
    m.lookRight = js::num(j, "lookRight", 0);
    m.lookUp = js::num(j, "lookUp", 0);
    m.moveForward = js::num(j, "moveForward", 0);
    m.moveRight = js::num(j, "moveRight", 0);
    m.moveUp = js::num(j, "moveUp", 0);
    m.zoomIn = js::num(j, "zoomIn", 0);
    return m;
}

}  // namespace

ModuleApp::ModuleApp(ICameraBackend& camera, IModuleServices& services, profiles::IStorageDriver& storage, std::string bootId,
                     std::string appVersion)
    : services_(services),
      repo_(storage, kSlotA, kSlotB),
      coord_(camera, [this](const std::string& name, json payload) { onCoordinatorEvent(name, std::move(payload)); },
             [this](const std::string& v) { return qualification(v); }),
      bootId_(std::move(bootId)),
      appVersion_(std::move(appVersion)) {
    working_.settings = profiles::defaultSettingsDocument();
    working_.bindings = profiles::defaultBindingsDocument();
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle

void ModuleApp::start(double now) {
    now_ = now;
    diag("module " + appVersion_ + " boot " + bootId_);
    repo_.load([this](std::vector<std::string> notes) { loadFromRepository(std::move(notes), now_); });
}

void ModuleApp::shutdown(double now) {
    for (const auto& e : router_.releaseAll()) coord_.onAction(e, now);
    coord_.release("Camera Studio is shutting down.", now);
}

void ModuleApp::loadFromRepository(std::vector<std::string> notes, double now) {
    const auto& st = repo_.committed();
    working_.settings = st.settings;
    working_.bindings = st.bindings;
    lastCamera_ = js::object(st.navigation, "lastCamera");
    for (const auto& n : notes) {
        diag("storage: " + n);
        emitNow("FaultRaised", {{"code", "storage"}, {"message", n}});
    }
    const std::string last = js::str(st.navigation, "lastProfileId");
    if (st.profiles.count(last)) openProfile(last, now);
    else if (!st.profiles.empty()) openProfile(st.profiles.begin()->first, now);
    else publishWorking(now, true);
    resolveAircraft_ = true;
    broadcastSnapshot();
}

void ModuleApp::update(double now) {
    now_ = now;
    reassembler_.expire(now);
    if (resolveAircraft_ && repo_.loaded()) {
        resolveAircraft_ = false;
        resolveAircraftProfile(now);
    }
    // UI gesture leases: expiry neutralises motion only, never releases the camera.
    for (auto it = gestures_.begin(); it != gestures_.end();) it = now > it->second.expires ? gestures_.erase(it) : std::next(it);
    core::ManualInput manual = router_.manualInput();
    for (const auto& [token, g] : gestures_) manual = add(manual, g.input);
    coord_.setManualInput(manual);

    if (!testDefinition_.empty() && now > testUntil_) {
        services_.mapTestInput("");
        testDefinition_.clear();
    }
    for (auto& [id, inst] : instances_)
        if (!inst.lapsed && now - inst.lastRenew > kLeaseSeconds) inst.lapsed = true;

    coord_.tick(now);
    flushState(now, false);
}

void ModuleApp::onEvent(const BackendEvent& ev, double now) {
    now_ = now;
    if (const auto* m = std::get_if<backend_event::CommBusMessage>(&ev)) {
        onPanelText(m->data, now);
        return;
    }
    if (const auto* in = std::get_if<backend_event::InputEvent>(&ev)) {
        if (in->test) {
            if (!testDefinition_.empty()) emitNow("InputTest", {{"definition", testDefinition_}, {"phase", in->down ? "down" : "up"}, {"value", in->value}});
            return;
        }
        for (const auto& a : router_.onInput(in->index, in->down, in->value)) coord_.onAction(a, now);
        return;
    }
    if (const auto* c = std::get_if<backend_event::ControllersList>(&ev)) {
        controllers_ = c->controllers;
        json list = json::array();
        for (const auto& [id, name] : controllers_) list.push_back({{"deviceId", id}, {"name", name}});
        emitNow("ControllersListed", {{"controllers", list}});
        return;
    }
    if (const auto* p = std::get_if<backend_event::Paused>(&ev)) {
        const bool resumed = paused_ && !p->paused;
        paused_ = p->paused;
        // JS does not process events while paused; resume every surface from one fresh snapshot.
        if (resumed) broadcastSnapshot();
    }
    if (const auto* r = std::get_if<backend_event::SimRunning>(&ev)) simRunning_ = r->running;
    if (std::holds_alternative<backend_event::FlightLoaded>(ev) || std::holds_alternative<backend_event::Disconnected>(ev))
        for (const auto& a : router_.releaseAll()) coord_.onAction(a, now);
    coord_.onBackendEvent(ev, now);
}

// ---------------------------------------------------------------------------------------------------------------
// Panel transport

void ModuleApp::send(const json& message) {
    const std::string text = js::dump(message);
    for (const auto& piece : protocol::chunk(text, "m" + bootId_.substr(0, 8) + "-" + std::to_string(++msgCounter_)))
        services_.sendToPanels(piece);
}

bool ModuleApp::anyVisibleLease() const {
    return std::any_of(instances_.begin(), instances_.end(), [](const auto& kv) { return kv.second.visible && !kv.second.lapsed; });
}

void ModuleApp::emitState(const std::string& name, json payload) { pendingState_[name] = std::move(payload); }

void ModuleApp::emitNow(const std::string& name, json payload, const std::string& to) {
    send(protocol::makeEvent(name, ++seq_, bootId_, coord_.epoch(), std::move(payload), to));
}

void ModuleApp::flushState(double now, bool force) {
    if (pendingState_.empty()) return;
    if (!force && (paused_ || !anyVisibleLease() || now - lastFlush_ < kStateFlushSeconds)) return;
    lastFlush_ = now;
    auto pending = std::move(pendingState_);
    pendingState_.clear();
    for (auto& [name, payload] : pending) send(protocol::makeEvent(name, ++seq_, bootId_, coord_.epoch(), std::move(payload)));
}

void ModuleApp::broadcastSnapshot(const std::string& to) {
    pendingState_.clear();  // the snapshot supersedes any coalesced state
    emitNow("Snapshot", snapshot(), to);
}

void ModuleApp::reply(const Command& c, AckStatus s, const std::string& reason, json result) {
    send(protocol::makeAck(c, s, reason, std::move(result)));
}

json ModuleApp::snapshot() const {
    json hasCommitted = json::object();
    const json committed = working_.profile.is_object() ? committedProfile(js::str(working_.profile, "profileId")) : json(nullptr);
    if (working_.profile.is_object())
        for (const auto& cam : js::array(working_.profile, "cameras")) {
            bool found = false;
            for (const auto& c : js::array(committed, "cameras")) found = found || js::str(c, "id") == js::str(cam, "id");
            hasCommitted[js::str(cam, "id")] = found;
        }
    const char* session = !repo_.loaded() ? "Initializing" : coord_.blockReason().find("not running") != std::string::npos ? "Discovering"
                          : !coord_.aircraft().known() ? "Discovering" : !simRunning_ ? "Suspended" : "Ready";
    json active{{"profileId", coord_.profile() ? json(coord_.profile()->profileId) : json(nullptr)},
                {"cameraId", coord_.activeCameraId().empty() ? json(nullptr) : json(coord_.activeCameraId())},
                {"peeking", false},
                {"transitioning", coord_.engine().transitioning()}};
    return {{"moduleBootId", bootId_},
            {"sessionEpoch", coord_.epoch()},
            {"appVersion", appVersion_},
            {"session", session},
            {"aircraft", lastAircraft_},
            {"ownership", {{"state", toString(coord_.ownership())}, {"gameControlled", coord_.gameControlled()}, {"reason", coord_.stateReason()}}},
            {"capabilities", coord_.capabilitiesJson()},
            {"activeCamera", active},
            {"selection", {{"profileId", working_.profile.is_object() ? js::get(working_.profile, "profileId") : json(nullptr)},
                           {"cameraId", selection_.empty() ? json(nullptr) : json(selection_)}}},
            {"profiles", profilesList()},
            {"working", {{"profile", working_.profile},
                         {"settings", working_.settings},
                         {"bindings", working_.bindings},
                         {"draftRevision", working_.draftRevision},
                         {"dirty", {{"profile", working_.dirtyProfile}, {"settings", working_.dirtySettings}, {"bindings", working_.dirtyBindings}}},
                         {"committedRevision", js::get(committed, "revision")},
                         {"hasCommittedVersion", hasCommitted}}},
            {"pendingCommit", pendingCommit_ ? json(pendingCommit_->requestId) : json(nullptr)},
            {"controllers", [this] {
                 json list = json::array();
                 for (const auto& [id, name] : controllers_) list.push_back({{"deviceId", id}, {"name", name}});
                 return list;
             }()}};
}

void ModuleApp::onPanelText(const std::string& text, double now) {
    auto whole = reassembler_.feed(text, now);
    if (!whole) return;
    json j = js::parse(*whole);
    std::string why;
    auto cmd = Command::fromJson(j, &why);
    if (!cmd) {
        if (j.is_object()) {
            Command fake;
            fake.requestId = js::str(j, "requestId");
            fake.uiInstanceId = js::str(j, "uiInstanceId");
            reply(fake, AckStatus::Rejected, why);
        }
        return;
    }
    handle(*cmd, now);
}

// ---------------------------------------------------------------------------------------------------------------
// Commands

json ModuleApp::forward(const Command& c, double now) { return coord_.handle(c, now); }

void ModuleApp::handle(const Command& c, double now) {
    const std::string& a = c.action;
    const json& p = c.payload;

    if (a == "ReleaseCamera") {  // priority: always, regardless of boot/epoch/state
        coord_.release("Camera released", now);
        reply(c, AckStatus::Completed);
        return;
    }
    if (a == "Hello") {
        instances_[c.uiInstanceId] = {js::str(p, "surface", "main"), now, true, false};
        reply(c, AckStatus::Completed, {}, {{"moduleBootId", bootId_}});
        emitNow("Snapshot", snapshot(), c.uiInstanceId);
        return;
    }
    if (a == "RequestSnapshot") {
        if (auto it = instances_.find(c.uiInstanceId); it != instances_.end()) {
            it->second.lastRenew = now;
            it->second.lapsed = false;
        }
        reply(c, AckStatus::Completed);
        emitNow("Snapshot", snapshot(), c.uiInstanceId);
        return;
    }
    if (!c.moduleBootId.empty() && c.moduleBootId != bootId_) {
        reply(c, AckStatus::Rejected, "staleBoot");
        return;
    }
    if (a == "RenewLease") {
        auto& inst = instances_[c.uiInstanceId];
        const bool wasHidden = inst.lapsed || !inst.visible;
        inst.lastRenew = now;
        inst.lapsed = false;
        inst.visible = js::boolean(p, "visible", true);
        if (inst.surface.empty()) inst.surface = "main";
        reply(c, AckStatus::Completed);
        if (wasHidden && inst.visible) emitNow("Snapshot", snapshot(), c.uiInstanceId);
        return;
    }
    if (!repo_.loaded()) {
        reply(c, AckStatus::Rejected, "Camera Studio is still starting.");
        return;
    }

    // Navigation and camera control -------------------------------------------------------------------------
    if (a == "SelectCamera") {
        const std::string id = js::str(p, "cameraId");
        if (!coord_.profile() || !coord_.profile()->camera(id)) return reply(c, AckStatus::Rejected, "That camera is not in the open profile.");
        selectCamera(id);
        return reply(c, AckStatus::Completed);
    }
    if (a == "ActivateCamera" || a == "EnableCamera") {
        Command fwd = c;
        fwd.action = "ActivateCamera";
        if (a == "EnableCamera") {
            const std::string target = selection_.empty() ? selectionFallback() : selection_;
            if (target.empty()) return reply(c, AckStatus::Rejected, "Select a view first.");
            fwd.payload = {{"cameraId", target}};
        }
        json ack = forward(fwd, now);
        if (js::str(ack, "status") == "Accepted") selectCamera(js::str(fwd.payload, "cameraId"));
        ack["to"] = c.uiInstanceId;
        ack["requestId"] = c.requestId;
        send(ack);
        return;
    }
    if (a == "BeginPeek" || a == "EndPeek" || a == "CycleCamera" || a == "NextCategory" || a == "ReturnToPilot" ||
        a == "RecenterLook" || a == "CancelMotion" || a == "BakeTemporaryLook" || a == "EnumerateDefinitions" ||
        a == "CaptureView" || a == "SeedFromDefinition") {
        if (a == "CaptureView" || a == "SeedFromDefinition") captureRequester_ = c.uiInstanceId;
        send(forward(c, now));
        return;
    }
    if (a == "BeginGesture" || a == "RenewGesture") {
        const std::string token = js::str(p, "gestureToken");
        if (token.empty()) return reply(c, AckStatus::Rejected, "missing gestureToken");
        auto& g = gestures_[token];
        if (js::has(p, "input")) g.input = manualFromJson(js::get(p, "input"));
        g.expires = now + kGestureLeaseSeconds;
        return reply(c, AckStatus::Completed);
    }
    if (a == "EndGesture") {
        gestures_.erase(js::str(p, "gestureToken"));
        return reply(c, AckStatus::Completed);
    }
    if (a == "TextFocus") {
        for (const auto& e : router_.setTextFocus(js::boolean(p, "focused", false))) coord_.onAction(e, now);
        return reply(c, AckStatus::Completed);
    }
    if (a == "TestBinding") {
        const json& b = js::get(p, "binding");
        auto action = profiles::parseAction(js::str(b, "action"));
        if (!action) return reply(c, AckStatus::Rejected, "Unknown action.");
        const int viewId = static_cast<int>(js::integer(b, "viewId", 0));
        coord_.onAction({*action, viewId, true, 0}, now);
        if (!profiles::actionIsContinuous(*action) && *action != profiles::Action::PeekView) coord_.onAction({*action, viewId, false, 0}, now);
        return reply(c, AckStatus::Completed, {}, {{"dispatched", true}, {"note", "Command sent. Watch the simulator view to confirm it moved."}});
    }
    if (a == "ListControllers") {
        if (!services_.enumerateControllers()) return reply(c, AckStatus::Rejected, "Controllers can be listed once a flight is loaded.");
        return reply(c, AckStatus::Accepted);
    }
    if (a == "TestInput") {
        auto spec = profiles::InputSpec::fromJson(js::get(p, "input"));
        if (!spec) return reply(c, AckStatus::Rejected, "That input is not one the simulator accepts.");
        if (!services_.mapTestInput(spec->definition)) return reply(c, AckStatus::Rejected, "Inputs can be tested once a flight is loaded.");
        testDefinition_ = spec->definition;
        testUntil_ = now + kInputTestSeconds;
        return reply(c, AckStatus::Accepted, {}, {{"seconds", kInputTestSeconds}});
    }
    if (a == "RelaySurface") {
        emitNow("SurfaceRequest", {{"target", js::str(p, "target", "main")}, {"op", js::str(p, "op", "expand")}, {"origin", c.uiInstanceId}});
        return reply(c, AckStatus::Completed);
    }
    if (a == "GetDiagnostics") {
        std::string text = "FS Camera Studio " + appVersion_ + "\nsimulator " + coord_.simVersion() + "\ncamera " +
                           toString(coord_.ownership()) + " (" + coord_.stateReason() + ")\n";
        for (const auto& l : diagnostics_) text += l + "\n";
        return reply(c, AckStatus::Completed, {}, {{"text", text}});
    }

    // In-panel axis check (gates G01 and G03) ------------------------------------------------------------------
    if (a == "QualificationStart") {
        coord_.setQualificationSession(true, now);
        return reply(c, AckStatus::Completed, {}, {{"simVersion", coord_.simVersion()},
                                                  {"contract", coord_.qualificationState().contract.toJson()},
                                                  {"axes", {"forward", "right", "up", "pitch", "heading", "bank"}}});
    }
    if (a == "QualificationProbe" || a == "QualificationRestore") {
        const auto* prof = coord_.profile();
        const auto* cam = prof ? prof->camera(coord_.activeCameraId()) : nullptr;
        if (!cam) return reply(c, AckStatus::Rejected, "Show a cockpit view in the simulator first.");
        core::CameraPose pose = cam->pose;
        std::string expect = "The view returned to the saved position.";
        if (a == "QualificationProbe") {
            const std::string axis = js::str(p, "axis");
            core::Pbh pbh = pose.orientation.toPbh();
            if (axis == "forward") { pose.positionFRD.x += 0.3; expect = "The view should move about 30 cm forward."; }
            else if (axis == "right") { pose.positionFRD.y += 0.3; expect = "The view should move about 30 cm to the right."; }
            else if (axis == "up") { pose.positionFRD.z -= 0.3; expect = "The view should move about 30 cm up."; }
            else if (axis == "pitch") { pbh.pitchDeg += 15; expect = "The view should tilt 15 degrees up."; }
            else if (axis == "heading") { pbh.headingDeg += 15; expect = "The view should turn 15 degrees to the right."; }
            else if (axis == "bank") { pbh.bankDeg += 15; expect = "The view should roll 15 degrees clockwise (right side down)."; }
            else return reply(c, AckStatus::Rejected, "Unknown axis.");
            pose.orientation = core::Quat::fromPbh(pbh);
        }
        if (!coord_.previewPose(pose)) return reply(c, AckStatus::Rejected, "Camera Studio must have camera control to run the check.");
        return reply(c, AckStatus::Completed, {}, {{"expect", expect}});
    }
    if (a == "QualificationRecord") {
        const json& ans = js::object(p, "answers");
        AdapterContract contract = coord_.qualificationState().contract;
        bool wrong = false, complete = true;
        auto apply = [&](const char* key, double& sign) {
            const std::string v = js::str(ans, key);
            if (v == "reversed") sign = -sign;
            else if (v == "wrong") wrong = true;
            else if (v != "correct") complete = false;
        };
        apply("forward", contract.axisSign[0]);
        apply("right", contract.axisSign[1]);
        apply("up", contract.axisSign[2]);
        apply("pitch", contract.pitchSign);
        apply("heading", contract.headingSign);
        apply("bank", contract.bankSign);
        if (!complete) return reply(c, AckStatus::Rejected, "Answer every step before recording.");
        contract.id = "msfs2024-wasm-measured-" + coord_.simVersion();
        const bool g01 = !wrong && contract.valid();
        const bool g03 = js::str(p, "recall") == "pass";
        json nav = navigation();
        json records = json::array();
        for (const auto& r : js::array(js::object(nav, "qualification"), "records"))
            if (js::str(r, "simVersion") != coord_.simVersion()) records.push_back(r);
        records.push_back({{"simVersion", coord_.simVersion()},
                           {"moduleVersion", appVersion_},
                           {"contract", contract.toJson()},
                           {"gates", {{"G01", g01 ? "pass" : "fail"}, {"G03", g03 ? "pass" : "fail"}, {"G02", "untested"}}},
                           {"notes", js::str(p, "notes").substr(0, 500)}});
        nav["qualification"] = {{"records", records}};
        CommitRequest req;
        req.navigation = nav;
        req.requestId = c.uiInstanceId + ":" + c.requestId;
        reply(c, AckStatus::Pending, {}, {{"G01", g01 ? "pass" : "fail"}, {"G03", g03 ? "pass" : "fail"}});
        commit(std::move(req), {c.requestId, "qualification", working_.draftRevision, {{"g01", g01}, {"g03", g03}}, {}}, now);
        return;
    }
    if (a == "QualificationEnd") {
        coord_.setQualificationSession(false, now);
        return reply(c, AckStatus::Completed);
    }

    // Drafts and persistence ---------------------------------------------------------------------------------
    if (a == "UpdateDraft") {
        if (c.draftRevision >= 0 && c.draftRevision != working_.draftRevision) return reply(c, AckStatus::Rejected, "staleDraft");
        json applied;
        std::string err;
        const json before = working_.bindings;
        if (!applyOps(js::get(p, "ops"), applied, err)) return reply(c, AckStatus::Rejected, err);
        publishWorking(now, before != working_.bindings);
        reply(c, AckStatus::Completed, {}, {{"draftRevision", working_.draftRevision}});
        const bool pinned = js::boolean(js::object(working_.settings, "ui"), "miniPinned", false);
        if (pinned != js::boolean(js::object(repo_.committed().settings, "ui"), "miniPinned", false)) {
            CommitRequest pref;
            json s = repo_.committed().settings;
            s["ui"]["miniPinned"] = pinned;
            pref.settings = s;
            pref.navigation = navigation();
            commit(std::move(pref), {c.requestId, "preference", working_.draftRevision, nullptr, {}}, now);
        }
        emitNow("DraftChanged", {{"draftRevision", working_.draftRevision},
                                 {"dirty", {{"profile", working_.dirtyProfile}, {"settings", working_.dirtySettings}, {"bindings", working_.dirtyBindings}}},
                                 {"ops", applied},
                                 {"origin", c.uiInstanceId}});
        return;
    }
    if (a == "RevertChanges") {
        if (pendingCommit_) return reply(c, AckStatus::Rejected, "A save is in progress.");
        const auto& st = repo_.committed();
        const json committed = working_.profile.is_object() ? committedProfile(js::str(working_.profile, "profileId")) : json(nullptr);
        working_.profile = committed;
        working_.settings = st.settings;
        working_.bindings = st.bindings;
        working_.draftRevision++;
        recomputeDirty();
        if (!selection_.empty()) {
            bool exists = false;
            for (const auto& cam : js::array(working_.profile, "cameras")) exists = exists || js::str(cam, "id") == selection_;
            if (!exists) selection_ = selectionFallback();
        }
        publishWorking(now, true);
        reply(c, AckStatus::Completed);
        broadcastSnapshot();
        return;
    }
    if (a == "CommitChanges") {
        if (!working_.profile.is_object() && !working_.dirtySettings && !working_.dirtyBindings)
            return reply(c, AckStatus::Rejected, "There is nothing to save.");
        CommitRequest req;
        if (working_.profile.is_object() && working_.dirtyProfile) req.upsertProfiles.push_back(working_.profile);
        req.settings = working_.settings;
        req.bindings = working_.bindings;
        req.navigation = navigation();
        req.backup = js::boolean(p, "backup", true);
        req.requestId = c.uiInstanceId + ":" + c.requestId;
        PendingCommit pc{c.requestId, "save", working_.draftRevision, nullptr, {}};
        reply(c, AckStatus::Pending, {}, {{"revisions", {{"draft", working_.draftRevision}}}});
        commit(std::move(req), std::move(pc), now);
        return;
    }
    if (a == "OpenProfile") {
        const std::string id = js::str(p, "profileId");
        if (!repo_.committed().profiles.count(id)) return reply(c, AckStatus::Rejected, "That profile does not exist.");
        if (pendingCommit_) return reply(c, AckStatus::Rejected, "A save is in progress.");
        if (working_.dirtyProfile && !js::boolean(p, "discard", false)) return reply(c, AckStatus::Rejected, "unsavedChanges");
        openProfile(id, now);
        reply(c, AckStatus::Completed);
        broadcastSnapshot();
        return;
    }
    if (a == "CreateProfile") {
        const std::string name = js::str(p, "name");
        if (name.empty() || name.size() > 60) return reply(c, AckStatus::Rejected, "Enter a name of 1-60 characters.");
        const std::string from = js::str(p, "from", "starter");
        json doc;
        if (from == "working" && working_.profile.is_object()) {
            doc = working_.profile;
            profiles::reassignIds(doc);
        } else if (from == "profile") {
            doc = committedProfile(js::str(p, "profileId"));
            if (!doc.is_object()) return reply(c, AckStatus::Rejected, "That profile does not exist.");
            profiles::reassignIds(doc);
        } else {
            const bool forAircraft = js::boolean(p, "forAircraft", true) && !aircraftModelKey_.empty();
            doc = profiles::makeStarterProfile(name, forAircraft ? aircraftModelKey_ : "", forAircraft ? aircraftTitle_ : "");
        }
        doc["name"] = name;
        if (js::boolean(p, "forAircraft", false) && !aircraftModelKey_.empty())
            doc["aircraftMatch"] = {{"strategy", "aircraftPath"}, {"modelKey", aircraftModelKey_}, {"variantKey", nullptr},
                                    {"displayName", aircraftTitle_.empty() ? json(aircraftModelKey_) : json(aircraftTitle_)}, {"confirmed", false}};
        if (from != "working" && working_.dirtyProfile && !js::boolean(p, "discard", false))
            return reply(c, AckStatus::Rejected, "unsavedChanges");
        CommitRequest req;
        req.upsertProfiles.push_back(doc);
        req.navigation = navigation();
        req.requestId = c.uiInstanceId + ":" + c.requestId;
        PendingCommit pc{c.requestId, "create", working_.draftRevision, nullptr, js::str(doc, "profileId")};
        reply(c, AckStatus::Pending, {}, {{"profileId", js::str(doc, "profileId")}});
        commit(std::move(req), std::move(pc), now);
        return;
    }
    if (a == "DeleteProfile") {
        const std::string id = js::str(p, "profileId");
        if (working_.profile.is_object() && js::str(working_.profile, "profileId") == id)
            return reply(c, AckStatus::Rejected, "This profile is open. Open another profile before deleting it.");
        if (!repo_.committed().profiles.count(id)) return reply(c, AckStatus::Rejected, "That profile does not exist.");
        CommitRequest req;
        req.removeProfiles.push_back(id);
        lastCamera_.erase(id);
        req.navigation = navigation();
        req.requestId = c.uiInstanceId + ":" + c.requestId;
        reply(c, AckStatus::Pending);
        commit(std::move(req), {c.requestId, "delete", working_.draftRevision, nullptr, {}}, now);
        return;
    }
    if (a == "BindProfileToAircraft") {
        if (aircraftModelKey_.empty()) return reply(c, AckStatus::Rejected, "Load an aircraft in the simulator first.");
        const std::string id = js::str(p, "profileId");
        json doc = committedProfile(id);
        if (!doc.is_object()) return reply(c, AckStatus::Rejected, "That profile does not exist.");
        const json match{{"strategy", "userConfirmedModelVariant"}, {"modelKey", aircraftModelKey_}, {"variantKey", nullptr},
                         {"displayName", aircraftTitle_.empty() ? json(aircraftModelKey_) : json(aircraftTitle_)}, {"confirmed", true}};
        doc["aircraftMatch"] = match;
        CommitRequest req;
        req.upsertProfiles.push_back(doc);
        req.navigation = navigation();
        req.requestId = c.uiInstanceId + ":" + c.requestId;
        reply(c, AckStatus::Pending);
        commit(std::move(req), {c.requestId, "bind", working_.draftRevision, nullptr, id}, now);
        return;
    }
    if (a == "ListBackups") {
        json list = json::array();
        const std::string id = working_.profile.is_object() ? js::str(working_.profile, "profileId") : "";
        if (auto it = repo_.committed().profiles.find(id); it != repo_.committed().profiles.end())
            for (const auto& b : it->second.backups)
                list.push_back({{"revision", js::get(b, "revision")}, {"name", js::get(b, "name")}, {"cameraCount", js::array(b, "cameras").size()}});
        return reply(c, AckStatus::Completed, {}, {{"backups", list}});
    }
    if (a == "LoadBackup") {
        const std::string id = working_.profile.is_object() ? js::str(working_.profile, "profileId") : "";
        auto it = repo_.committed().profiles.find(id);
        if (it == repo_.committed().profiles.end()) return reply(c, AckStatus::Rejected, "Open a profile first.");
        const auto rev = js::integer(p, "revision", -1);
        for (const auto& b : it->second.backups)
            if (js::integer(b, "revision", -2) == rev) {
                working_.profile = b;
                working_.draftRevision++;
                recomputeDirty();
                if (!selection_.empty() && !profiles::toSnapshot(working_.profile).camera(selection_)) selection_ = selectionFallback();
                publishWorking(now, false);
                reply(c, AckStatus::Completed, {}, {{"note", "Backup loaded. Save changes to keep it."}});
                broadcastSnapshot();
                return;
            }
        return reply(c, AckStatus::Rejected, "That backup is no longer available.");
    }
    if (a == "ExportProfile") {
        const std::string id = js::str(p, "profileId");
        const bool isWorking = working_.profile.is_object() && js::str(working_.profile, "profileId") == id;
        const bool useWorking = isWorking && js::boolean(p, "working", working_.dirtyProfile);
        json doc = useWorking ? working_.profile : committedProfile(id);
        if (!doc.is_object()) return reply(c, AckStatus::Rejected, "That profile does not exist.");
        return reply(c, AckStatus::Completed, {},
                     {{"code", profiles::exportProfileCode(doc, useWorking && working_.dirtyProfile)},
                      {"label", useWorking && working_.dirtyProfile ? "working copy" : "saved"},
                      {"path", nullptr}});
    }
    if (a == "ImportProfile") {
        std::string err;
        auto report = profiles::importProfileCode(js::str(p, "code"), &err);
        if (!report) return reply(c, AckStatus::Rejected, err);
        json rep{{"cameraCount", report->cameraCount},
                 {"unsupportedCount", report->unsupportedCount},
                 {"notes", report->notes},
                 {"name", js::get(report->profile, "name")},
                 {"aircraft", js::get(js::object(report->profile, "aircraftMatch"), "displayName")},
                 {"matchesLoadedAircraft", !aircraftModelKey_.empty() &&
                                               js::str(js::object(report->profile, "aircraftMatch"), "modelKey") == aircraftModelKey_}};
        CommitRequest req;
        req.upsertProfiles.push_back(report->profile);
        req.navigation = navigation();
        req.requestId = c.uiInstanceId + ":" + c.requestId;
        reply(c, AckStatus::Pending, {}, {{"report", rep}, {"profileId", js::get(report->profile, "profileId")}});
        commit(std::move(req), {c.requestId, "import", working_.draftRevision, rep, {}}, now);
        return;
    }
    reply(c, AckStatus::Rejected, "unknownAction");
}

// ---------------------------------------------------------------------------------------------------------------
// Coordinator events

void ModuleApp::onCoordinatorEvent(const std::string& name, json payload) {
    if (name == "ConnectionChanged") {
        diag("session " + js::dump(payload));
        emitState("SessionChanged", {{"moduleBootId", bootId_},
                                     {"sessionEpoch", coord_.epoch()},
                                     {"session", !js::boolean(payload, "simConnected", false) ? "Discovering" : coord_.aircraft().known() ? "Ready" : "Discovering"},
                                     {"simVersion", js::get(payload, "simVersion")},
                                     {"reason", js::get(payload, "reason")}});
        return;
    }
    if (name == "AircraftChanged") {
        const std::string key = js::str(payload, "modelKey");
        aircraftTitle_ = js::str(payload, "title");
        if (key != aircraftModelKey_) {
            aircraftModelKey_ = key;
            for (const auto& e : router_.releaseAll()) coord_.onAction(e, now_);
        }
        lastAircraft_ = {{"title", js::get(payload, "title")}, {"modelKey", key.empty() ? json(nullptr) : json(key)}};
        resolveAircraft_ = true;  // handled in update(): never re-enter the coordinator from its own callback
        return;
    }
    if (name == "TemporaryLookBaked") {
        // Composed view becomes part of the active camera's draft (design 2.0 §4.2 "Update from current view").
        const std::string id = js::str(payload, "cameraId");
        for (const auto& cam : js::array(working_.profile, "cameras"))
            if (js::str(cam, "id") == id) {
                json updated = cam;
                updated["positionMetersFRD"] = js::get(payload, "positionMetersFRD");
                updated["orientationCameraToAnchorXYZW"] = js::get(payload, "orientationCameraToAnchorXYZW");
                updated["projection"]["value"] = js::num(payload, "fovRad", 1.0);
                json applied;
                std::string err;
                if (applyOps(json::array({{{"op", "setCamera"}, {"camera", updated}}}), applied, err)) {
                    publishWorking(now_, false);
                    emitNow("DraftChanged", {{"draftRevision", working_.draftRevision},
                                             {"dirty", {{"profile", working_.dirtyProfile}, {"settings", working_.dirtySettings}, {"bindings", working_.dirtyBindings}}},
                                             {"ops", applied},
                                             {"origin", "module"}});
                }
                break;
            }
        return;
    }
    if (name == "ViewCaptured") {
        emitNow(name, std::move(payload), captureRequester_);
        return;
    }
    if (name == "ActiveCameraChanged") {
        const std::string id = js::str(payload, "cameraId");
        if (!id.empty() && id != selection_) selectCamera(id);  // both surfaces follow the recalled view
    }
    if (name == "FaultRaised") diag("fault " + js::str(payload, "code") + ": " + js::str(payload, "message"));
    if (name == "OwnershipChanged") diag("camera " + js::str(payload, "state") + ": " + js::str(payload, "reason"));
    if (isStateEvent(name)) emitState(name, std::move(payload));
    else emitNow(name, std::move(payload));
}

// ---------------------------------------------------------------------------------------------------------------
// Working set

json ModuleApp::committedProfile(const std::string& profileId) const {
    auto it = repo_.committed().profiles.find(profileId);
    return it == repo_.committed().profiles.end() ? json(nullptr) : it->second.current;
}

json ModuleApp::profilesList() const {
    json list = json::array();
    for (const auto& [id, rec] : repo_.committed().profiles) {
        const auto& d = rec.current;
        const json& m = js::object(d, "aircraftMatch");
        json copy = d;
        list.push_back({{"profileId", id},
                        {"name", js::get(d, "name")},
                        {"displayName", js::get(m, "displayName")},
                        {"modelKey", js::get(m, "modelKey")},
                        {"matchStrategy", js::str(m, "strategy", "none")},
                        {"cameraCount", js::array(d, "cameras").size()},
                        {"revision", js::get(d, "revision")},
                        {"readOnly", profiles::validateProfile(copy).readOnly}});
    }
    std::sort(list.begin(), list.end(), [](const json& x, const json& y) { return js::str(x, "name") < js::str(y, "name"); });
    return list;
}

json ModuleApp::navigation() const {
    json nav = repo_.committed().navigation.is_object() ? repo_.committed().navigation : json::object();
    nav["lastProfileId"] = working_.profile.is_object() ? js::get(working_.profile, "profileId") : json(nullptr);
    if (js::boolean(js::object(working_.settings, "ui"), "remember", true)) nav["lastCamera"] = lastCamera_;
    if (!aircraftModelKey_.empty() && working_.profile.is_object()) nav["lastByModel"][aircraftModelKey_] = js::get(working_.profile, "profileId");
    return nav;
}

void ModuleApp::selectCamera(const std::string& cameraId) {
    selection_ = cameraId;
    if (working_.profile.is_object()) lastCamera_[js::str(working_.profile, "profileId")] = cameraId;
    emitState("SelectionChanged", {{"profileId", working_.profile.is_object() ? js::get(working_.profile, "profileId") : json(nullptr)},
                                   {"cameraId", cameraId.empty() ? json(nullptr) : json(cameraId)}});
}

std::string ModuleApp::selectionFallback() const {
    if (!working_.profile.is_object()) return {};
    return js::str(working_.profile, "defaultCameraId");
}

void ModuleApp::openProfile(const std::string& profileId, double now) {
    working_.profile = committedProfile(profileId);
    working_.open = working_.profile.is_object();
    working_.draftRevision++;
    recomputeDirty();
    std::string sel;
    if (js::boolean(js::object(working_.settings, "ui"), "remember", true)) sel = js::str(lastCamera_, profileId.c_str());
    if (sel.empty() || !profiles::toSnapshot(working_.profile).camera(sel)) sel = selectionFallback();
    publishWorking(now, false);
    selectCamera(sel);
}

void ModuleApp::recomputeDirty() {
    const auto& st = repo_.committed();
    const json committed = working_.profile.is_object() ? committedProfile(js::str(working_.profile, "profileId")) : json(nullptr);
    working_.dirtyProfile = working_.profile.is_object() && withoutRevision(working_.profile) != withoutRevision(committed);
    working_.dirtySettings = withoutPreferences(working_.settings) != withoutPreferences(st.settings);
    working_.dirtyBindings = working_.bindings != st.bindings;
}

void ModuleApp::publishWorking(double now, bool bindingsChanged) {
    if (working_.profile.is_object()) {
        json copy = working_.profile;
        if (profiles::validateProfile(copy).ok()) coord_.setProfile(profiles::toSnapshot(copy), now);
    } else {
        coord_.setProfile(std::nullopt, now);
    }
    coord_.setSettings(profiles::parseHostSettings(working_.settings), now);
    if (bindingsChanged) {
        for (const auto& e : router_.releaseAll()) coord_.onAction(e, now);
        auto set = profiles::parseBindings(working_.bindings);
        services_.mapInputs(router_.setBindings(set.bindings));
    }
}

bool ModuleApp::applyOps(const json& ops, json& applied, std::string& error) {
    if (!ops.is_array() || ops.empty() || ops.size() > 64) {
        error = "No changes were sent.";
        return false;
    }
    json profile = working_.profile, settings = working_.settings, bindings = working_.bindings;
    applied = json::array();
    for (const auto& op : ops) {
        const std::string kind = js::str(op, "op");
        if (kind == "setSettings") {
            settings = profiles::normalizeSettings(js::get(op, "settings"));
            applied.push_back({{"op", kind}, {"settings", settings}});
            continue;
        }
        if (kind == "setBindings") {
            const json& b = js::get(op, "bindings");
            auto parsed = profiles::parseBindings(b);
            if (!parsed.problems.empty()) {
                error = parsed.problems.front();
                return false;
            }
            bindings = b;
            applied.push_back({{"op", kind}, {"bindings", bindings}});
            continue;
        }
        if (!profile.is_object()) {
            error = "Open or create a profile first.";
            return false;
        }
        if (kind == "setCamera") {
            json cam = js::get(op, "camera");
            const std::string id = js::str(cam, "id");
            if (!cam.is_object() || !profiles::isUuid(id)) {
                error = "The camera has no valid ID.";
                return false;
            }
            bool replaced = false;
            for (auto& existing : profile["cameras"])
                if (js::str(existing, "id") == id) {
                    existing = cam;
                    replaced = true;
                }
            if (!replaced) {
                profile["cameras"].push_back(cam);
                for (auto& cat : profile["categories"])
                    if (js::str(cat, "id") == js::str(cam, "categoryId")) {
                        bool listed = false;
                        for (const auto& ref : js::array(cat, "cameraIds")) listed = listed || js::str(ref) == id;
                        if (!listed) cat["cameraIds"].push_back(id);
                    }
            }
        } else if (kind == "removeCamera") {
            const std::string id = js::str(op, "cameraId");
            auto& cams = profile["cameras"];
            cams.erase(std::remove_if(cams.begin(), cams.end(), [&](const json& c) { return js::str(c, "id") == id; }), cams.end());
            for (auto& cat : profile["categories"]) {
                if (!js::get(cat, "cameraIds").is_array()) continue;
                auto& ids = cat["cameraIds"];
                ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const json& r) { return js::str(r) == id; }), ids.end());
            }
        } else if (kind == "setCategories") {
            const json& cats = js::get(op, "categories");
            if (!cats.is_array()) {
                error = "Categories must be a list.";
                return false;
            }
            profile["categories"] = cats;
            // Keep each camera's categoryId consistent with the list it now appears in.
            for (const auto& cat : cats)
                for (const auto& ref : js::array(cat, "cameraIds"))
                    for (auto& cam : profile["cameras"])
                        if (js::str(cam, "id") == js::str(ref)) cam["categoryId"] = js::get(cat, "id");
        } else if (kind == "setProfileFields") {
            for (const char* k : {"name", "defaultCameraId", "aircraftMatch"})
                if (js::has(op, k)) profile[k] = js::get(op, k);
        } else {
            error = "Unknown change '" + kind + "'.";
            return false;
        }
        applied.push_back(op);
    }
    if (profile.is_object()) {
        auto v = profiles::validateProfile(profile);
        if (!v.ok()) {
            error = "That change was not applied: " + v.summary();
            return false;
        }
        if (v.readOnly) {
            error = "This profile needs a newer version of Camera Studio and cannot be edited.";
            return false;
        }
        // Echo normalised camera objects (defaults filled) so every mirror converges on the same draft.
        for (auto& op : applied)
            if (js::str(op, "op") == "setCamera")
                for (const auto& cam : js::array(profile, "cameras"))
                    if (js::str(cam, "id") == js::str(js::get(op, "camera"), "id")) op["camera"] = cam;
    }
    working_.profile = std::move(profile);
    working_.settings = std::move(settings);
    working_.bindings = std::move(bindings);
    working_.draftRevision++;
    recomputeDirty();
    return true;
}

void ModuleApp::commit(CommitRequest req, PendingCommit pending, double now) {
    (void)now;
    if (pendingCommit_) {
        commitQueue_.emplace_back(std::move(req), std::move(pending));
        return;
    }
    pendingCommit_ = pending;
    // Snapshot of what this transaction publishes; later edits stay dirty.
    const json profileAtRequest = req.upsertProfiles.empty() ? json(nullptr) : req.upsertProfiles.front();
    repo_.commit(std::move(req), [this, profileAtRequest](const CommitResult& r) {
        PendingCommit pc = *pendingCommit_;
        pendingCommit_.reset();
        if (r.status == CommitStatus::Committed) {
            if (pc.kind == "save" && working_.profile.is_object() && profileAtRequest.is_object()) {
                const json saved = committedProfile(js::str(profileAtRequest, "profileId"));
                // Edits made while the save was pending remain in the draft and stay dirty.
                if (withoutRevision(working_.profile) == withoutRevision(profileAtRequest)) working_.profile = saved;
                else working_.profile["revision"] = js::get(saved, "revision");
            }
            if (pc.kind == "bind" && working_.profile.is_object() && js::str(working_.profile, "profileId") == pc.openAfter) {
                working_.profile["aircraftMatch"] = js::get(committedProfile(pc.openAfter), "aircraftMatch");
            }
            recomputeDirty();
            if (pc.kind == "qualification") coord_.refreshQualification();
            if (pc.kind == "create" && !pc.openAfter.empty()) {
                // Save as New carries the edits into the new profile; the old profile's draft is dropped.
                openProfile(pc.openAfter, now_);
            } else {
                publishWorking(now_, false);
            }
        }
        if (pc.kind == "preference") {
            if (!commitQueue_.empty()) {
                auto next = std::move(commitQueue_.front());
                commitQueue_.pop_front();
                commit(std::move(next.first), std::move(next.second), now_);
            }
            return;
        }
        json completed{{"requestId", pc.requestId},
                       {"kind", pc.kind},
                       {"status", r.status == CommitStatus::Committed ? "Committed" : "Failed"},
                       {"revisions", {{"draft", pc.draftRevision}, {"txn", r.txn}}},
                       {"message", r.message},
                       {"report", pc.report}};
        if (pc.kind == "qualification") {
            completed["g01"] = js::boolean(pc.report, "g01", false);
            completed["g03"] = js::boolean(pc.report, "g03", false);
        }
        emitNow("CommitCompleted", std::move(completed));
        if (r.status == CommitStatus::Committed) {
            if (pc.kind == "create" || pc.kind == "bind" || pc.kind == "import" || pc.kind == "delete") {
                resolveAircraft_ = pc.kind == "bind";
                broadcastSnapshot();
            } else {
                emitNow("DraftChanged", {{"draftRevision", working_.draftRevision},
                                         {"dirty", {{"profile", working_.dirtyProfile}, {"settings", working_.dirtySettings}, {"bindings", working_.dirtyBindings}}},
                                         {"ops", json::array()},
                                         {"origin", "module"}});
                emitState("ProfilesChanged", {{"profiles", profilesList()}});
            }
        } else {
            diag("save failed: " + r.message);
        }
        if (!commitQueue_.empty()) {
            auto next = std::move(commitQueue_.front());
            commitQueue_.pop_front();
            commit(std::move(next.first), std::move(next.second), now_);
        }
    });
}

void ModuleApp::resolveAircraftProfile(double now) {
    json reason = nullptr, match = nullptr;
    if (aircraftModelKey_.empty()) {
        lastAircraft_["matchProfileId"] = nullptr;
        lastAircraft_["matchReason"] = nullptr;
        emitState("AircraftChanged", lastAircraft_);
        return;
    }
    const auto& st = repo_.committed();
    const std::string preferred = js::str(js::object(st.navigation, "lastByModel"), aircraftModelKey_.c_str());
    std::string chosen;
    std::string strategy;
    for (const auto& [id, rec] : st.profiles) {
        const json& m = js::object(rec.current, "aircraftMatch");
        if (js::str(m, "modelKey") != aircraftModelKey_) continue;
        if (chosen.empty() || id == preferred) {
            chosen = id;
            strategy = js::str(m, "strategy");
        }
    }
    const std::string openId = working_.profile.is_object() ? js::str(working_.profile, "profileId") : "";
    if (!chosen.empty()) {
        match = chosen;
        reason = strategy == "userConfirmedModelVariant" ? "Matched because you chose this profile for this aircraft"
                                                          : "Matched by the aircraft's package";
        if (chosen != openId) {
            if (!working_.dirtyProfile && !pendingCommit_) {
                openProfile(chosen, now);
                broadcastSnapshot();
            } else {
                reason = "A matching profile exists; save or discard the open changes to switch to it";
            }
        }
    } else {
        reason = "No camera profile for this aircraft yet";
    }
    lastAircraft_["matchProfileId"] = match;
    lastAircraft_["matchReason"] = reason;
    emitState("AircraftChanged", lastAircraft_);
}

Qualification ModuleApp::qualification(const std::string& simVersion) const {
    return qualificationFor(js::object(repo_.committed().navigation, "qualification"), simVersion);
}

void ModuleApp::diag(const std::string& line) {
    diagnostics_.push_back(line.substr(0, 300));
    if (diagnostics_.size() > 200) diagnostics_.pop_front();
}

}  // namespace fscs::host
