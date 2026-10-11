#include "sim_session.hpp"
#include "input_registration.hpp"

#include <MSFS/MSFS.h>
#include <MSFS/MSFS_CommBus.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <SimConnect.h>

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace fscs::host {

namespace {

enum : DWORD { kEvtAircraftLoaded = 1, kEvtFlightLoaded, kEvtSim, kEvtPause, kEvtMouseRight, kEvtMouseX, kEvtMouseY, kEvtTest = 900, kEvtTestPovRelease = 901, kEvtInputBase = 1000, kEvtPovReleaseBase = 4000 };
enum : DWORD { kReqAircraftState = 1, kReqTitle, kReqTitleOnce, kReqMotion, kReqPosition };
enum : DWORD { kDefTitle = 1, kDefMotion, kDefPosition };
enum : DWORD { kInputGroup = 1, kTestInputGroup = 2, kMouseInputGroup = 3, kMouseAxesGroup = 4, kCaptureInputGroup = 5 };
enum : DWORD { kNotifyInput = 1 };
enum : DWORD { kEvtZoomIn = 800, kEvtZoomOut, kEvtZoomPlus, kEvtZoomMinus };
constexpr std::size_t kMaxInputs = 1000; // SDK input-group limit, including primary/secondary and modifier aliases
constexpr DWORD kFirstCaptureEvent = 10000;
// Controller enumeration has no capability count. Keep discovery to the common
// 32-button bank and the documented first POV hat. Manual tested assignments
// support additional buttons and hats.
// Out-of-range mappings on some controllers can alias button zero, so never
// blindly install 128 mappings. All asynchronous mapping failures are removed.
constexpr int kCaptureButtons = 32;
constexpr int kCaptureInputs = kCaptureButtons + 1;
constexpr int kCaptureEvents = kCaptureInputs + 1; // Separate POV release event.

std::string captureDefinition(int device, int input) {
    return "joystick:" + std::to_string(device) + (input < kCaptureButtons ? ":button:" + std::to_string(input) : ":POV:0");
}

void CALLBACK dispatchProc(SIMCONNECT_RECV* data, DWORD size, void* ctx) { static_cast<SimSession*>(ctx)->onRecv(data, size); }

bool isPovDefinition(const std::string& d) { return d.find(":POV:") != std::string::npos; }

HRESULT mapPovInput(HANDLE handle, DWORD group, const std::string& definition, DWORD down, DWORD release) {
    return registerPovInput([&](const std::string& input, std::uint32_t downId, std::uint32_t downValue,
                               std::uint32_t upId, std::uint32_t upValue) {
        return SimConnect_MapInputEventToClientEvent_EX1(handle, group, input.c_str(), downId, downValue, upId, upValue, FALSE);
    }, definition, down, release);
}

// Under WASM a SimConnect HANDLE is an integer; the header keeps it as unsigned long long.
HANDLE asHandle(unsigned long long v) { return static_cast<HANDLE>(v); }

}  // namespace

bool SimSession::open() {
    if (handle_) return true;
    HANDLE h = 0;
    if (FAILED(SimConnect_Open(&h, "FS Camera Studio", nullptr, 0, 0, 0))) return false;
    handle_ = static_cast<unsigned long long>(h);
    setup();
    // In WASM this registers a persistent callback, rather than polling a desktop queue.
    // Register exactly once per connection; repeated per-frame registrations can add work
    // throughout a flight. The simulator delivers into staged_ between module updates.
    if (FAILED(SimConnect_CallDispatch(h, dispatchProc, this))) {
        close();
        return false;
    }
    return true;
}

void SimSession::setup() {
    const HANDLE h = static_cast<HANDLE>(handle_);
    SimConnect_SubscribeToSystemEvent(h, kEvtAircraftLoaded, "AircraftLoaded");
    SimConnect_SubscribeToSystemEvent(h, kEvtFlightLoaded, "FlightLoaded");
    SimConnect_SubscribeToSystemEvent(h, kEvtSim, "Sim");
    SimConnect_SubscribeToSystemEvent(h, kEvtPause, "Pause_EX1");
    SimConnect_AddToDataDefinition(h, kDefTitle, "TITLE", nullptr, SIMCONNECT_DATATYPE_STRING256);
    SimConnect_RequestDataOnSimObject(h, kReqTitle, kDefTitle, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SECOND,
                                      SIMCONNECT_DATA_REQUEST_FLAG_CHANGED);
    SimConnect_RequestSystemState(h, kReqAircraftState, "AircraftLoaded");
    airportDefinitionReady_=false;airportRequests_.clear();
    airportIdentityDefinitionReady_=false;airportIdentityRequests_.clear();
    for(const auto& v:std::vector<std::pair<const char*,const char*>>{{"PLANE LATITUDE","degrees"},{"PLANE LONGITUDE","degrees"},{"PLANE ALTITUDE","meters"},{"PLANE HEADING DEGREES TRUE","degrees"}})
        SimConnect_AddToDataDefinition(h,kDefPosition,v.first,v.second,SIMCONNECT_DATATYPE_FLOAT64);
    SimConnect_RequestDataOnSimObject(h,kReqPosition,kDefPosition,SIMCONNECT_OBJECT_ID_USER,SIMCONNECT_PERIOD_SIM_FRAME,SIMCONNECT_DATA_REQUEST_FLAG_DEFAULT,0,1);
    // Fixed double layout, in SI units except RPM. Read-only, throttled to every second sim frame.
    const std::pair<const char*, const char*> motionVars[] = {
        {"SIM ON GROUND", "Bool"}, {"GROUND VELOCITY", "meters per second"}, {"VERTICAL SPEED", "meters per second"},
        {"G FORCE", "GForce"}, {"ACCELERATION BODY Z", "meters per second squared"},
        {"ACCELERATION BODY X", "meters per second squared"}, {"GENERAL ENG RPM:1", "rpm"}};
    for (const auto& [name, unit] : motionVars) SimConnect_AddToDataDefinition(h, kDefMotion, name, unit, SIMCONNECT_DATATYPE_FLOAT64);
    SimConnect_RequestDataOnSimObject(h, kReqMotion, kDefMotion, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME,
                                    SIMCONNECT_DATA_REQUEST_FLAG_DEFAULT, 0, 1);

    for (const DWORD id : {kEvtTest, kEvtTestPovRelease}) {
        SimConnect_MapClientEventToSimEvent(h, id, "");
        SimConnect_AddClientEventToNotificationGroup(h, kNotifyInput, id, FALSE);
    }
    SimConnect_SetNotificationGroupPriority(h, kNotifyInput, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
    // A loading crash was observed with startup-time native mouse subscriptions. Keep setup free of them.
    mouseMappingPackets_.clear();
    inputMappingPackets_.clear();
    mouseRegistered_ = mouseEnabled_ = mouseAxesEnabled_ = mouseFailed_ = false;
    clientEventsDefined_ = 0; povReleaseEventsDefined_.clear();
    zoomRegistered_ = false;
    captureDevice_ = -1; captureEventBase_ = nextCaptureEventBase_ = kFirstCaptureEvent; captureGeneration_ = 0; capturePackets_.clear(); captureInputsValid_.clear();
}

void SimSession::setNativeMouseEnabled(bool enabled) {
    if (!handle_ || mouseFailed_ || enabled == mouseEnabled_) return;
    const HANDLE h = asHandle(handle_);
    bool ready = true;
    const auto checkMouse = [&](HRESULT result) {
        ready = SUCCEEDED(result) && ready;
        DWORD packet = 0;
        if (SUCCEEDED(SimConnect_GetLastSentPacketID(h, &packet))) mouseMappingPackets_.push_back(packet);
    };
    // SU6 mouse mappings are installed only after the custom camera is acquired, once per connection.
    // Asobo: https://devsupport.flightsimulator.com/t/mapinputeventtoclientevent-mouse/9489/6
    if (enabled && !mouseRegistered_) {
        for (DWORD id : {kEvtMouseRight, kEvtMouseX, kEvtMouseY}) {
            checkMouse(SimConnect_MapClientEventToSimEvent(h, id, ""));
            checkMouse(SimConnect_AddClientEventToNotificationGroup(h, kNotifyInput, id, FALSE));
        }
        checkMouse(SimConnect_MapInputEventToClientEvent_EX1(h, kMouseInputGroup, "mouse:0:button:1", kEvtMouseRight, 1, kEvtMouseRight, 0, FALSE));
        checkMouse(SimConnect_MapInputEventToClientEvent_EX1(h, kMouseAxesGroup, "mouse:0:XAxis", kEvtMouseX));
        checkMouse(SimConnect_MapInputEventToClientEvent_EX1(h, kMouseAxesGroup, "mouse:0:YAxis", kEvtMouseY));
        checkMouse(SimConnect_SetInputGroupPriority(h, kMouseInputGroup, SIMCONNECT_GROUP_PRIORITY_HIGHEST));
        checkMouse(SimConnect_SetInputGroupPriority(h, kMouseAxesGroup, SIMCONNECT_GROUP_PRIORITY_HIGHEST));
        checkMouse(SimConnect_SetInputGroupState(h, kMouseAxesGroup, SIMCONNECT_STATE_OFF));
        mouseRegistered_ = ready;
    }
    if (!enabled) setMouseAxesEnabled(false);
    checkMouse(SimConnect_SetInputGroupState(h, kMouseInputGroup, enabled && ready ? SIMCONNECT_STATE_ON : SIMCONNECT_STATE_OFF));
    mouseFailed_ = !ready;
    mouseEnabled_ = enabled && ready;
    if (!ready) SimConnect_SetInputGroupState(h, kMouseInputGroup, SIMCONNECT_STATE_OFF);
    staged_.push_back(backend_event::NativeMouseMapping{mouseEnabled_});
}

void SimSession::setMouseAxesEnabled(bool enabled) {
    if (!handle_ || enabled == mouseAxesEnabled_) return;
    // Outside a held right drag, leave native mouse-axis handling (including toolbar hover) alone.
    const HRESULT result = SimConnect_SetInputGroupState(asHandle(handle_), kMouseAxesGroup,
        enabled ? SIMCONNECT_STATE_ON : SIMCONNECT_STATE_OFF);
    DWORD packet = 0;
    if (SUCCEEDED(SimConnect_GetLastSentPacketID(asHandle(handle_), &packet))) mouseMappingPackets_.push_back(packet);
    if (FAILED(result)) {
        mouseFailed_ = true; mouseEnabled_ = false;
        SimConnect_SetInputGroupState(asHandle(handle_), kMouseInputGroup, SIMCONNECT_STATE_OFF);
        SimConnect_SetInputGroupState(asHandle(handle_), kMouseAxesGroup, SIMCONNECT_STATE_OFF);
        staged_.push_back(backend_event::NativeMouseMapping{false});
    }
    mouseAxesEnabled_ = enabled && SUCCEEDED(result);
}

void SimSession::close() {
    if (!handle_) return;
    SimConnect_Close(static_cast<HANDLE>(handle_));
    handle_ = 0;
    staged_.clear();
}

void SimSession::dispatch(std::vector<BackendEvent>& out) {
    for (auto& e : staged_) out.push_back(std::move(e));
    staged_.clear();
}

bool SimSession::sendToPanels(const std::string& text) {
    return fsCommBusCall(kHostToUiEvent, text.c_str(), static_cast<unsigned int>(text.size() + 1), FsCommBusBroadcast_JS);
}

bool SimSession::ensureClientEvents(std::size_t count) {
    // Private client events (empty sim-event name) that mapped inputs fire; registered once each.
    for (; clientEventsDefined_ < count; ++clientEventsDefined_) {
        const DWORD id = kEvtInputBase + static_cast<DWORD>(clientEventsDefined_);
        if (FAILED(SimConnect_MapClientEventToSimEvent(asHandle(handle_), id, ""))) return false;
        if (FAILED(SimConnect_AddClientEventToNotificationGroup(asHandle(handle_), kNotifyInput, id, FALSE))) return false;
    }
    return true;
}

bool SimSession::ensureZoomEvents() {
    if (zoomRegistered_) return true;
    // Observe the simulator's zoom commands, without masking cockpit controls or capturing the screen.
    // The wheel must be assigned to MSFS zoom; these are NOT raw wheel mappings.
    // ZOOM_OUT is the SDK alias for PLUS and ZOOM_IN for MINUS (despite their names).
    // Subscribe once per canonical event: aliases must not cancel or double a wheel step.
    const char* names[] = {"ZOOM_PLUS", "ZOOM_MINUS"};
    for (DWORD i = 0; i < 2; ++i) {
        if (FAILED(SimConnect_MapClientEventToSimEvent(asHandle(handle_), kEvtZoomPlus + i, names[i]))) return false;
        if (FAILED(SimConnect_AddClientEventToNotificationGroup(asHandle(handle_), kNotifyInput, kEvtZoomPlus + i, FALSE))) return false;
    }
    zoomRegistered_ = true;
    return true;
}

bool SimSession::mapInputs(const std::vector<std::string>& definitions) {
    if (!handle_) return false;
    const std::size_t n = (std::min)(definitions.size(), kMaxInputs);
    const HANDLE h = asHandle(handle_);
    inputMappingPackets_.clear();
    std::size_t failed = definitions.size() - n;
    std::uint32_t error = 0;
    const auto check = [&](HRESULT result) {
        DWORD packet = 0;
        if (SUCCEEDED(result) && SUCCEEDED(SimConnect_GetLastSentPacketID(h, &packet))) inputMappingPackets_.push_back(packet);
        if (FAILED(result)) { ++failed; error = static_cast<std::uint32_t>(result); return false; }
        return true;
    };
    const auto report = [&]() {
        staged_.push_back(backend_event::InputMappingStatus{failed == 0, n, failed, error});
        return failed == 0;
    };
    if (!check(SimConnect_ClearInputGroup(h, kInputGroup))) return report();
    mapped_.assign(definitions.begin(), definitions.begin() + static_cast<long>(n));
    isPov_.clear();
    if (!ensureClientEvents(n)) { ++failed; return report(); }
    for (std::size_t i = 0; i < n; ++i) {
        const DWORD id = kEvtInputBase + static_cast<DWORD>(i);
        const bool pov = isPovDefinition(mapped_[i]);
        isPov_.push_back(pov);
        if (mapped_[i] == "sim:ZOOM_IN" || mapped_[i] == "sim:ZOOM_OUT") {
            if (!ensureZoomEvents()) ++failed;
            continue;
        }
        // Keep valid mappings available even if one optional alias is rejected.
        // Buttons and keys: down sends 1, up sends 0. POV angles need an
        // explicit release notification, normalized to center in onRecv.
        if (pov) {
            const DWORD release = kEvtPovReleaseBase + static_cast<DWORD>(i);
            if (!povReleaseEventsDefined_.count(release)) {
                if (!check(SimConnect_MapClientEventToSimEvent(h, release, "")) ||
                    !check(SimConnect_AddClientEventToNotificationGroup(h, kNotifyInput, release, FALSE))) continue;
                povReleaseEventsDefined_.insert(release);
            }
            check(mapPovInput(h, kInputGroup, mapped_[i], id, release));
        }
        else check(SimConnect_MapInputEventToClientEvent_EX1(h, kInputGroup, mapped_[i].c_str(), id, 1, id, 0));
    }
    check(SimConnect_SetInputGroupPriority(h, kInputGroup, SIMCONNECT_GROUP_PRIORITY_HIGHEST));
    check(SimConnect_SetInputGroupState(h, kInputGroup, captureDevice_ < 0 ? SIMCONNECT_STATE_ON : SIMCONNECT_STATE_OFF));
    return report();
}

bool SimSession::mapTestInput(const std::string& definition) {
    if (!handle_) return false;
    if (testMapped_) SimConnect_ClearInputGroup(asHandle(handle_), kTestInputGroup);
    testMapped_ = false;
    if (definition.empty()) return true;
    testIsPov_ = isPovDefinition(definition);
    const HRESULT hr = testIsPov_ ? mapPovInput(asHandle(handle_), kTestInputGroup, definition, kEvtTest, kEvtTestPovRelease)
                                  : SimConnect_MapInputEventToClientEvent_EX1(asHandle(handle_), kTestInputGroup, definition.c_str(), kEvtTest, 1, kEvtTest, 0);
    if (FAILED(hr)) return false;
    SimConnect_SetInputGroupPriority(asHandle(handle_), kTestInputGroup, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
    SimConnect_SetInputGroupState(asHandle(handle_), kTestInputGroup, SIMCONNECT_STATE_ON);
    testMapped_ = true;
    return true;
}

bool SimSession::captureControllerInputs(int deviceId, std::uint64_t generation) {
    if (!handle_) return deviceId < 0;
    const HANDLE h = asHandle(handle_);
    SimConnect_SetInputGroupState(h, kCaptureInputGroup, SIMCONNECT_STATE_OFF);
    SimConnect_ClearInputGroup(h, kCaptureInputGroup);
    if (captureDevice_ >= 0) for (int input = 0; input < kCaptureEvents; ++input)
        SimConnect_RemoveClientEvent(h, kNotifyInput, captureEventBase_ + input);
    captureDevice_ = -1; capturePackets_.clear(); captureInputsValid_.clear(); captureGeneration_ = 0;
    if (deviceId < 0) return SUCCEEDED(SimConnect_SetInputGroupState(h, kInputGroup, SIMCONNECT_STATE_ON));
    captureDevice_ = deviceId;
    captureGeneration_ = generation;
    const auto check = [&](HRESULT result, int input, const std::string& stage) {
        if (FAILED(result)) {
            staged_.push_back(backend_event::ControllerCaptureMappingStatus{deviceId, generation, input >= 0,
                input < 0 ? "" : captureDefinition(deviceId, input), stage, static_cast<std::uint32_t>(result)});
            return false;
        }
        DWORD packet = 0;
        if (SUCCEEDED(SimConnect_GetLastSentPacketID(h, &packet))) capturePackets_[packet] = {input, stage};
        return true;
    };
    if (!check(SimConnect_SetInputGroupState(h, kInputGroup, SIMCONNECT_STATE_OFF), -1, "suspend camera inputs")) {
        captureControllerInputs(-1); return false;
    }
    // New event IDs for each listening request prevent queued SDK events from an
    // earlier controller or dialog being attributed to the newly selected device.
    captureEventBase_ = nextCaptureEventBase_;
    nextCaptureEventBase_ += kCaptureEvents;
    captureInputsValid_.assign(kCaptureInputs, false);
    int mapped = 0;
    for (int input = 0; input < kCaptureInputs; ++input) {
        const std::string definition = captureDefinition(deviceId, input);
        const DWORD id = captureEventBase_ + input;
        if (!check(SimConnect_MapClientEventToSimEvent(h, id, ""), input, "register event") ||
            !check(SimConnect_AddClientEventToNotificationGroup(h, kNotifyInput, id, FALSE), input, "subscribe event")) continue;
        HRESULT result;
        if (input < kCaptureButtons) {
            result = SimConnect_MapInputEventToClientEvent_EX1(h, kCaptureInputGroup, definition.c_str(), id, 1, id, 0, FALSE);
        } else {
            const DWORD release = captureEventBase_ + kCaptureInputs;
            if (!check(SimConnect_MapClientEventToSimEvent(h, release, ""), input, "register hat release") ||
                !check(SimConnect_AddClientEventToNotificationGroup(h, kNotifyInput, release, FALSE), input, "subscribe hat release")) continue;
            result = mapPovInput(h, kCaptureInputGroup, definition, id, release);
        }
        if (check(result, input, "map input")) { captureInputsValid_[input] = true; ++mapped; }
    }
    const bool ready = mapped > 0 && check(SimConnect_SetInputGroupPriority(h, kCaptureInputGroup, SIMCONNECT_GROUP_PRIORITY_HIGHEST), -1, "input priority") &&
        check(SimConnect_SetInputGroupState(h, kCaptureInputGroup, SIMCONNECT_STATE_ON), -1, "enable listening");
    if (!ready) {
        if (mapped == 0) staged_.push_back(backend_event::ControllerCaptureMappingStatus{deviceId, generation, false, "", "no usable inputs", 0});
        captureControllerInputs(-1); return false;
    }
    return true;
}

void SimSession::requestTitle() {
    // The periodic CHANGED request may have reported the title before the aircraft was known; ask once more.
    if (handle_) SimConnect_RequestDataOnSimObject(asHandle(handle_), kReqTitleOnce, kDefTitle, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_ONCE);
}

bool SimSession::enumerateControllers() { return handle_ && SUCCEEDED(SimConnect_EnumerateControllers(asHandle(handle_))); }

void SimSession::onRecv(const void* raw, unsigned long size) {
    if(onAirportRecv(raw,size))return;
    const auto* data = static_cast<const SIMCONNECT_RECV*>(raw);
    switch (data->dwID) {
        case SIMCONNECT_RECV_ID_OPEN: {
            const auto* o = static_cast<const SIMCONNECT_RECV_OPEN*>(data);
            char v[64];
            std::snprintf(v, sizeof(v), "%lu.%lu.%lu.%lu", static_cast<unsigned long>(o->dwApplicationVersionMajor),
                          static_cast<unsigned long>(o->dwApplicationVersionMinor), static_cast<unsigned long>(o->dwApplicationBuildMajor),
                          static_cast<unsigned long>(o->dwApplicationBuildMinor));
            staged_.push_back(backend_event::Connected{v});
            break;
        }
        case SIMCONNECT_RECV_ID_EXCEPTION: {
            const auto* e = static_cast<const SIMCONNECT_RECV_EXCEPTION*>(raw);
            const auto capturePacket = capturePackets_.find(e->dwSendID);
            if (captureDevice_ >= 0 && capturePacket != capturePackets_.end()) {
                const auto registration = capturePacket->second;
                const int input = registration.input;
                const std::string definition = input < 0 ? "" : captureDefinition(captureDevice_, input);
                if (input >= 0) {
                    captureInputsValid_[input] = false;
                    SimConnect_RemoveInputEvent(asHandle(handle_), kCaptureInputGroup, definition.c_str());
                }
                const bool ready = input >= 0 && std::any_of(captureInputsValid_.begin(), captureInputsValid_.end(), [](bool valid) { return valid; });
                staged_.push_back(backend_event::ControllerCaptureMappingStatus{captureDevice_, captureGeneration_, ready, definition,
                    registration.stage, e->dwException});
                capturePackets_.erase(capturePacket);
            }
            const auto inputPacket = std::find(inputMappingPackets_.begin(), inputMappingPackets_.end(), e->dwSendID);
            if (inputPacket != inputMappingPackets_.end()) {
                staged_.push_back(backend_event::InputMappingStatus{false, mapped_.size(), 1, e->dwException});
                inputMappingPackets_.erase(inputPacket); // Log each rejected registration only once.
            }
            for (const auto packet : mouseMappingPackets_) if (packet == e->dwSendID) {
                SimConnect_SetInputGroupState(asHandle(handle_), kMouseInputGroup, SIMCONNECT_STATE_OFF);
                SimConnect_SetInputGroupState(asHandle(handle_), kMouseAxesGroup, SIMCONNECT_STATE_OFF);
                mouseEnabled_ = mouseAxesEnabled_ = false;
                mouseFailed_ = true; // do not retry a rejected native API call each frame
                staged_.push_back(backend_event::NativeMouseMapping{false});
                break;
            }
            break;
        }
        case SIMCONNECT_RECV_ID_QUIT:
            staged_.push_back(backend_event::Disconnected{"The simulator is closing."});
            break;
        case SIMCONNECT_RECV_ID_EVENT_FILENAME: {
            const auto* f = static_cast<const SIMCONNECT_RECV_EVENT_FILENAME*>(data);
            if (f->uEventID == kEvtAircraftLoaded) {
                staged_.push_back(backend_event::AircraftLoaded{f->szFileName});
                requestTitle();
            }
            else if (f->uEventID == kEvtFlightLoaded) staged_.push_back(backend_event::FlightLoaded{f->szFileName});
            break;
        }
        case SIMCONNECT_RECV_ID_EVENT: {
            const auto* e = static_cast<const SIMCONNECT_RECV_EVENT*>(data);
            if (e->uEventID == kEvtSim) {
                staged_.push_back(backend_event::SimRunning{e->dwData == 1});
            } else if (e->uEventID == kEvtPause) {
                staged_.push_back(backend_event::Paused{e->dwData != 0});
            } else if (e->uEventID == kEvtMouseRight) {
                setMouseAxesEnabled(mouseEnabled_ && !mouseFailed_ && e->dwData == 1);
                staged_.push_back(backend_event::NativeMouseButton{e->dwData == 1});
            } else if (e->uEventID == kEvtMouseX || e->uEventID == kEvtMouseY) {
                if (mouseAxesEnabled_) staged_.push_back(backend_event::NativeMouseAxis{e->uEventID == kEvtMouseX, static_cast<std::int32_t>(e->dwData)});
            } else if (e->uEventID >= kEvtZoomIn && e->uEventID <= kEvtZoomMinus) {
                const auto definition = (e->uEventID == kEvtZoomIn || e->uEventID == kEvtZoomPlus) ? "sim:ZOOM_IN" : "sim:ZOOM_OUT";
                const auto it = std::find(mapped_.begin(), mapped_.end(), definition);
                if (it != mapped_.end()) staged_.push_back(backend_event::InputEvent{static_cast<std::size_t>(it - mapped_.begin()), true, 1, false});
            } else if (e->uEventID == kEvtTest || e->uEventID == kEvtTestPovRelease) {
                const bool release = e->uEventID == kEvtTestPovRelease;
                if (!release || testIsPov_) staged_.push_back(backend_event::InputEvent{0, testIsPov_ || e->dwData == 1, povInputValue(release, e->dwData), true});
            } else if (captureDevice_ >= 0 && e->uEventID >= captureEventBase_ && e->uEventID < captureEventBase_ + kCaptureEvents) {
                const bool release = e->uEventID == captureEventBase_ + kCaptureInputs;
                const int input = release ? kCaptureButtons : static_cast<int>(e->uEventID - captureEventBase_);
                if (captureInputsValid_[input]) {
                    if (input < kCaptureButtons)
                        staged_.push_back(backend_event::ControllerButton{captureDevice_, input, e->dwData == 1, captureGeneration_});
                    else staged_.push_back(backend_event::ControllerHat{captureDevice_, 0, povInputValue(release, e->dwData), captureGeneration_});
                }
            } else if (e->uEventID >= kEvtPovReleaseBase && e->uEventID < kEvtPovReleaseBase + mapped_.size()) {
                const std::size_t i = e->uEventID - kEvtPovReleaseBase;
                if (i < isPov_.size() && isPov_[i]) staged_.push_back(backend_event::InputEvent{i, true, povInputValue(true, e->dwData), false});
            } else if (e->uEventID >= kEvtInputBase && e->uEventID < kEvtInputBase + mapped_.size()) {
                const std::size_t i = e->uEventID - kEvtInputBase;
                const bool pov = i < isPov_.size() && isPov_[i];
                staged_.push_back(backend_event::InputEvent{i, pov || e->dwData == 1, e->dwData, false});
            }
            break;
        }
        case SIMCONNECT_RECV_ID_SYSTEM_STATE: {
            const auto* s = static_cast<const SIMCONNECT_RECV_SYSTEM_STATE*>(data);
            if (s->dwRequestID == kReqAircraftState && s->szString[0]) {
                staged_.push_back(backend_event::AircraftLoaded{s->szString});
                requestTitle();
            }
            break;
        }
        case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
            const auto* d = static_cast<const SIMCONNECT_RECV_SIMOBJECT_DATA*>(data);
            if(d->dwRequestID==kReqPosition && d->dwDefineCount==4 && data->dwSize>=offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA,dwData)+4*sizeof(double)){
                double v[4];std::memcpy(v,&d->dwData,sizeof(v));staged_.push_back(backend_event::AircraftPosition{v[0],v[1],v[2],v[3]});
            }
            if (d->dwRequestID == kReqMotion && d->dwDefineCount == 7 && data->dwSize >= offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA, dwData) + 7 * sizeof(double)) {
                double v[7]; std::memcpy(v, &d->dwData, sizeof(v));
                staged_.push_back(backend_event::MotionTelemetry{{v[0], v[1], v[2], v[3], v[4], v[5], v[6]}});
            }
            if (d->dwRequestID == kReqTitle || d->dwRequestID == kReqTitleOnce) {
                const char* title = reinterpret_cast<const char*>(&d->dwData);
                staged_.push_back(backend_event::AircraftTitle{std::string(title, strnlen(title, 256))});
            }
            break;
        }
        case SIMCONNECT_RECV_ID_CONTROLLERS_LIST: {
            const auto* l = static_cast<const SIMCONNECT_RECV_CONTROLLERS_LIST*>(data);
            backend_event::ControllersList list;
            for (DWORD i = 0; i < l->dwArraySize; ++i)
                list.controllers.emplace_back(static_cast<int>(l->rgData[i].DeviceId), std::string(l->rgData[i].DeviceName, strnlen(l->rgData[i].DeviceName, 256)));
            staged_.push_back(std::move(list));
            break;
        }
        default: break;
    }
}

}  // namespace fscs::host
