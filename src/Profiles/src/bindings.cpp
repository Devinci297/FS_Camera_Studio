#include "fscs/profiles/bindings.hpp"

#include "fscs/profiles/json_util.hpp"
#include "fscs/profiles/profile.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace fscs::profiles {

namespace {

constexpr std::array<std::pair<Action, const char*>, 27> kActions{{
    {Action::SaveViews, "saveViews"},
    {Action::ToggleHeadTracking, "toggleHeadTracking"}, {Action::ToggleAirportAuto, "toggleAirportAuto"},
    {Action::SelectView, "selectView"},       {Action::PeekView, "peekView"},
    {Action::NextCamera, "nextCamera"},       {Action::PreviousCamera, "previousCamera"},
    {Action::NextCategory, "nextCategory"},   {Action::ReturnToPilot, "returnToPilot"},
    {Action::ReleaseCamera, "releaseCamera"}, {Action::RecenterLook, "recenterLook"},
    {Action::ToggleEditor, "toggleEditor"},   {Action::ToggleMini, "toggleMini"}, {Action::ToggleMouseLook, "toggleMouseLook"},
    {Action::LookLeft, "lookLeft"},           {Action::LookRight, "lookRight"},
    {Action::LookUp, "lookUp"},               {Action::LookDown, "lookDown"},
    {Action::MoveForward, "moveForward"},     {Action::MoveBack, "moveBack"},
    {Action::MoveLeft, "moveLeft"},           {Action::MoveRight, "moveRight"},
    {Action::MoveUp, "moveUp"},               {Action::MoveDown, "moveDown"},
    {Action::MoveFast, "moveFast"}, {Action::ZoomIn, "zoomIn"},               {Action::ZoomOut, "zoomOut"},
}};

const char* povName(PovDirection d) {
    switch (d) {
        case PovDirection::Up: return "up";
        case PovDirection::Right: return "right";
        case PovDirection::Down: return "down";
        case PovDirection::Left: return "left";
        case PovDirection::None: break;
    }
    return "";
}

PovDirection parsePov(const std::string& s) {
    if (s == "up") return PovDirection::Up;
    if (s == "right") return PovDirection::Right;
    if (s == "down") return PovDirection::Down;
    if (s == "left") return PovDirection::Left;
    return PovDirection::None;
}

bool isModifier(const std::string& t) {
    return t == "VK_LCONTROL" || t == "VK_RCONTROL" || t == "VK_LSHIFT" || t == "VK_RSHIFT" || t == "VK_LMENU" || t == "VK_RMENU";
}

bool isKeyToken(const std::string& t) {
    if (t.size() == 1) return std::isupper(static_cast<unsigned char>(t[0])) || std::isdigit(static_cast<unsigned char>(t[0]));
    if (t.size() < 4 || t.compare(0, 3, "VK_") != 0) return false;
    return std::all_of(t.begin() + 3, t.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == 'x'; });
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool allDigits(const std::string& s) { return !s.empty() && s.size() <= 4 && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }); }

}  // namespace

const char* toString(Action a) {
    for (const auto& [act, name] : kActions)
        if (act == a) return name;
    return "nextCamera";
}

std::optional<Action> parseAction(const std::string& s) {
    for (const auto& [act, name] : kActions)
        if (s == name) return act;
    return std::nullopt;
}

bool actionTakesViewId(Action a) { return a == Action::SelectView || a == Action::PeekView; }

bool actionIsContinuous(Action a) {
    switch (a) {
        case Action::LookLeft: case Action::LookRight: case Action::LookUp: case Action::LookDown:
        case Action::MoveForward: case Action::MoveBack: case Action::MoveLeft: case Action::MoveRight:
        case Action::MoveFast: case Action::MoveUp: case Action::MoveDown: case Action::ZoomIn: case Action::ZoomOut:
            return true;
        default: return false;
    }
}

bool validDefinition(const std::string& d) {
    if (d.empty() || d.size() > 64) return false;
    if (d == "sim:ZOOM_IN" || d == "sim:ZOOM_OUT") return true;
    if (d == "VK_SHIFT" || d == "VK_LSHIFT" || d == "VK_RSHIFT") return true;
    if (d.compare(0, 9, "joystick:") == 0) {
        const auto parts = split(d, ':');
        if (parts.size() != 4 || !allDigits(parts[1]) || !allDigits(parts[3])) return false;
        return parts[2] == "button" || parts[2] == "POV";
    }
    const auto tokens = split(d, '+');
    int modifiers = 0, keys = 0;
    for (const auto& t : tokens) {
        if (isModifier(t)) {
            if (keys > 0) return false;  // modifiers come first
            ++modifiers;
        } else if (isKeyToken(t)) {
            ++keys;
        } else {
            return false;
        }
    }
    return modifiers <= 2 && keys >= 1 && keys <= 2;
}

json InputSpec::toJson() const {
    if (kind == InputKind::SimZoom) return {{"kind", "simZoom"}, {"definition", definition}, {"label", label}};
    if (kind == InputKind::Key) return {{"kind", "key"}, {"definition", definition}, {"label", label}};
    json j{{"kind", "joystick"}, {"definition", definition}, {"label", label}, {"deviceId", deviceId}, {"deviceName", deviceName}};
    if (isPov()) j["pov"] = povName(pov);
    return j;
}

std::optional<InputSpec> InputSpec::fromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;
    InputSpec s;
    const std::string kind = js::str(j, "kind");
    s.definition = js::str(j, "definition");
    s.label = js::str(j, "label", s.definition);
    if (!validDefinition(s.definition) || s.label.size() > 120 || !js::validUtf8(s.label)) return std::nullopt;
    if (s.definition.compare(0, 4, "sim:") == 0) {
        if (kind != "simZoom") return std::nullopt;
        s.kind = InputKind::SimZoom;
        return s;
    }
    if (kind == "simZoom") return std::nullopt;
    const bool joystick = s.definition.compare(0, 9, "joystick:") == 0;
    if (kind == "key" && !joystick) {
        s.kind = InputKind::Key;
        return s;
    }
    if (kind != "joystick" || !joystick) return std::nullopt;
    s.kind = InputKind::Joystick;
    s.deviceId = static_cast<int>(js::integer(j, "deviceId", -1));
    s.deviceName = js::str(j, "deviceName", "Controller");
    if (s.deviceName.size() > 120 || !js::validUtf8(s.deviceName)) return std::nullopt;
    const bool pov = s.definition.find(":POV:") != std::string::npos;
    s.pov = parsePov(js::str(j, "pov"));
    if (pov != s.isPov()) return std::nullopt;
    return s;
}

BindingSet parseBindings(const json& doc) {
    BindingSet set;
    if (js::integer(doc, "schemaVersion", 0) != kBindingsSchemaVersion || !js::get(doc, "bindings").is_array()) {
        set.problems.push_back("The bindings document is not a version 2 bindings document.");
        return set;
    }
    for (const auto& b : doc["bindings"]) {
        auto action = parseAction(js::str(b, "action"));
        auto input = InputSpec::fromJson(js::get(b, "input"));
        if (!action || !input) {
            set.problems.push_back("Skipped an unreadable binding.");
            continue;
        }
        Binding out{*action, 0, *input};
        if (input->kind == InputKind::SimZoom && *action != Action::ZoomIn && *action != Action::ZoomOut) {
            set.problems.push_back("Simulator zoom inputs can only control zoom in or out.");
            continue;
        }
        if (actionTakesViewId(*action)) {
            const auto viewId = js::integer(b, "viewId", 0);
            if (viewId < 1 || viewId > kMaxViewId) {
                set.problems.push_back("Skipped a view binding without a valid view ID.");
                continue;
            }
            out.viewId = static_cast<int>(viewId);
        }
        const bool clash = std::any_of(set.bindings.begin(), set.bindings.end(), [&](const Binding& e) { return e.input.sameInput(out.input); });
        if (clash) {
            set.problems.push_back(out.input.label + " was assigned twice; the first use was kept.");
            continue;
        }
        set.bindings.push_back(std::move(out));
    }
    return set;
}

json upgradeMovementBindings(json doc) {
    if (!parseBindings(doc).problems.empty()) return doc;
    if (js::integer(doc, "saveViewsDefaultsVersion", 0) < 1) {
        bool used = false;
        for (const auto& b : js::array(doc,"bindings"))
            used = used || js::str(b,"action")=="saveViews" || js::str(js::object(b,"input"),"definition")=="VK_LCONTROL+S";
        if (!used) doc["bindings"].push_back({{"action","saveViews"},{"input",{{"kind","key"},{"definition","VK_LCONTROL+S"},{"label","Ctrl + S"}}}});
        doc["saveViewsDefaultsVersion"] = 1;
    }
    const auto version = js::integer(doc, "movementDefaultsVersion", 0);
    if (version >= 2 || !parseBindings(doc).problems.empty()) return doc;
    if (version == 1) {
        // Only migrate the complete, unmodified stock pair. Custom bindings and deliberate removals win.
        int up = -1, down = -1, verticalCount = 0;
        for (std::size_t i = 0; i < doc["bindings"].size(); ++i) {
            const auto& binding = doc["bindings"][i];
            const auto action = js::str(binding, "action");
            if (action != "moveUp" && action != "moveDown") continue;
            ++verticalCount;
            const auto& input = js::object(binding, "input");
            if (js::str(input, "kind") != "key") continue;
            const auto key = js::str(input, "definition");
            if (js::str(input, "label", key) != key) continue;
            if (action == "moveDown" && key == "Q") down = static_cast<int>(i);
            if (action == "moveUp" && key == "E") up = static_cast<int>(i);
        }
        if (verticalCount == 2 && up >= 0 && down >= 0) {
            doc["bindings"][down]["action"] = "moveUp";
            doc["bindings"][up]["action"] = "moveDown";
        }
    } else {
        for (const auto& entry : std::vector<std::pair<const char*, const char*>>{
            {"moveForward", "W"}, {"moveBack", "S"}, {"moveLeft", "A"}, {"moveRight", "D"},
            {"moveUp", "Q"}, {"moveDown", "E"}, {"moveFast", "VK_SHIFT"},
            {"zoomIn", "sim:ZOOM_IN"}, {"zoomOut", "sim:ZOOM_OUT"}}) {
            bool used = false;
            for (const auto& b : doc["bindings"])
                used = used || js::str(b, "action") == entry.first || js::str(js::get(b, "input"), "definition") == entry.second;
            if (used) continue;
            const std::string def = entry.second;
            const bool zoom = def.compare(0, 4, "sim:") == 0;
            const std::string label = zoom ? (def == "sim:ZOOM_IN" ? "Mouse wheel up / MSFS zoom in" : "Mouse wheel down / MSFS zoom out") : def == "VK_SHIFT" ? "Shift" : def;
            doc["bindings"].push_back({{"action", entry.first}, {"input", {{"kind", zoom ? "simZoom" : "key"}, {"definition", def}, {"label", label}}}});
        }
    }
    doc["movementDefaultsVersion"] = 2;
    return doc;
}

json defaultBindingsDocument() {
    auto key = [](const std::string& k, const std::string& label) {
        return json{{"kind", "key"}, {"definition", "VK_LCONTROL+VK_LMENU+" + k}, {"label", "Ctrl + Alt + " + label}};
    };
    json list = json::array();
    for (int v = 1; v <= 9; ++v) list.push_back({{"action", "selectView"}, {"viewId", v}, {"input", key(std::to_string(v), std::to_string(v))}});
    const std::pair<const char*, std::pair<const char*, const char*>> rest[] = {
        {"returnToPilot", {"P", "P"}},   {"nextCamera", {"N", "N"}},   {"previousCamera", {"B", "B"}},
        {"releaseCamera", {"R", "R"}},   {"toggleEditor", {"E", "E"}}, {"toggleMini", {"M", "M"}},
        {"recenterLook", {"C", "C"}},    {"lookLeft", {"VK_LEFT", "Left arrow"}}, {"lookRight", {"VK_RIGHT", "Right arrow"}},
        {"lookUp", {"VK_UP", "Up arrow"}}, {"lookDown", {"VK_DOWN", "Down arrow"}}};
    for (const auto& [action, k] : rest) list.push_back({{"action", action}, {"input", key(k.first, k.second)}});
    return upgradeMovementBindings({{"schemaVersion", kBindingsSchemaVersion}, {"bindings", list}});
}

json defaultSettingsDocument() {
    return {{"schemaVersion", kSettingsSchemaVersion},
            {"ui", {{"shortcuts", true}, {"contrast", false}, {"backups", true}, {"remember", true}, {"precision", "normal"},
                    {"theme", "dark"}, {"reducedMotion", false}, {"miniEnabled", true}, {"miniHover", true}, {"miniPinned", false}, {"miniOpacity", 0.65}, {"mainOpacity", 1.0}, {"nameDisplay", "both"}}},
            {"host", {{"allowUnqualifiedAdapter", false}, {"lookSpeedDegPerSec", 60.0}, {"moveSpeedMetersPerSec", 0.5}, {"airportRadiusNm", 25.0}, {"mouseSensitivity", 1.0}, {"mouseSmoothingMs", 55.0}, {"mouseLookEnabled", true}, {"mouseInvertY", false}, {"globalHeadTrackingEnabled", false}, {"globalAircraftTracking", "both"}}}};
}

json normalizeSettings(const json& doc) {
    json out = defaultSettingsDocument();
    const json& ui = js::object(doc, "ui");
    out["ui"]["miniOpacity"] = std::clamp(js::num(ui, "miniOpacity", 0.65), 0.0, 1.0);
    out["ui"]["mainOpacity"] = std::clamp(js::num(ui, "mainOpacity", 1.0), 0.0, 1.0);
    const auto display = js::str(ui, "nameDisplay", "both");
    out["ui"]["nameDisplay"] = display == "icons" || display == "labels" ? display : "both";
    for (const char* k : {"shortcuts", "contrast", "backups", "remember", "reducedMotion", "miniEnabled", "miniHover", "miniPinned"})
        out["ui"][k] = js::boolean(ui, k, out["ui"][k].get<bool>());
    const std::string precision = js::str(ui, "precision", "normal");
    out["ui"]["precision"] = (precision == "fine" || precision == "coarse") ? precision : "normal";
    const std::string theme = js::str(ui, "theme", "dark");
    out["ui"]["theme"] = (theme == "light" || theme == "sim") ? theme : "dark";
    const HostSettings h = parseHostSettings(doc);
    out["host"] = {{"allowUnqualifiedAdapter", h.allowUnqualifiedAdapter}, {"lookSpeedDegPerSec", h.lookSpeedDegPerSec},
                   {"moveSpeedMetersPerSec", h.moveSpeedMetersPerSec}, {"airportRadiusNm", h.airportRadiusNm}, {"mouseSensitivity", h.mouseSensitivity},
                   {"mouseSmoothingMs", h.mouseSmoothingMs}, {"mouseLookEnabled", h.mouseLookEnabled}, {"mouseInvertY", h.mouseInvertY},
                   {"globalHeadTrackingEnabled", h.globalHeadTrackingEnabled}, {"globalAircraftTracking", h.globalAircraftTracking}};
    return out;
}

HostSettings parseHostSettings(const json& doc) {
    HostSettings s;
    const json& host = js::object(doc, "host");
    s.allowUnqualifiedAdapter = js::boolean(host, "allowUnqualifiedAdapter", false);
    s.airportRadiusNm=std::clamp(js::num(host,"airportRadiusNm",25),5.0,100.0);
    s.mouseSensitivity = std::clamp(js::num(host, "mouseSensitivity", 1.0), 0.1, 4.0);
    s.mouseSmoothingMs = std::clamp(js::num(host, "mouseSmoothingMs", 55.0), 0.0, 250.0);
    s.mouseLookEnabled = js::boolean(host, "mouseLookEnabled", true);
    s.globalHeadTrackingEnabled = js::boolean(host, "globalHeadTrackingEnabled", false);
    const auto aircraftTracking = js::str(host,"globalAircraftTracking","both");
    s.globalAircraftTracking = aircraftTracking=="off" || aircraftTracking=="heading" ? aircraftTracking : "both";
    s.mouseInvertY = js::boolean(host, "mouseInvertY", false);
    s.lookSpeedDegPerSec = std::clamp(js::num(host, "lookSpeedDegPerSec", 60.0), 5.0, 360.0);
    s.moveSpeedMetersPerSec = std::clamp(js::num(host, "moveSpeedMetersPerSec", 0.5), 0.05, 20.0);
    const auto precision = js::str(js::object(doc, "ui"), "precision", "normal");
    s.movementPrecisionScale = precision == "fine" ? 0.1 : precision == "coarse" ? 10.0 : 1.0;
    return s;
}

}  // namespace fscs::profiles
