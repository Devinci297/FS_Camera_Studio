#include "fscs/profiles/bindings.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace fscs::profiles {

namespace {

constexpr std::array<std::pair<Action, const char*>, 22> kActions{{
    {Action::SelectView, "selectView"},       {Action::PeekView, "peekView"},
    {Action::NextCamera, "nextCamera"},       {Action::PreviousCamera, "previousCamera"},
    {Action::NextCategory, "nextCategory"},   {Action::ReturnToPilot, "returnToPilot"},
    {Action::ReleaseCamera, "releaseCamera"}, {Action::RecenterLook, "recenterLook"},
    {Action::ToggleEditor, "toggleEditor"},   {Action::ToggleMini, "toggleMini"},
    {Action::LookLeft, "lookLeft"},           {Action::LookRight, "lookRight"},
    {Action::LookUp, "lookUp"},               {Action::LookDown, "lookDown"},
    {Action::MoveForward, "moveForward"},     {Action::MoveBack, "moveBack"},
    {Action::MoveLeft, "moveLeft"},           {Action::MoveRight, "moveRight"},
    {Action::MoveUp, "moveUp"},               {Action::MoveDown, "moveDown"},
    {Action::ZoomIn, "zoomIn"},               {Action::ZoomOut, "zoomOut"},
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
        case Action::MoveUp: case Action::MoveDown: case Action::ZoomIn: case Action::ZoomOut:
            return true;
        default: return false;
    }
}

bool validDefinition(const std::string& d) {
    if (d.empty() || d.size() > 64) return false;
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
        if (actionTakesViewId(*action)) {
            out.viewId = static_cast<int>(js::integer(b, "viewId", 0));
            if (out.viewId < 1 || out.viewId > 20) {
                set.problems.push_back("Skipped a view binding without a valid view ID.");
                continue;
            }
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
    return {{"schemaVersion", kBindingsSchemaVersion}, {"bindings", list}};
}

json defaultSettingsDocument() {
    return {{"schemaVersion", kSettingsSchemaVersion},
            {"ui", {{"shortcuts", true}, {"contrast", false}, {"backups", true}, {"remember", true}, {"precision", "normal"},
                    {"theme", "dark"}, {"reducedMotion", false}, {"miniEnabled", true}, {"miniHover", true}, {"miniPinned", false}}},
            {"host", {{"allowUnqualifiedAdapter", false}, {"lookSpeedDegPerSec", 60.0}, {"moveSpeedMetersPerSec", 0.5}}}};
}

json normalizeSettings(const json& doc) {
    json out = defaultSettingsDocument();
    const json& ui = js::object(doc, "ui");
    for (const char* k : {"shortcuts", "contrast", "backups", "remember", "reducedMotion", "miniEnabled", "miniHover", "miniPinned"})
        out["ui"][k] = js::boolean(ui, k, out["ui"][k].get<bool>());
    const std::string precision = js::str(ui, "precision", "normal");
    out["ui"]["precision"] = (precision == "fine" || precision == "coarse") ? precision : "normal";
    const std::string theme = js::str(ui, "theme", "dark");
    out["ui"]["theme"] = (theme == "light" || theme == "sim") ? theme : "dark";
    const HostSettings h = parseHostSettings(doc);
    out["host"] = {{"allowUnqualifiedAdapter", h.allowUnqualifiedAdapter}, {"lookSpeedDegPerSec", h.lookSpeedDegPerSec},
                   {"moveSpeedMetersPerSec", h.moveSpeedMetersPerSec}};
    return out;
}

HostSettings parseHostSettings(const json& doc) {
    HostSettings s;
    const json& host = js::object(doc, "host");
    s.allowUnqualifiedAdapter = js::boolean(host, "allowUnqualifiedAdapter", false);
    s.lookSpeedDegPerSec = std::clamp(js::num(host, "lookSpeedDegPerSec", 60.0), 5.0, 360.0);
    s.moveSpeedMetersPerSec = std::clamp(js::num(host, "moveSpeedMetersPerSec", 0.5), 0.05, 20.0);
    return s;
}

}  // namespace fscs::profiles
