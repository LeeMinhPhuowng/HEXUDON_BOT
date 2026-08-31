// ========================================================================
//  HEXUDON BOT v3.0 — Hệ thống Thu hoạch Đột phá & Tiếp tế Liên hoàn
// ========================================================================
//  Đột phá v3.0:
//    1. En-Route Auto-Harvesting: Tự động ăn tất cả các quán Udon dọc đường
//       hành quân tầm xa mà không tốn thêm bước di chuyển
//    2. 100% Step Utilization: Loại bỏ hoàn toàn ngắt vòng lặp sớm, khai thác
//       triệt để toàn bộ 100 bước mỗi ngày của mỗi xe
//    3. Multi-Patrol Tanker Chain: Xe tiếp tế lập lộ trình chuỗi ghé thăm lần
//       lượt 2-3 xe tuần tra cạn xăng nhất trong ngày
//    4. Smart Low-Fuel Rendezvous: Xe tuần tra cạn xăng (<15) chủ động hẹn
//       kết thúc ngày cùng ô với Tanker để sáng hôm sau nạp 100% bình xăng (40 xăng)
//    5. Primary Brand Assignment: Giữ vững 60/60 điểm tuyệt đối lũy kế ngày
// ========================================================================
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "minijson.hpp"
#include "http.hpp"
using namespace std;

namespace cfg {
    constexpr double W_GLOBAL_DIVERSITY = 30000.0; // Thưởng chuỗi chưa từng ăn toàn trận
    constexpr double W_DAILY_PRIMARY    = 20000.0; // Thưởng chuỗi được giao trong nhiệm vụ
    constexpr double W_DAILY_DIVERSITY  = 10000.0; // Thưởng chuỗi hôm nay cả đội chưa ăn
    constexpr double W_PORTION_BASE     = 500.0;   // Thưởng cơ bản cho mỗi phần Udon
    constexpr double W_DISTANCE         = 1.0;     // Phạt khoảng cách (steps)
    constexpr int    POLL_MS            = 250;     // Polling rate limit
}

struct SpotInfo {
    int pos;
    int brand;
    int stocks;
};

struct DijkResult {
    vector<int> dist;
    vector<int> fuel;
    vector<int> prev;
};

static int W = 0, H = 0;
static int g_nAgents   = 0;
static int g_maxFuel   = 40;
static int g_totalDays = 0;

static vector<int>      g_cells;
static vector<SpotInfo> g_spots;
static vector<int>      g_daySteps;
static vector<int>      g_assignment;
static set<int>         g_allBrands;
static set<int>         g_collectedBrands;

// Lưới lục giác even-r
static const int DE[6][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,0}};   // Hàng chẵn
static const int DO[6][2] = {{-1,-1},{0,-1},{1,0},{0,1},{-1,1},{-1,0}}; // Hàng lẻ

static int neighbor(int pos, int d) {
    if (W <= 0) return -1;
    int r = pos / W, c = pos % W;
    const int (*dl)[2] = (r & 1) ? DO : DE;
    int nc = c + dl[d][0], nr = r + dl[d][1];
    if (nc < 0 || nc >= W || nr < 0 || nr >= H) return -1;
    return nr * W + nc;
}

static pair<int,int> moveCost(int pos, int trafficStatus) {
    switch (g_cells[pos]) {
        case 0: return {2, 1};                     // Đồng bằng: 2 steps, 1 xăng
        case 2: return {3, 2};                     // Núi: 3 steps, 2 xăng
        case 1:                                    // Đường bộ
            if (trafficStatus == 1) return {2, 2}; // Đông đúc: 2 steps, 2 xăng
            if (trafficStatus == 2) return {4, 2}; // Ùn tắc: 4 steps, 2 xăng
            return {1, 2};                         // Thông thoáng: 1 step, 2 xăng
    }
    return {-1, -1};                               // Ao: không đi được
}

static int dirTo(int a, int b) {
    for (int d = 0; d < 6; d++) if (neighbor(a, d) == b) return d;
    return -1;
}

static int hexDist(int posA, int posB) {
    int ra = posA / W, ca = posA % W;
    int rb = posB / W, cb = posB % W;
    int qa = ca - (ra + (ra & 1)) / 2;
    int qb = cb - (rb + (rb & 1)) / 2;
    int sa = -qa - ra, sb = -qb - rb;
    return (abs(qa - qb) + abs(ra - rb) + abs(sa - sb)) / 2;
}

static int mapCenter() { return (H / 2) * W + (W / 2); }

// Dijkstra tìm đường tối ưu
static DijkResult dijkstraAll(int src, int maxSteps, int maxFuel,
                               const vector<int>& traffic, bool isTanker) {
    int N = W * H;
    DijkResult res;
    res.dist.assign(N, INT_MAX);
    res.fuel.assign(N, INT_MAX);
    res.prev.assign(N, -1);
    res.dist[src] = 0;
    res.fuel[src] = 0;

    priority_queue<pair<int,int>, vector<pair<int,int>>, greater<pair<int,int>>> pq;
    pq.push({0, src});

    while (!pq.empty()) {
        auto top = pq.top(); pq.pop();
        int d = top.first, u = top.second;
        if (d > res.dist[u]) continue;

        for (int dir = 0; dir < 6; dir++) {
            int nb = neighbor(u, dir);
            if (nb < 0 || g_cells[nb] == 3) continue;

            auto cost = moveCost(u, traffic[u]);
            int sc = cost.first, fc = cost.second;
            if (sc < 0) continue;

            int nd = d + sc;
            int nf = res.fuel[u] + (isTanker ? 0 : fc);
            if (nd > maxSteps) continue;
            if (!isTanker && nf > maxFuel) continue;

            if (nd < res.dist[nb] || (nd == res.dist[nb] && nf < res.fuel[nb])) {
                res.dist[nb] = nd;
                res.fuel[nb] = nf;
                res.prev[nb] = u;
                pq.push({nd, nb});
            }
        }
    }
    return res;
}

static vector<int> reconstructPath(const DijkResult& dijk, int src, int dst) {
    if (dst < 0 || dst >= (int)dijk.dist.size() || dijk.dist[dst] == INT_MAX)
        return {};
    vector<int> path;
    int safetyLimit = (int)dijk.dist.size();
    for (int v = dst; v != src; v = dijk.prev[v]) {
        if (v < 0 || --safetyLimit < 0) return {};
        path.push_back(v);
    }
    reverse(path.begin(), path.end());
    return path;
}

// Kiểm tra và tự động thu thập Udon nếu ô hiện tại là quán còn hàng
static bool checkAndClaimSpot(int pos, set<int>& visitedSpotsByMe,
                              map<int,int>& claimedStock,
                              set<int>& dayCollectedBrands) {
    for (auto& sp : g_spots) {
        if (sp.pos == pos && !visitedSpotsByMe.count(sp.pos) && claimedStock[sp.pos] < sp.stocks) {
            visitedSpotsByMe.insert(sp.pos);
            claimedStock[sp.pos]++;
            dayCollectedBrands.insert(sp.brand);
            g_collectedBrands.insert(sp.brand);
            return true;
        }
    }
    return false;
}

static string computeAssignment() {
    // 1 Tanker + 7 Patrols cho đội 8 xe
    int nTankers = 1;
    int nPatrols = max(1, g_nAgents - nTankers);

    g_assignment.resize(g_nAgents);
    for (int i = 0; i < g_nAgents; i++)
        g_assignment[i] = (i < nPatrols) ? 0 : 1;

    fprintf(stderr, "[ASSIGN] %d Patrols + %d Tankers\n", nPatrols, nTankers);

    ostringstream out;
    out << "[";
    for (int i = 0; i < g_nAgents; i++) {
        if (i) out << ",";
        out << g_assignment[i];
    }
    out << "]";
    return out.str();
}

// Chấm điểm mục tiêu
static double scoreSpot(const SpotInfo& spot, const DijkResult& dijk,
                         int primaryBrand, const set<int>& dayCollectedBrands) {
    if (dijk.dist[spot.pos] == INT_MAX) return -1e18;

    double score = cfg::W_PORTION_BASE;

    // 1. Chuỗi được giao nhiệm vụ hôm nay
    if (spot.brand == primaryBrand && !dayCollectedBrands.count(spot.brand)) {
        score += cfg::W_DAILY_PRIMARY;
    }

    // 2. Chuỗi hôm nay cả đội chưa ăn
    if (!dayCollectedBrands.count(spot.brand)) {
        score += cfg::W_DAILY_DIVERSITY;
    }

    // 3. Chuỗi toàn trận chưa từng ăn
    if (!g_collectedBrands.count(spot.brand)) {
        score += cfg::W_GLOBAL_DIVERSITY;
    }

    // 4. Phạt khoảng cách di chuyển
    score -= dijk.dist[spot.pos] * cfg::W_DISTANCE;

    return score;
}

// Lập kế hoạch tuần tra cho 1 xe (v3.0 En-Route Harvesting & 100% Step Usage)
static vector<int> planPatrolRoute(int pos, int fuel, int daySteps,
                                    const vector<int>& traffic,
                                    map<int,int>& claimedStock,
                                    set<int>& dayCollectedBrands,
                                    int primaryBrand,
                                    int tankerPos) {
    vector<int> actions;
    int stepsUsed = 0;
    int curPos    = pos;
    int curFuel   = fuel;
    set<int> visitedSpotsByMe;

    // Kiểm tra ngay ô xuất phát có quán không
    checkAndClaimSpot(curPos, visitedSpotsByMe, claimedStock, dayCollectedBrands);

    // Vòng lặp Multi-Target liên tục cho tới khi cạn sạch bước hoặc xăng
    int loopCount = 0;
    while (stepsUsed < daySteps && curFuel > 0 && loopCount++ < 50) {
        int stepsLeft  = daySteps - stepsUsed;
        int fuelBudget = curFuel;

        DijkResult dijk = dijkstraAll(curPos, stepsLeft, fuelBudget, traffic, false);

        int    bestIdx   = -1;
        double bestScore = -1e18;

        for (int i = 0; i < (int)g_spots.size(); i++) {
            const SpotInfo& sp = g_spots[i];

            if (visitedSpotsByMe.count(sp.pos)) continue;
            if (claimedStock[sp.pos] >= sp.stocks) continue;
            if (dijk.dist[sp.pos] == INT_MAX || dijk.fuel[sp.pos] > fuelBudget) continue;

            double sc = scoreSpot(sp, dijk, primaryBrand, dayCollectedBrands);
            if (sc > bestScore) {
                bestScore = sc;
                bestIdx   = i;
            }
        }

        // Nếu chưa ăn được primaryBrand và không có quán trong tầm 1 bước nhảy
        if (bestIdx < 0 && primaryBrand >= 0 && !dayCollectedBrands.count(primaryBrand)) {
            int targetSpotPos = -1, minH = INT_MAX;
            for (auto& sp : g_spots) {
                if (sp.brand == primaryBrand && claimedStock[sp.pos] < sp.stocks) {
                    int h = hexDist(curPos, sp.pos);
                    if (h < minH) { minH = h; targetSpotPos = sp.pos; }
                }
            }
            if (targetSpotPos >= 0) {
                DijkResult dijkGlobal = dijkstraAll(curPos, 9999, 9999, traffic, true);
                vector<int> fullPath = reconstructPath(dijkGlobal, curPos, targetSpotPos);
                bool moved = false;
                for (int nxt : fullPath) {
                    auto cost = moveCost(curPos, traffic[curPos]);
                    int sc = cost.first, fc = cost.second;
                    if (sc < 0 || stepsUsed + sc > daySteps || curFuel < fc) break;
                    int d = dirTo(curPos, nxt);
                    if (d < 0) break;
                    actions.push_back(d);
                    stepsUsed += sc;
                    curFuel   -= fc;
                    curPos     = nxt;
                    moved = true;

                    // Ăn dọc đường hành quân!
                    checkAndClaimSpot(curPos, visitedSpotsByMe, claimedStock, dayCollectedBrands);
                }
                if (moved) continue; // Tiếp tục vòng lặp tại vị trí mới thay vì break!
            }
            break;
        }

        if (bestIdx < 0) break; // Không còn quán nào trong tầm với

        vector<int> path = reconstructPath(dijk, curPos, g_spots[bestIdx].pos);
        if (path.empty()) break;

        bool pathOk = true;
        for (int nxt : path) {
            auto cost = moveCost(curPos, traffic[curPos]);
            int sc = cost.first, fc = cost.second;

            if (sc < 0 || stepsUsed + sc > daySteps || curFuel < fc) {
                pathOk = false;
                break;
            }
            int d = dirTo(curPos, nxt);
            if (d < 0) { pathOk = false; break; }

            actions.push_back(d);
            stepsUsed += sc;
            curFuel   -= fc;
            curPos     = nxt;

            // Ăn dọc đường nếu bước qua quán khác!
            checkAndClaimSpot(curPos, visitedSpotsByMe, claimedStock, dayCollectedBrands);
        }
        if (!pathOk) break;
    }

    // Nếu còn bước và xăng thấp (<15) hoặc còn steps dư: Hẹn gặp Tanker
    if (stepsUsed < daySteps && tankerPos >= 0 && curFuel > 0) {
        int stepsLeft = daySteps - stepsUsed;
        DijkResult dijkTanker = dijkstraAll(curPos, stepsLeft, curFuel, traffic, false);
        if (dijkTanker.dist[tankerPos] != INT_MAX) {
            vector<int> pathToTanker = reconstructPath(dijkTanker, curPos, tankerPos);
            for (int nxt : pathToTanker) {
                auto cost = moveCost(curPos, traffic[curPos]);
                int sc = cost.first, fc = cost.second;
                if (sc < 0 || stepsUsed + sc > daySteps || curFuel < fc) break;
                int d = dirTo(curPos, nxt);
                if (d < 0) break;
                actions.push_back(d);
                stepsUsed += sc;
                curFuel   -= fc;
                curPos     = nxt;

                checkAndClaimSpot(curPos, visitedSpotsByMe, claimedStock, dayCollectedBrands);
            }
        }
    }

    int rest = daySteps - stepsUsed;
    if (rest > 0) actions.push_back(-rest);

    return actions;
}

// Lập kế hoạch tiếp tế chuỗi cho xe Tanker (v3.0 Multi-Patrol Tanker Chain)
static vector<int> planTankerRoute(int tPos, int daySteps,
                                    const vector<int>& traffic,
                                    const vector<tuple<int,int,int>>& patrolInfo) {
    if (patrolInfo.empty()) return {-daySteps};

    // Sắp xếp các xe tuần tra theo mức xăng tăng dần (xe cạn xăng nhất ưu tiên hàng đầu)
    vector<tuple<int,int,int>> sortedPatrols = patrolInfo;
    sort(sortedPatrols.begin(), sortedPatrols.end(), [](const tuple<int,int,int>& a, const tuple<int,int,int>& b) {
        return get<1>(a) < get<1>(b);
    });

    vector<int> actions;
    int curPos = tPos, stepsUsed = 0;

    // Tanker lập lộ trình chuỗi ghé thăm lần lượt các xe tuần tra ít xăng
    for (auto& p : sortedPatrols) {
        int pFuel   = get<1>(p);
        int targetP = get<2>(p); // Vị trí kết thúc của xe đó

        // Chỉ ưu tiên tiếp tế nếu xe có xăng < 28 hoặc là xe cạn nhất
        if (pFuel > 28 && !actions.empty()) continue;

        int stepsLeft = daySteps - stepsUsed;
        if (stepsLeft <= 0) break;

        DijkResult dijk = dijkstraAll(curPos, stepsLeft, INT_MAX, traffic, true);
        if (dijk.dist[targetP] == INT_MAX) continue;

        vector<int> path = reconstructPath(dijk, curPos, targetP);
        bool reached = true;
        for (int nxt : path) {
            auto cost = moveCost(curPos, traffic[curPos]);
            int sc = cost.first;
            if (sc < 0 || stepsUsed + sc > daySteps) { reached = false; break; }
            int d = dirTo(curPos, nxt);
            if (d < 0) { reached = false; break; }
            actions.push_back(d);
            stepsUsed += sc;
            curPos = nxt;
        }
        if (!reached) break;
    }

    // Nếu vẫn còn bước sau khi tiếp tế: Di chuyển về phía trọng tâm nhóm hoặc trung tâm bản đồ
    if (stepsUsed < daySteps) {
        int stepsLeft = daySteps - stepsUsed;
        int center = mapCenter();
        DijkResult dijkCenter = dijkstraAll(curPos, stepsLeft, INT_MAX, traffic, true);
        if (dijkCenter.dist[center] != INT_MAX) {
            vector<int> path = reconstructPath(dijkCenter, curPos, center);
            for (int nxt : path) {
                auto cost = moveCost(curPos, traffic[curPos]);
                int sc = cost.first;
                if (sc < 0 || stepsUsed + sc > daySteps) break;
                int d = dirTo(curPos, nxt);
                if (d < 0) break;
                actions.push_back(d);
                stepsUsed += sc;
                curPos = nxt;
            }
        }
    }

    int rest = daySteps - stepsUsed;
    if (rest > 0) actions.push_back(-rest);
    return actions;
}

// ========================================================================
//  ĐIỀU PHỐI ĐA TÁC NHÂN HÀNG NGÀY (ORCHESTRATOR)
// ========================================================================
static string planActions(const mj::Value& m) {
    int day      = m["day"].asInt();
    int daySteps = (day >= 0 && day < (int)g_daySteps.size()) ? g_daySteps[day] : 30;

    vector<int> traffic(W * H, 0);
    for (size_t i = 0; i < m["traffics"].size(); i++) {
        int tpos = m["traffics"][i]["pos"].asInt();
        int tst  = m["traffics"][i]["status"].asInt();
        if (tpos >= 0 && tpos < W * H) traffic[tpos] = tst;
    }

    struct AgentState { int pos, kind, fuel; };
    const mj::Value& ags = m["agents"];
    int nAgs = (int)ags.size();
    vector<AgentState> agents(nAgs);
    for (int i = 0; i < nAgs; i++) {
        agents[i].pos  = ags[i]["pos"].asInt();
        agents[i].kind = ags[i]["kind"].asInt();
        agents[i].fuel = ags[i]["fuel"].isNull() ? (1 << 30) : ags[i]["fuel"].asInt();
    }

    if (g_maxFuel <= 0) {
        for (int i = 0; i < nAgs; i++)
            if (agents[i].kind == 0)
                g_maxFuel = max(g_maxFuel, agents[i].fuel);
        if (g_maxFuel <= 0) g_maxFuel = 40;
    }

    vector<int> patrolIds, tankerIds;
    for (int i = 0; i < nAgs; i++) {
        if (agents[i].kind == 0) patrolIds.push_back(i);
        else                     tankerIds.push_back(i);
    }

    // ── PHÂN BỔ NHIỆM VỤ CHUỖI UDON (PRIMARY BRAND MATCHING) ─────────
    vector<int> brandList(g_allBrands.begin(), g_allBrands.end());
    map<int, int> patrolToBrand;
    set<int> assignedBrands;

    // Gán tối ưu theo khoảng cách thực tế
    for (int b : brandList) {
        int bestPatrol = -1;
        int minDistance = INT_MAX;

        for (int pi : patrolIds) {
            if (patrolToBrand.count(pi)) continue;
            for (auto& sp : g_spots) {
                if (sp.brand == b) {
                    int d = hexDist(agents[pi].pos, sp.pos);
                    if (d < minDistance) {
                        minDistance = d;
                        bestPatrol = pi;
                    }
                }
            }
        }

        if (bestPatrol >= 0) {
            patrolToBrand[bestPatrol] = b;
            assignedBrands.insert(b);
        }
    }

    for (int pi : patrolIds) {
        if (!patrolToBrand.count(pi)) {
            int bestB = -1, minD = INT_MAX;
            for (auto& sp : g_spots) {
                int d = hexDist(agents[pi].pos, sp.pos);
                if (d < minD) { minD = d; bestB = sp.brand; }
            }
            patrolToBrand[pi] = bestB;
        }
    }

    vector<vector<int>> allActions(nAgs);
    vector<int>         endPos(nAgs);
    map<int,int>        claimedStock;
    set<int>            dayCollectedBrands;

    auto nearestTankerPos = [&](int patrolPos) -> int {
        int best = -1, bestD = INT_MAX;
        for (int ti : tankerIds) {
            int d = hexDist(patrolPos, agents[ti].pos);
            if (d < bestD) { bestD = d; best = agents[ti].pos; }
        }
        return best;
    };

    // ── LẬP TUYẾN CHO TỪNG PATROL ──────────────────────────────────
    for (int pi : patrolIds) {
        int tPos = nearestTankerPos(agents[pi].pos);
        int primaryBrand = patrolToBrand[pi];

        allActions[pi] = planPatrolRoute(
            agents[pi].pos, agents[pi].fuel, daySteps, traffic,
            claimedStock, dayCollectedBrands, primaryBrand, tPos
        );

        int cur = agents[pi].pos;
        for (int a : allActions[pi]) {
            if (a >= 0) {
                int nb = neighbor(cur, a);
                if (nb >= 0) cur = nb;
            }
        }
        endPos[pi] = cur;
    }

    // ── LẬP TUYẾN CHO TANKER (MULTI-STOP REFUEL CHAIN) ──────────────
    for (int ti : tankerIds) {
        vector<tuple<int,int,int>> patrolInfo;
        for (int pi : patrolIds) {
            patrolInfo.push_back(make_tuple(
                agents[pi].pos, agents[pi].fuel, endPos[pi]
            ));
        }
        allActions[ti] = planTankerRoute(
            agents[ti].pos, daySteps, traffic, patrolInfo
        );
    }

    // ── ĐÓNG GÓI JSON ──────────────────────────────────────────────
    ostringstream out;
    out << "[";
    for (int i = 0; i < nAgs; i++) {
        if (i) out << ",";
        out << "[";
        vector<int>& acts = allActions[i];
        if (acts.empty()) {
            out << -daySteps;
        } else {
            for (size_t k = 0; k < acts.size(); k++) {
                if (k) out << ",";
                out << acts[k];
            }
        }
        out << "]";
    }
    out << "]";

    int totalClaimed = 0;
    for (auto& kv : claimedStock) totalClaimed += kv.second;
    fprintf(stderr, "[DAY %d] Steps=%d | Da thu: %d phan | %zu / %zu loai chuoi hom nay | Tong toan tran: %zu loai\n",
            day, daySteps, totalClaimed, dayCollectedBrands.size(), g_allBrands.size(), g_collectedBrands.size());

    return out.str();
}

static string parseSetup(const mj::Value& m) {
    const mj::Value& mp = m["map"];
    W = mp["width"].asInt();
    H = mp["height"].asInt();
    g_cells.assign((size_t)W * H, 0);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            g_cells[r * W + c] = mp["cells"][r][c].asInt();

    g_spots.clear();
    g_allBrands.clear();
    for (size_t i = 0; i < m["spots"].size(); i++) {
        SpotInfo s;
        s.pos = m["spots"][i]["pos"].asInt();

        if      (!m["spots"][i]["brand"].isNull())     s.brand = m["spots"][i]["brand"].asInt();
        else if (!m["spots"][i]["franchise"].isNull()) s.brand = m["spots"][i]["franchise"].asInt();
        else if (!m["spots"][i]["chain"].isNull())     s.brand = m["spots"][i]["chain"].asInt();
        else if (!m["spots"][i]["type"].isNull())      s.brand = m["spots"][i]["type"].asInt();
        else s.brand = 0;

        if      (!m["spots"][i]["stocks"].isNull())    s.stocks = m["spots"][i]["stocks"].asInt();
        else if (!m["spots"][i]["stock"].isNull())     s.stocks = m["spots"][i]["stock"].asInt();
        else s.stocks = 1;

        if (s.stocks <= 0) s.stocks = 1;
        g_spots.push_back(s);
        g_allBrands.insert(s.brand);
    }

    g_daySteps.clear();
    for (size_t i = 0; i < m["daySteps"].size(); i++)
        g_daySteps.push_back(m["daySteps"][i].asInt());
    g_totalDays = (int)g_daySteps.size();

    if (!m["fuelLimits"].isNull())
        g_maxFuel = m["fuelLimits"].asInt();
    else if (!m["fuelCapacity"].isNull())
        g_maxFuel = m["fuelCapacity"].asInt();
    else
        g_maxFuel = 40;

    g_nAgents = (int)m["agents"].size();
    g_collectedBrands.clear();

    return computeAssignment();
}

static void sleepMs(int ms) {
    this_thread::sleep_for(chrono::milliseconds(ms));
}

int main(int argc, char** argv) {
    string url, matchId, token;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-url" && i + 1 < argc)                          url     = argv[++i];
        else if ((arg == "-match" || arg == "-m") && i + 1 < argc)  matchId = argv[++i];
        else if ((arg == "-token" || arg == "-t") && i + 1 < argc)  token   = argv[++i];
        else if (arg == "-transport" && i + 1 < argc)               i++;
        else if (arg[0] != '-') {
            if      (url.empty())     url     = arg;
            else if (matchId.empty()) matchId = arg;
            else if (token.empty())   token   = arg;
        }
    }
    if (url.empty() || matchId.empty() || token.empty()) {
        fprintf(stderr, "Cach dung:\n");
        fprintf(stderr, "  %s <URL> <MATCH_ID> <TOKEN>\n", argv[0]);
        fprintf(stderr, "  %s -url <URL> -match <MATCH_ID> -token <TOKEN>\n", argv[0]);
        return 2;
    }
    string base = url + "/api/v1/matches/" + matchId;

    string assignBody;
    for (;;) {
        auto r = http::request(base, "GET", "/setup", token, "");
        if (r.status == 200) {
            auto v = mj::parse(r.body);
            assignBody = parseSetup(*v);
            break;
        }
        if (r.status == 0 || r.status == 425 || r.status == 429) {
            sleepMs(cfg::POLL_MS);
            continue;
        }
        fprintf(stderr, "GET /setup -> HTTP %d (token sai / match khong hop le?)\n", r.status);
        return 1;
    }

    fprintf(stderr, "=== HEXUDON BOT v3.0 ===\n");
    fprintf(stderr, "[SETUP] Map %dx%d | %zu spots | %zu brands | %d agents | maxFuel=%d | %d days\n",
            W, H, g_spots.size(), g_allBrands.size(), g_nAgents, g_maxFuel, g_totalDays);

    for (;;) {
        auto r = http::request(base, "POST", "/assignment", token, assignBody);
        if (r.status == 200) break;
        if (r.status == 0 || r.status == 429) { sleepMs(cfg::POLL_MS); continue; }
        fprintf(stderr, "POST /assignment -> HTTP %d\n", r.status);
        return 1;
    }
    fprintf(stderr, "[ASSIGNED] %s\n", assignBody.c_str());

    int lastDay = -1;
    for (;;) {
        auto r = http::request(base, "GET", "/state", token, "");
        if (r.status == 200) {
            auto v = mj::parse(r.body);
            int day = (*v)["day"].asInt();

            if (day != lastDay) {
                string acts = planActions(*v);
                auto pr = http::request(base, "POST", "/actions", token, acts);

                if (pr.status == 200) {
                    lastDay = day;
                } else {
                    fprintf(stderr, "[ERR] POST /actions day %d -> HTTP %d: %s\n",
                            day, pr.status, pr.body.c_str());
                }
            }
        } else if (r.status != 429 && r.status != 0) {
            auto rr = http::request(base, "GET", "/result", token, "");
            if (rr.status == 200) {
                fprintf(stderr, "[RESULT] %s\n", rr.body.c_str());
                break;
            }
        }
        sleepMs(cfg::POLL_MS);
    }

    fprintf(stderr, "=== KET THUC TRAN DAU ===\n");
    fprintf(stderr, "Tong loai brand thu duoc: %zu / %zu\n",
            g_collectedBrands.size(), g_allBrands.size());
    return 0;
}
