#pragma once
// XP Camera Studio: airports for the module from X-Plane's apt.dat files, as the MSFS module gets them from SimConnect
// facilities (no XPLM calls, so it is tested on its own).
//
//   Database   indexes every airport header once, on a worker thread (Global Airports is a few hundred MB), then
//              answers nearby lists from memory and reads one airport's block per request, also on that thread.
//   facility() one apt.dat airport as the module's facility JSON: runways (rows 100 and 101), the tower viewpoint
//              (14), startup locations as parking (1300/1301, older 15), the taxi network (1201/1202) and signs (20).
//
// Priority is X-Plane's own: the first enabled Custom Scenery pack in scenery_packs.ini that has an airport wins, and
// Global Airports sits where *GLOBAL_AIRPORTS* is listed (last when it is not).

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fscs::xplane::apt {

struct Entry {
    std::string id, region, name;  // id: the row-1 airport code, upper case
    double lat = 0, lon = 0, elevation = 0;  // the airport reference point (datum, else runway centroid), meters MSL
    int kind = 1;  // the header row: 1 airport, 16 seaplane base, 17 heliport
    std::uint32_t file = 0;
    std::uint64_t offset = 0;  // where its header line starts
};

// Reads a file line by line in large blocks, with each line's byte offset.
class LineReader {
public:
    explicit LineReader(const std::filesystem::path& path, std::uint64_t offset = 0);
    bool ok() const { return file_ != nullptr; }
    bool next(std::string_view& line, std::uint64_t& offset);
    ~LineReader();
    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

private:
    std::FILE* file_ = nullptr;
    std::vector<char> buffer_;
    std::size_t begin_ = 0, end_ = 0;
    std::uint64_t bufferOffset_ = 0;
    bool eof_ = false;
    std::string carry_;
};

// apt.dat files in priority order.
std::vector<std::filesystem::path> sceneryFiles(const std::filesystem::path& xplaneRoot);
// Every airport header in one file.
void index(const std::filesystem::path& path, std::uint32_t file, std::vector<Entry>& out);
// One airport's block as facility JSON (summary: without the taxi network and signs); null when it is not there.
nlohmann::json facility(const std::filesystem::path& path, const Entry& entry, bool summary);
// A taxi sign's text without its markup: "{@Y}A{@R}16R-34L" reads "A 16R-34L".
std::string signLabel(std::string_view text);

class Database {
public:
    struct Result {
        std::uint32_t requestId = 0;
        nlohmann::json facility;
        std::string error;
    };

    ~Database() { stop(); }
    void start(std::filesystem::path xplaneRoot);
    void stop();
    bool ready() const { return ready_.load(); }
    std::size_t airports() const { return ready() ? byId_.size() : 0; }
    std::size_t files() const { return ready() ? files_.size() : 0; }
    // AirportList entries within radiusNm, nearest first (heliports left out). Only once ready().
    nlohmann::json nearby(double lat, double lon, double radiusNm, std::size_t limit) const;
    void request(std::string code, std::uint32_t requestId, bool summary);
    std::vector<Result> take();

private:
    struct Job {
        std::string code;
        std::uint32_t requestId = 0;
        bool summary = false;
    };
    void run(std::filesystem::path root);

    std::thread worker_;
    std::atomic<bool> ready_{false}, stopping_{false};
    // Written by the worker before ready_, read-only after it.
    std::vector<std::filesystem::path> files_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> byId_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
};

}  // namespace fscs::xplane::apt
