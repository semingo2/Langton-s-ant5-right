#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdlib>
#include <deque>
#include <chrono>
#include <iomanip>
#include <memory>
#include <random>

#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  static void closeSocket(socket_t s) { closesocket(s); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <signal.h>
  using socket_t = int;
  static void closeSocket(socket_t s) { close(s); }
#endif

namespace {
constexpr uint64_t HIGHWAY_PERIOD = 104;
constexpr int HIGHWAY_CYCLES_REQUIRED = 20;
constexpr uint64_t HASH_POLY = 0x9e3779b185ebca87ULL;
constexpr uint64_t HASH_BASE_AX = 0xd6e8feb86659fd93ULL;
constexpr uint64_t HASH_BASE_BY = 0xa5a3564e27f8862bULL;
constexpr uint64_t HASH_BASE_CX = 0x9e3779b185ebca87ULL;
constexpr uint64_t HASH_BASE_DY = 0xc2b2ae3d27d4eb4fULL;

struct Coord {
    int64_t x = 0, y = 0;
    bool operator==(const Coord& o) const noexcept { return x == o.x && y == o.y; }
};
struct CoordHash {
    size_t operator()(const Coord& c) const noexcept {
        uint64_t x = static_cast<uint64_t>(c.x);
        uint64_t y = static_cast<uint64_t>(c.y);
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31;
        y ^= y >> 30; y *= 0xbf58476d1ce4e5b9ULL; y ^= y >> 27; y *= 0x94d049bb133111ebULL; y ^= y >> 31;
        return static_cast<size_t>(x ^ (y + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2)));
    }
};

static uint64_t powMod(uint64_t base, uint64_t exp) {
    uint64_t r = 1;
    while (exp) { if (exp & 1) r *= base; base *= base; exp >>= 1; }
    return r;
}
static uint64_t inverseMod2_64(uint64_t a) {
    uint64_t x = a;
    for (int i = 0; i < 6; ++i) x *= 2 - a * x;
    return x;
}
static uint64_t signedPow(uint64_t base, int64_t exp) {
    if (exp >= 0) return powMod(base, static_cast<uint64_t>(exp));
    uint64_t mag = static_cast<uint64_t>(-(exp + 1)) + 1;
    return powMod(inverseMod2_64(base), mag);
}

struct AxisPowers { uint64_t a, ai, c, ci; };
struct PairPowers { uint64_t b, bi, d, di; };

struct Snapshot {
    Coord p1, p2;
    int d1 = 0, d2 = 0;
};
struct HighwayTrack {
    bool candidate = false;
    bool confirmed = false;
    uint64_t lastMatchLocal = 0;
    uint64_t candidateStartLocal = 0;
    int streakCycles = 0;
    uint64_t startLocal = 0;
    uint64_t startGlobal = 0;
    uint64_t confirmedGlobal = 0;
};
struct LoopFingerprint {
    uint64_t a = 0, b = 0;
    bool operator==(const LoopFingerprint& o) const noexcept { return a == o.a && b == o.b; }
};
static uint64_t mix64(uint64_t z) noexcept {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
struct LoopFingerprintHash {
    size_t operator()(const LoopFingerprint& k) const noexcept {
        return static_cast<size_t>(mix64(k.a ^ ((k.b << 23) | (k.b >> 41))));
    }
};

class Simulation {
public:
    Coord ant[2]{};
    int dir[2] = {0, 0}; // 0 up, 1 right, 2 down, 3 left
    int spacing = 10;
    uint64_t totalSteps = 0;
    uint64_t epochStartStep = 0;

    // The row sets are the source of truth for black/highway cells. Avoid a
    // second full set of Coord objects for the same cells.
    std::unordered_set<Coord, CoordHash> visitedCells;
    std::unordered_map<int64_t, std::unordered_set<int64_t>> blackRows;
    std::unordered_map<int64_t, std::unordered_set<int64_t>> highwayRows;
    uint64_t blackCellCount = 0;
    uint64_t highwayCellCount = 0;
    uint64_t highwayBlackCount = 0;

    // Hashes are normalized to ant 1's current position. Coordinate-power
    // caches use relative offsets only and are capped to a fixed size.
    uint64_t boardHash1 = 0, boardHash2 = 0;
    static constexpr size_t POWER_CACHE_LIMIT = 2048;
    std::unordered_map<int64_t, AxisPowers> xPowers;
    std::unordered_map<int64_t, PairPowers> yPowers;

    // Only the recent 312 turns / 313 positions are needed to discover a 104-step
    // cycle and walk back to its beginning. A short per-candidate path is retained
    // while the 20-cycle confirmation is pending.
    std::deque<Snapshot> snapshots;
    std::array<std::deque<uint8_t>, 2> turns;
    std::array<std::vector<Coord>, 2> candidatePath;
    std::array<HighwayTrack, 2> hw{};
    bool highwayHistoryFinished = false;
    std::unordered_map<LoopFingerprint, uint64_t, LoopFingerprintHash> seenStates;
    bool loopFound = false;
    uint64_t loopFirstStep = 0, loopDetectedStep = 0, loopPeriod = 0;

    Simulation() { reset(10, 0, 0); }

    AxisPowers xPow(int64_t x) {
        auto it = xPowers.find(x); if (it != xPowers.end()) return it->second;
        AxisPowers p{signedPow(HASH_BASE_AX, x), signedPow(HASH_BASE_AX, -x), signedPow(HASH_BASE_CX, x), signedPow(HASH_BASE_CX, -x)};
        if (xPowers.size() < POWER_CACHE_LIMIT) xPowers.emplace(x, p);
        return p;
    }
    PairPowers yPow(int64_t y) {
        auto it = yPowers.find(y); if (it != yPowers.end()) return it->second;
        PairPowers p{signedPow(HASH_BASE_BY, y), signedPow(HASH_BASE_BY, -y), signedPow(HASH_BASE_DY, y), signedPow(HASH_BASE_DY, -y)};
        if (yPowers.size() < POWER_CACHE_LIMIT) yPowers.emplace(y, p);
        return p;
    }
    std::pair<uint64_t,uint64_t> cellTokens(Coord p) {
        const int64_t rx = p.x - ant[0].x;
        const int64_t ry = p.y - ant[0].y;
        auto xp = xPow(rx); auto yp = yPow(ry);
        return {xp.a * yp.b, xp.c * yp.d};
    }
    bool isBlack(Coord p) const {
        auto row = blackRows.find(p.y);
        return row != blackRows.end() && row->second.find(p.x) != row->second.end();
    }
    bool isHighway(Coord p) const {
        auto row = highwayRows.find(p.y);
        return row != highwayRows.end() && row->second.find(p.x) != row->second.end();
    }
    void shiftBoardHashForOrigin(Coord oldOrigin, Coord newOrigin) {
        const int64_t dx = oldOrigin.x - newOrigin.x;
        const int64_t dy = oldOrigin.y - newOrigin.y;
        boardHash1 *= signedPow(HASH_BASE_AX, dx) * signedPow(HASH_BASE_BY, dy);
        boardHash2 *= signedPow(HASH_BASE_CX, dx) * signedPow(HASH_BASE_DY, dy);
    }
    void setBlack(Coord p, bool black) {
        const bool old = isBlack(p); if (old == black) return;
        const auto tok = cellTokens(p);
        if (black) {
            auto& row = blackRows[p.y];
            if (row.insert(p.x).second) ++blackCellCount;
            boardHash1 += tok.first; boardHash2 += tok.second;
            if (isHighway(p)) ++highwayBlackCount;
        } else {
            auto row = blackRows.find(p.y);
            if (row != blackRows.end()) {
                if (row->second.erase(p.x) && blackCellCount) --blackCellCount;
                if (row->second.empty()) blackRows.erase(row);
            }
            boardHash1 -= tok.first; boardHash2 -= tok.second;
            if (isHighway(p) && highwayBlackCount) --highwayBlackCount;
        }
    }
    void addHighwayCell(Coord p) {
        auto& row = highwayRows[p.y];
        if (row.insert(p.x).second) {
            ++highwayCellCount;
            if (isBlack(p)) ++highwayBlackCount;
        }
    }
    void clearHighwayCells() {
        highwayRows.clear(); highwayCellCount = 0; highwayBlackCount = 0;
    }

    void reset(int newSpacing, int d1, int d2) {
        visitedCells.clear(); clearHighwayCells(); blackRows.clear(); blackCellCount = 0;
        boardHash1 = boardHash2 = 0; totalSteps = 0; spacing = std::clamp(newSpacing, 1, 1000000);
        dir[0] = ((d1 % 4) + 4) % 4; dir[1] = ((d2 % 4) + 4) % 4;
        int64_t x1 = -(static_cast<int64_t>(spacing) / 2);
        ant[0] = {x1, 0}; ant[1] = {x1 + spacing, 0};
        visitedCells.insert(ant[0]); visitedCells.insert(ant[1]);
        xPowers.clear(); yPowers.clear();
        startNewAnalysisEpoch();
    }

    void repositionSpacing(int newSpacing) {
        spacing = std::clamp(newSpacing, 1, 1000000);
        const Coord oldOrigin = ant[0];
        int64_t x1 = -(static_cast<int64_t>(spacing) / 2);
        ant[0] = {x1, 0}; ant[1] = {x1 + spacing, 0};
        shiftBoardHashForOrigin(oldOrigin, ant[0]);
        visitedCells.insert(ant[0]); visitedCells.insert(ant[1]);
        startNewAnalysisEpoch();
    }
    void changeDirection(int idx, int d) {
        if (idx < 0 || idx > 1) return;
        dir[idx] = ((d % 4) + 4) % 4;
        startNewAnalysisEpoch();
    }

    void startNewAnalysisEpoch() {
        epochStartStep = totalSteps;
        snapshots.clear(); turns[0].clear(); turns[1].clear();
        candidatePath[0].clear(); candidatePath[1].clear();
        hw = {}; highwayHistoryFinished = false;
        seenStates.clear(); loopFound = false; loopFirstStep = loopDetectedStep = loopPeriod = 0;
        snapshots.push_back({ant[0], ant[1], dir[0], dir[1]});
        rememberLoopState();
    }

    bool repeatPairStart(int idx, uint64_t s, uint64_t currentLocalStep) const {
        const uint64_t p = HIGHWAY_PERIOD;
        if (s + 2*p > currentLocalStep) return false;
        const auto& history = turns[idx];
        if (history.size() < 2*p) return false;
        const uint64_t turnBase = currentLocalStep - history.size();
        if (s < turnBase) return false;
        const size_t off = static_cast<size_t>(s - turnBase);
        if (off + 2*p > history.size()) return false;
        for (size_t j = 0; j < p; ++j) {
            if (history[off+j] != history[off+p+j]) return false;
        }

        const uint64_t snapshotBase = currentLocalStep - (snapshots.size() - 1);
        if (s < snapshotBase) return false;
        const size_t snapOff = static_cast<size_t>(s - snapshotBase);
        if (snapOff + 2*p >= snapshots.size()) return false;
        const Snapshot& a = snapshots[snapOff];
        const Snapshot& b = snapshots[snapOff+p];
        const Snapshot& c = snapshots[snapOff+2*p];
        Coord pa = idx == 0 ? a.p1 : a.p2;
        Coord pb = idx == 0 ? b.p1 : b.p2;
        Coord pc = idx == 0 ? c.p1 : c.p2;
        int da = idx == 0 ? a.d1 : a.d2;
        int db = idx == 0 ? b.d1 : b.d2;
        int dc = idx == 0 ? c.d1 : c.d2;
        const int64_t stepDx = pb.x-pa.x;
        const int64_t stepDy = pb.y-pa.y;
        return (stepDx != 0 || stepDy != 0) && stepDx == pc.x-pb.x && stepDy == pc.y-pb.y && da == db && db == dc;
    }

    void analyseHighways(uint64_t localStep) {
        if (highwayHistoryFinished) {
            if (hw[0].confirmed) addHighwayCell(ant[0]);
            if (hw[1].confirmed) addHighwayCell(ant[1]);
            return;
        }
        const uint64_t oldestTurn = localStep - turns[0].size();
        for (int idx = 0; idx < 2; ++idx) {
            auto& h = hw[idx];
            if (h.confirmed) { addHighwayCell(ant[idx]); continue; }

            // Extend only an active candidate's short path, not an entire run history.
            if (h.candidate) candidatePath[idx].push_back(ant[idx]);
            if (localStep < 2*HIGHWAY_PERIOD) continue;

            if (h.candidate) {
                if (localStep == h.lastMatchLocal + HIGHWAY_PERIOD) {
                    if (repeatPairStart(idx, localStep - 2*HIGHWAY_PERIOD, localStep)) {
                        ++h.streakCycles;
                        h.lastMatchLocal = localStep;
                        if (h.streakCycles >= HIGHWAY_CYCLES_REQUIRED) {
                            h.confirmed = true;
                            h.startGlobal = epochStartStep + h.startLocal;
                            h.confirmedGlobal = totalSteps;
                            for (Coord p : candidatePath[idx]) addHighwayCell(p);
                            std::vector<Coord>().swap(candidatePath[idx]);
                        }
                    } else {
                        h.candidate = false;
                        h.streakCycles = 0;
                        std::vector<Coord>().swap(candidatePath[idx]);
                    }
                }
            } else if (repeatPairStart(idx, localStep - 2*HIGHWAY_PERIOD, localStep)) {
                h.candidate = true;
                h.streakCycles = 2;
                h.lastMatchLocal = localStep;
                uint64_t start = localStep - 2*HIGHWAY_PERIOD;
                // Match the prior implementation's backward phase search, bounded by the ring buffer.
                while (start > oldestTurn && repeatPairStart(idx, start - 1, localStep)) --start;
                h.candidateStartLocal = start;
                h.startLocal = start;
                candidatePath[idx].clear();
                const uint64_t snapBase = localStep - (snapshots.size() - 1);
                const uint64_t first = std::max(start, snapBase);
                for (uint64_t j = first; j <= localStep; ++j) {
                    const Snapshot& snap = snapshots[static_cast<size_t>(j - snapBase)];
                    candidatePath[idx].push_back(idx == 0 ? snap.p1 : snap.p2);
                }
            }
        }
        if (hw[0].confirmed && hw[1].confirmed) {
            highwayHistoryFinished = true;
            std::deque<Snapshot>().swap(snapshots);
            std::deque<uint8_t>().swap(turns[0]);
            std::deque<uint8_t>().swap(turns[1]);
            std::vector<Coord>().swap(candidatePath[0]);
            std::vector<Coord>().swap(candidatePath[1]);
        }
    }

    LoopFingerprint currentLoopKey() {
        const uint64_t relx = static_cast<uint64_t>(ant[1].x - ant[0].x);
        const uint64_t rely = static_cast<uint64_t>(ant[1].y - ant[0].y);
        const uint64_t d = static_cast<uint64_t>(dir[0] | (dir[1] << 2));
        uint64_t a = mix64(boardHash1 ^ mix64(relx) ^ (mix64(rely) << 1) ^ mix64(d));
        uint64_t b = mix64(boardHash2 ^ mix64(relx + 0x632be59bd9b4e019ULL) ^
                           mix64(rely + 0x8cb92baa3f3d8dd7ULL) ^ mix64(d + 0x9e3779b97f4a7c15ULL));
        return {a, b};
    }
    void rememberLoopState() {
        if (loopFound) return;
        const LoopFingerprint key = currentLoopKey();
        auto it = seenStates.find(key);
        if (it != seenStates.end()) {
            loopFound = true; loopFirstStep = it->second; loopDetectedStep = totalSteps; loopPeriod = totalSteps - it->second;
        } else {
            seenStates.emplace(key, totalSteps);
        }
    }

    void stepOne() {
        // Read both cells before either one changes, then flip and move both ants together.
        const bool black0 = isBlack(ant[0]);
        const bool black1 = isBlack(ant[1]);
        const Coord old0 = ant[0], old1 = ant[1];
        const uint8_t t0 = black0 ? 1 : 0;
        const uint8_t t1 = black1 ? 1 : 0;
        dir[0] = (dir[0] + (black0 ? 3 : 1)) % 4;
        dir[1] = (dir[1] + (black1 ? 3 : 1)) % 4;
        if (old0 == old1) setBlack(old0, !black0);
        else { setBlack(old0, !black0); setBlack(old1, !black1); }

        static const int dx[4] = {0, 1, 0, -1};
        static const int dy[4] = {-1, 0, 1, 0};
        ant[0].x += dx[dir[0]]; ant[0].y += dy[dir[0]];
        ant[1].x += dx[dir[1]]; ant[1].y += dy[dir[1]];
        // Rebase the incremental board fingerprint from old ant-1 origin to its new position.
        shiftBoardHashForOrigin(old0, ant[0]);

        ++totalSteps;
        visitedCells.insert(ant[0]); visitedCells.insert(ant[1]);
        if (!highwayHistoryFinished) {
            turns[0].push_back(t0); turns[1].push_back(t1);
            if (turns[0].size() > 3*HIGHWAY_PERIOD) turns[0].pop_front();
            if (turns[1].size() > 3*HIGHWAY_PERIOD) turns[1].pop_front();
            snapshots.push_back({ant[0], ant[1], dir[0], dir[1]});
            if (snapshots.size() > 3*HIGHWAY_PERIOD + 1) snapshots.pop_front();
            analyseHighways(totalSteps - epochStartStep);
        } else {
            analyseHighways(0);
        }
        rememberLoopState();
    }

    void runSteps(uint64_t n) { n = std::min<uint64_t>(n, 5000); for (uint64_t i=0; i<n; ++i) stepOne(); }

    uint64_t nonHighwayBlackCount() const { return blackCellCount >= highwayBlackCount ? blackCellCount - highwayBlackCount : 0; }
    static std::string dirName(int d) { static const char* names[] = {"上", "右", "下", "左"}; return names[(d%4+4)%4]; }
    static std::string jsonEscape(const std::string& s) {
        std::ostringstream o; for (unsigned char c : s) { if (c=='"') o << "\\\""; else if (c=='\\') o << "\\\\"; else if (c=='\n') o << "\\n"; else if (c=='\r') o << "\\r"; else if (c=='\t') o << "\\t"; else if (c < 32) o << ' '; else o << c; } return o.str();
    }
    std::string stateJson() const {
        std::ostringstream o;
        o << "{\"steps\":" << totalSteps
          << ",\"spacing\":" << spacing
          << ",\"ants\":["
          << "{\"x\":" << ant[0].x << ",\"y\":" << ant[0].y << ",\"dir\":" << dir[0] << ",\"highway\":" << (hw[0].confirmed?"true":"false") << ",\"highwayStart\":" << (hw[0].confirmed?std::to_string(hw[0].startGlobal):"null") << ",\"highwayConfirmedAt\":" << (hw[0].confirmed?std::to_string(hw[0].confirmedGlobal):"null") << "},"
          << "{\"x\":" << ant[1].x << ",\"y\":" << ant[1].y << ",\"dir\":" << dir[1] << ",\"highway\":" << (hw[1].confirmed?"true":"false") << ",\"highwayStart\":" << (hw[1].confirmed?std::to_string(hw[1].startGlobal):"null") << ",\"highwayConfirmedAt\":" << (hw[1].confirmed?std::to_string(hw[1].confirmedGlobal):"null") << "}],"
          << "\"blackCells\":" << blackCellCount << ",\"visitedCells\":" << visitedCells.size()
          << ",\"highwayCells\":" << highwayCellCount << ",\"nonHighwayBlackCells\":" << nonHighwayBlackCount()
          << ",\"bothHighway\":" << ((hw[0].confirmed && hw[1].confirmed)?"true":"false")
          << ",\"loop\":{\"found\":" << (loopFound?"true":"false") << ",\"firstStep\":" << (loopFound?std::to_string(loopFirstStep):"null") << ",\"detectedStep\":" << (loopFound?std::to_string(loopDetectedStep):"null") << ",\"period\":" << (loopFound?std::to_string(loopPeriod):"null") << "}"
          << ",\"epochStartStep\":" << epochStartStep << "}";
        return o.str();
    }

    std::string visibleJson(int64_t x0, int64_t y0, int64_t w, int64_t h) const {
        w = std::clamp<int64_t>(w, 1, 400); h = std::clamp<int64_t>(h, 1, 300);
        std::ostringstream o; o << "{\"black\":["; bool first=true;
        for (int64_t y=y0; y<y0+h; ++y) { auto it=blackRows.find(y); if (it==blackRows.end()) continue; for (int64_t x:it->second) if (x>=x0 && x<x0+w) { if(!first)o<<','; first=false; o<<'['<<x<<','<<y<<']'; } }
        o << "],\"highway\":["; first=true;
        for (int64_t y=y0; y<y0+h; ++y) { auto it=highwayRows.find(y); if (it==highwayRows.end()) continue; for (int64_t x:it->second) if (x>=x0 && x<x0+w) { if(!first)o<<','; first=false; o<<'['<<x<<','<<y<<']'; } }
        o << "]}"; return o.str();
    }
};

// Each browser profile receives its own opaque session cookie. Simulation state and
// its mutex are kept per session so visitors cannot affect one another's board.
struct UserSession {
    Simulation sim;
    std::mutex mutex;
    std::atomic<int64_t> lastSeenMs{0};
    UserSession() { touch(); }
    void touch() noexcept {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        lastSeenMs.store(static_cast<int64_t>(now), std::memory_order_relaxed);
    }
};

std::mutex sessionsMutex;
std::unordered_map<std::string, std::shared_ptr<UserSession>> sessions;
constexpr int64_t SESSION_IDLE_LIMIT_MS = 12LL * 60 * 60 * 1000;

static int64_t monotonicMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::string generateSessionId() {
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(),
        static_cast<unsigned int>(monotonicMillis() & 0xffffffff),
        static_cast<unsigned int>((monotonicMillis() >> 32) & 0xffffffff)};
    std::mt19937_64 rng(seed);
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << rng()
        << std::setw(16) << rng();
    return out.str();
}
static bool validSessionId(std::string id) {
    if (id.size() != 32) return false;
    for (char& c : id) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}
static std::string headerValue(const std::string& headers, const std::string& wantedName) {
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    std::istringstream lines(headers);
    std::string line;
    std::getline(lines, line); // request line
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos || lower(line.substr(0, colon)) != lower(wantedName)) continue;
        size_t begin = colon + 1;
        while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin]))) ++begin;
        return line.substr(begin);
    }
    return {};
}
static std::string extractSessionCookie(const std::string& cookieHeader) {
    std::istringstream items(cookieHeader);
    std::string item;
    while (std::getline(items, item, ';')) {
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
        constexpr const char* key = "langtons_ant_session=";
        if (item.rfind(key, 0) == 0) {
            std::string id = item.substr(std::char_traits<char>::length(key));
            if (validSessionId(id)) {
                for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return id;
            }
        }
    }
    return {};
}
static std::shared_ptr<UserSession> getUserSession(const std::string& cookieId,
                                                    std::string& sessionId,
                                                    bool& shouldSetCookie) {
    const int64_t now = monotonicMillis();
    const bool validCookie = !cookieId.empty() && validSessionId(cookieId);
    sessionId = validCookie ? cookieId : generateSessionId();
    for (char& c : sessionId) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    shouldSetCookie = !validCookie;

    std::lock_guard<std::mutex> guard(sessionsMutex);
    for (auto it = sessions.begin(); it != sessions.end();) {
        const int64_t last = it->second->lastSeenMs.load(std::memory_order_relaxed);
        if (now - last > SESSION_IDLE_LIMIT_MS) it = sessions.erase(it);
        else ++it;
    }
    auto it = sessions.find(sessionId);
    if (it == sessions.end()) {
        auto fresh = std::make_shared<UserSession>();
        fresh->touch();
        sessions.emplace(sessionId, fresh);
        return fresh;
    }
    it->second->touch();
    return it->second;
}

static std::string readFile(const std::string& name) {
    std::ifstream f(name, std::ios::binary); if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string getQueryValue(const std::string& query, const std::string& key, const std::string& fallback="0") {
    std::istringstream ss(query); std::string item;
    while (std::getline(ss, item, '&')) { auto p=item.find('='); if (p!=std::string::npos && item.substr(0,p)==key) return item.substr(p+1); }
    return fallback;
}
static long long numQuery(const std::string& q, const std::string& key, long long fallback=0) {
    try { auto v=getQueryValue(q,key,std::to_string(fallback)); size_t n=0; long long x=std::stoll(v,&n); return n?x:fallback; } catch(...) { return fallback; }
}
static long long numJson(const std::string& body, const std::string& key, long long fallback=0) {
    std::string tag="\""+key+"\""; auto p=body.find(tag); if(p==std::string::npos) return fallback; p=body.find(':',p+tag.size()); if(p==std::string::npos) return fallback; ++p; while(p<body.size()&&std::isspace(static_cast<unsigned char>(body[p])))++p;
    try { size_t n=0; long long v=std::stoll(body.substr(p), &n); return n?v:fallback; } catch(...) { return fallback; }
}
static std::string contentType(const std::string& p) {
    if (p.size()>=5 && p.substr(p.size()-5)==".html") return "text/html; charset=utf-8";
    if (p.size()>=4 && p.substr(p.size()-4)==".css") return "text/css; charset=utf-8";
    if (p.size()>=3 && p.substr(p.size()-3)==".js") return "application/javascript; charset=utf-8";
    return "text/plain; charset=utf-8";
}
static void sendResponse(socket_t s, int status, const std::string& type, const std::string& body, const std::string& extraHeaders = {}) {
    std::string reason = status==200?"OK":status==400?"Bad Request":status==404?"Not Found":status==405?"Method Not Allowed":"Internal Server Error";
    std::ostringstream h; h << "HTTP/1.1 "<<status<<' '<<reason<<"\r\nContent-Type: "<<type<<"\r\nContent-Length: "<<body.size()<<"\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n" << extraHeaders << "\r\n";
    std::string head=h.str();
    auto sendBytes=[&](const char* data, size_t len) { size_t sent=0; while(sent<len) { int r=send(s,data+sent,static_cast<int>(std::min<size_t>(len-sent, 1<<20)),0); if(r<=0)break; sent+=static_cast<size_t>(r); } };
    sendBytes(head.data(),head.size()); sendBytes(body.data(),body.size());
}
static std::string mimeSafePath(const std::string& route) {
    if (route=="/" || route=="/index.html") return "index.html";
    if (route=="/style.css") return "style.css";
    if (route=="/app.js") return "app.js";
    return {};
}
static void handleClient(socket_t s) {
    std::string req; req.reserve(8192); char buf[4096]; size_t headerEnd=std::string::npos; size_t expected=0;
    while (req.size() < 2*1024*1024) {
        int n=recv(s,buf,sizeof(buf),0); if(n<=0) { closeSocket(s); return; }
        req.append(buf,static_cast<size_t>(n));
        if (headerEnd==std::string::npos) {
            headerEnd=req.find("\r\n\r\n");
            if (headerEnd!=std::string::npos) {
                std::string header=req.substr(0,headerEnd); auto cp=header.find("Content-Length:");
                if(cp==std::string::npos) cp=header.find("content-length:");
                if(cp!=std::string::npos) { cp=header.find(':',cp)+1; while(cp<header.size()&&std::isspace(static_cast<unsigned char>(header[cp])))++cp; try{expected=static_cast<size_t>(std::stoull(header.substr(cp)));}catch(...){expected=0;} }
            }
        }
        if(headerEnd!=std::string::npos && req.size()>=headerEnd+4+expected) break;
    }
    if(headerEnd==std::string::npos) { sendResponse(s,400,"text/plain; charset=utf-8","Bad request"); closeSocket(s); return; }
    std::string header=req.substr(0,headerEnd); std::string body=req.substr(headerEnd+4, expected);
    std::istringstream hs(header); std::string method,target,version; hs>>method>>target>>version;
    auto qp=target.find('?'); std::string route=target.substr(0,qp), query=qp==std::string::npos?"":target.substr(qp+1);

    const std::string incomingCookie = extractSessionCookie(headerValue(header, "Cookie"));
    std::string sessionId;
    bool shouldSetCookie = false;
    auto userSession = getUserSession(incomingCookie, sessionId, shouldSetCookie);
    std::string responseHeaders;
    if (shouldSetCookie) {
        const bool secure = headerValue(header, "X-Forwarded-Proto") == "https";
        responseHeaders = "Set-Cookie: langtons_ant_session=" + sessionId +
            "; Path=/; HttpOnly; SameSite=Lax; Max-Age=604800" +
            (secure ? "; Secure" : "") + "\r\n";
    }
    auto respond = [&](int status, const std::string& type, const std::string& content) {
        sendResponse(s, status, type, content, responseHeaders);
    };
    if(method=="GET" && route.rfind("/api/state",0)==0) {
        std::lock_guard<std::mutex> lock(userSession->mutex); respond(200,"application/json; charset=utf-8",userSession->sim.stateJson());
    } else if(method=="GET" && route=="/api/visible") {
        int64_t x0=numQuery(query,"x0"),y0=numQuery(query,"y0"),w=numQuery(query,"w",80),h=numQuery(query,"h",45);
        std::lock_guard<std::mutex> lock(userSession->mutex); respond(200,"application/json; charset=utf-8",userSession->sim.visibleJson(x0,y0,w,h));
    } else if(method=="POST" && route=="/api/step") {
        uint64_t n=static_cast<uint64_t>(std::clamp<long long>(numJson(body,"count",1),1,5000));
        int64_t x0=numQuery(query,"x0"), y0=numQuery(query,"y0");
        int64_t w=numQuery(query,"w",80), h=numQuery(query,"h",45);
        std::lock_guard<std::mutex> lock(userSession->mutex);
        userSession->sim.runSteps(n);
        const std::string payload = "{\"state\":" + userSession->sim.stateJson() +
            ",\"visible\":" + userSession->sim.visibleJson(x0,y0,w,h) + "}";
        respond(200,"application/json; charset=utf-8",payload);
    } else if(method=="POST" && route=="/api/reset") {
        int sp=static_cast<int>(std::clamp<long long>(numJson(body,"spacing",10),1,1000000)); int d1=static_cast<int>(numJson(body,"dir1",0)), d2=static_cast<int>(numJson(body,"dir2",0));
        std::lock_guard<std::mutex> lock(userSession->mutex); userSession->sim.reset(sp,d1,d2); respond(200,"application/json; charset=utf-8",userSession->sim.stateJson());
    } else if(method=="POST" && route=="/api/spacing") {
        int sp=static_cast<int>(std::clamp<long long>(numJson(body,"spacing",10),1,1000000));
        std::lock_guard<std::mutex> lock(userSession->mutex); userSession->sim.repositionSpacing(sp); respond(200,"application/json; charset=utf-8",userSession->sim.stateJson());
    } else if(method=="POST" && route=="/api/direction") {
        int idx=static_cast<int>(numJson(body,"ant",0)), d=static_cast<int>(numJson(body,"dir",0));
        std::lock_guard<std::mutex> lock(userSession->mutex); userSession->sim.changeDirection(idx,d); respond(200,"application/json; charset=utf-8",userSession->sim.stateJson());
    } else if(method=="GET") {
        std::string path=mimeSafePath(route); if(path.empty()) respond(404,"text/plain; charset=utf-8","Not found"); else { auto file=readFile(path); if(file.empty()) respond(404,"text/plain; charset=utf-8","File not found. Run the server from its project folder."); else respond(200,contentType(path),file); }
    } else respond(405,"text/plain; charset=utf-8","Method not allowed");
    closeSocket(s);
}
}

int main(int argc, char** argv) {
    // Local development defaults to loopback. Hosted platforms can set PORT and HOST.
    int port = 8080;
    if (const char* envPort = std::getenv("PORT")) {
        try { port = std::clamp(std::stoi(envPort), 1, 65535); } catch(...) {}
    }
    if (argc > 1) { try { port=std::clamp(std::stoi(argv[1]),1,65535); } catch(...){} }
    std::string bindHost = "127.0.0.1";
    if (const char* envHost = std::getenv("HOST")) {
        if (*envHost) bindHost = envHost;
    }
#ifdef _WIN32
    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2), &wsa)!=0) { std::cerr << "Winsock の初期化に失敗しました。\n"; return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    socket_t server=socket(AF_INET,SOCK_STREAM,0);
#ifdef _WIN32
    if(server==INVALID_SOCKET) { std::cerr << "ソケットを作成できません。\n"; WSACleanup(); return 1; }
#else
    if(server<0) { std::cerr << "ソケットを作成できません。\n"; return 1; }
#endif
    int yes=1;
#ifdef _WIN32
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&yes),sizeof(yes));
#else
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
#endif
    sockaddr_in addr{}; addr.sin_family=AF_INET;
    if (bindHost == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bindHost.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "HOSTにはIPv4アドレスを指定してください（例: 127.0.0.1 または 0.0.0.0）。\n";
        closeSocket(server);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    addr.sin_port=htons(static_cast<uint16_t>(port));
    if(bind(server,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0 || listen(server,32)<0) {
        std::cerr << "ポート "<<port<<" を使用できません。別のポート番号を指定してください。\n"; closeSocket(server);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    std::cout << "Langton's Ant C++ サーバーを起動しました。\n待受アドレス: "<<bindHost<<":"<<port<<"\n";
    if (bindHost == "127.0.0.1") std::cout << "ブラウザーで http://127.0.0.1:"<<port<<" を開いてください。\n";
    std::cout << "終了するには Ctrl+C を押してください。\n" << std::flush;
    while(true) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int clientLen=sizeof(clientAddr);
#else
        socklen_t clientLen=sizeof(clientAddr);
#endif
        socket_t client=accept(server,reinterpret_cast<sockaddr*>(&clientAddr),&clientLen);
#ifdef _WIN32
        if(client==INVALID_SOCKET) continue;
#else
        if(client<0) continue;
#endif
        std::thread(handleClient,client).detach();
    }
    closeSocket(server);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
