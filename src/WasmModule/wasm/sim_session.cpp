#include "sim_session.hpp"

#include <MSFS/MSFS.h>
#include <MSFS/MSFS_CommBus.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <SimConnect.h>

#include <cstdio>
#include <cstring>

namespace fscs::host {

namespace {

enum : DWORD { kEvtAircraftLoaded = 1, kEvtFlightLoaded, kEvtSim, kEvtPause, kEvtTest = 900, kEvtInputBase = 1000 };
enum : DWORD { kReqAircraftState = 1, kReqTitle, kReqTitleOnce };
enum : DWORD { kDefTitle = 1 };
enum : DWORD { kInputGroup = 1, kTestInputGroup = 2 };
enum : DWORD { kNotifyInput = 1 };
constexpr std::size_t kMaxInputs = 256;

void CALLBACK dispatchProc(SIMCONNECT_RECV* data, DWORD size, void* ctx) { static_cast<SimSession*>(ctx)->onRecv(data, size); }

bool isPovDefinition(const std::string& d) { return d.find(":POV:") != std::string::npos; }

// Under WASM a SimConnect HANDLE is an integer; the header keeps it as unsigned long long.
HANDLE asHandle(unsigned long long v) { return static_cast<HANDLE>(v); }

}  // namespace

bool SimSession::open() {
    if (handle_) return true;
    HANDLE h = 0;
    if (FAILED(SimConnect_Open(&h, "FS Camera Studio", nullptr, 0, 0, 0))) return false;
    handle_ = static_cast<unsigned long long>(h);
    setup();
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
    SimConnect_MapClientEventToSimEvent(h, kEvtTest, "");
    SimConnect_AddClientEventToNotificationGroup(h, kNotifyInput, kEvtTest, FALSE);
    SimConnect_SetNotificationGroupPriority(h, kNotifyInput, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
    clientEventsDefined_ = 0;
}

void SimSession::close() {
    if (!handle_) return;
    SimConnect_Close(static_cast<HANDLE>(handle_));
    handle_ = 0;
}

void SimSession::dispatch(std::vector<BackendEvent>& out) {
    if (handle_) SimConnect_CallDispatch(asHandle(handle_), dispatchProc, this);
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
        SimConnect_AddClientEventToNotificationGroup(asHandle(handle_), kNotifyInput, id, FALSE);
    }
    return true;
}

bool SimSession::mapInputs(const std::vector<std::string>& definitions) {
    if (!handle_) return false;
    const std::size_t n = definitions.size() < kMaxInputs ? definitions.size() : kMaxInputs;
    SimConnect_ClearInputGroup(asHandle(handle_), kInputGroup);
    mapped_.assign(definitions.begin(), definitions.begin() + static_cast<long>(n));
    isPov_.clear();
    if (!ensureClientEvents(n)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const DWORD id = kEvtInputBase + static_cast<DWORD>(i);
        const bool pov = isPovDefinition(mapped_[i]);
        isPov_.push_back(pov);
        // Buttons and keys: down sends 1, up sends 0. POV: one event per change with the angle in dwData.
        if (pov) SimConnect_MapInputEventToClientEvent_EX1(asHandle(handle_), kInputGroup, mapped_[i].c_str(), id);
        else SimConnect_MapInputEventToClientEvent_EX1(asHandle(handle_), kInputGroup, mapped_[i].c_str(), id, 1, id, 0);
    }
    SimConnect_SetInputGroupPriority(asHandle(handle_), kInputGroup, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
    SimConnect_SetInputGroupState(asHandle(handle_), kInputGroup, SIMCONNECT_STATE_ON);
    return true;
}

bool SimSession::mapTestInput(const std::string& definition) {
    if (!handle_) return false;
    if (testMapped_) SimConnect_ClearInputGroup(asHandle(handle_), kTestInputGroup);
    testMapped_ = false;
    if (definition.empty()) return true;
    testIsPov_ = isPovDefinition(definition);
    const HRESULT hr = testIsPov_ ? SimConnect_MapInputEventToClientEvent_EX1(asHandle(handle_), kTestInputGroup, definition.c_str(), kEvtTest)
                                  : SimConnect_MapInputEventToClientEvent_EX1(asHandle(handle_), kTestInputGroup, definition.c_str(), kEvtTest, 1, kEvtTest, 0);
    if (FAILED(hr)) return false;
    SimConnect_SetInputGroupPriority(asHandle(handle_), kTestInputGroup, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
    SimConnect_SetInputGroupState(asHandle(handle_), kTestInputGroup, SIMCONNECT_STATE_ON);
    testMapped_ = true;
    return true;
}

void SimSession::requestTitle() {
    // The periodic CHANGED request may have reported the title before the aircraft was known; ask once more.
    if (handle_) SimConnect_RequestDataOnSimObject(asHandle(handle_), kReqTitleOnce, kDefTitle, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_ONCE);
}

bool SimSession::enumerateControllers() { return handle_ && SUCCEEDED(SimConnect_EnumerateControllers(asHandle(handle_))); }

void SimSession::onRecv(const void* raw, unsigned long) {
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
            } else if (e->uEventID == kEvtTest) {
                staged_.push_back(backend_event::InputEvent{0, testIsPov_ || e->dwData == 1, e->dwData, true});
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
