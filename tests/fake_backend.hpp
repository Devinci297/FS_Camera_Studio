#pragma once
// Fakes for the module's adapters (design 2.0 §13.2): record every call so tests can assert ordering and the
// absence of writes, and let tests deliver simulator callbacks with arbitrary delay or reordering.

#include "backend.hpp"
#include "fscs/profiles/repository.hpp"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fscs::host {

class FakeBackend final : public ICameraBackend {
public:
    bool open = false;
    std::vector<std::string> calls;  // "acquire", "release", "submit", "flag:interaction:on", "get", ...
    std::vector<SdkPose> submitted;
    std::deque<BackendEvent> inbox;

    bool tryConnect() override {
        calls.push_back("connect");
        open = true;
        return true;
    }
    void disconnect() override {
        calls.push_back("disconnect");
        open = false;
    }
    bool isOpen() const override { return open; }
    void dispatch(const std::function<void(BackendEvent)>& sink) override {
        while (!inbox.empty()) {
            auto ev = std::move(inbox.front());
            inbox.pop_front();
            sink(std::move(ev));
        }
    }
    bool requestStatus() override { calls.push_back("status"); return open; }
    bool acquire(const std::string&) override { calls.push_back("acquire"); return open; }
    bool release(const std::string&) override { calls.push_back("release"); return open; }
    bool submitPose(const SdkPose& p) override {
        calls.push_back("submit");
        submitted.push_back(p);
        return open;
    }
    bool setFlag(CameraFlag f, bool on) override {
        calls.push_back(std::string("flag:") + (f == CameraFlag::Interaction ? "interaction" : "ground") + (on ? ":on" : ":off"));
        return open;
    }
    bool requestCameraData(Referential) override { calls.push_back("get"); return open; }
    bool enumerateDefinitions() override { calls.push_back("enumerate"); return open; }
    bool setUsingDefinition(const std::string& n) override { calls.push_back("definition:" + n); return open; }

    std::size_t count(const std::string& call) const {
        std::size_t n = 0;
        for (const auto& c : calls) n += c == call;
        return n;
    }
};

class FakeServices final : public IModuleServices {
public:
    std::vector<std::string> sent;              // raw CommBus payloads to panels
    std::vector<std::string> mapped;            // latest mapped definitions
    std::string testInput;
    int controllerRequests = 0;

    bool sendToPanels(const std::string& text) override {
        sent.push_back(text);
        return true;
    }
    bool mapInputs(const std::vector<std::string>& defs) override {
        mapped = defs;
        return true;
    }
    bool mapTestInput(const std::string& d) override {
        testInput = d;
        return true;
    }
    bool enumerateControllers() override {
        ++controllerRequests;
        return true;
    }
};

// In-memory storage. `failNextWrite` simulates a full disk; `tearNextWrite` stores only a prefix (power loss);
// `deferred` holds callbacks until `runDeferred()` (asynchronous IO).
class FakeStorage final : public profiles::IStorageDriver {
public:
    std::map<std::string, std::string> files;
    bool failNextWrite = false;
    bool tearNextWrite = false;
    bool deferred = false;
    std::vector<std::function<void()>> queue;
    int writes = 0;

    void writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) override {
        auto op = [this, path, bytes = std::move(bytes), done]() mutable {
            ++writes;
            if (failNextWrite) {
                failNextWrite = false;
                done(false, "disk full");
                return;
            }
            if (tearNextWrite) {
                tearNextWrite = false;
                files[path] = bytes.substr(0, bytes.size() / 2);
                done(true, {});  // the device claims success; readback must catch it
                return;
            }
            files[path] = std::move(bytes);
            done(true, {});
        };
        if (deferred) queue.push_back(std::move(op));
        else op();
    }
    void readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) override {
        auto op = [this, path, done] {
            auto it = files.find(path);
            done(it == files.end() ? std::nullopt : std::optional<std::string>(it->second));
        };
        if (deferred) queue.push_back(op);
        else op();
    }
    void runDeferred() {
        while (!queue.empty()) {
            auto q = std::move(queue);
            queue.clear();
            for (auto& f : q) f();
        }
    }
};

}  // namespace fscs::host
