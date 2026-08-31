// ========================================================================
//  HEXUDON BOT v2.0 — Hệ thống Phân công Độc quyền & Đa nhiệm Toàn diện
// ========================================================================
//  Đột phá v2.0:
//    1. Primary Brand Assignment: Mỗi ngày gán độc quyền từng Chuỗi Udon
//       cho các xe tuần tra gần nhất -> ĐẢM BẢO 6/6 LOẠI MỖI NGÀY (60/60)
//    2. Opportunistic Multi-Target: Sau khi ăn chuỗi được giao, lập tức
//       quét sạch các quán Udon lân cận -> ĐẠT 300+ PHẦN UDON
//    3. Long-Range Target Navigation: Xe chủ động hướng tới cụm mục tiêu
//       xa thay vì bị kẹt trong vùng cục bộ
//    4. Tanker Intercept & Refuel: Xe tiếp tế chủ động đón đầu xe cạn xăng
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
    constexpr double W_GLOBAL_DIVERSITY = 20000.0; // Thưởng chuỗi chưa từng ăn toàn trận
    constexpr double W_DAILY_PRIMARY    = 15000.0; // Thưởng chuỗi được giao trong nhiệm vụ
    constexpr double W_DAILY_DIVERSITY  = 8000.0;  // Thưởng chuỗi hôm nay cả đội chưa ăn
    constexpr double W_PORTION_BASE     = 300.0;   // Thưởng cơ bản cho mỗi phần Udon
    constexpr double W_DISTANCE         = 1.0;     // Phạt khoảng cách (steps)
    constexpr double W_TANKER_PROX      = 0.5;     // Kéo về xe tiếp tế cuối ngày
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

static string computeAssignment() {
    // 1 Tanker + (N-1) Patrols để tối đa số xe ghi điểm
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
                         int primaryBrand, const set<int>& dayCollectedBrands, int tankerPos) {
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

// Lập kế hoạch tuần tra cho 1 xe
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

    // Vòng lặp Multi-Target liên tục
    while (stepsUsed < daySteps && curFuel > 0) {
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

            double sc = scoreSpot(sp, dijk, primaryBrand, dayCollectedBrands, tankerPos);
            if (sc > bestScore) {
                bestScore = sc;
                bestIdx   = i;
            }
        }

        // Nếu không có quán nào trong tầm với trực tiếp, nhưng vẫn chưa ăn primaryBrand
        if (bestIdx < 0 && primaryBrand >= 0 && !dayCollectedBrands.count(primaryBrand)) {
            // Tìm quán thuộc primaryBrand gần nhất theo khoảng cách hex và di chuyển về phía đó
            int targetSpotPos = -1, minH = INT_MAX;
            for (auto& sp : g_spots) {
                if (sp.brand == primaryBrand && claimedStock[sp.pos] < sp.stocks) {
                    int h = hexDist(curPos, sp.pos);
                    if (h < minH) { minH = h; targetSpotPos = sp.pos; }
                }
            }
            if (targetSpotPos >= 0) {
                // Dijkstra không giới hạn steps/fuel để lấy hướng đi tốt nhất
                DijkResult dijkGlobal = dijkstraAll(curPos, 9999, 9999, traffic, true);
                vector<int> fullPath = reconstructPath(dijkGlobal, curPos, targetSpotPos);
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
                }
            }
            break;
        }

        if (bestIdx < 0) break; // Không còn quán nào nữa

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
        }
        if (!pathOk) break;

        visitedSpotsByMe.insert(g_spots[bestIdx].pos);
        claimedStock[g_spots[bestIdx].pos]++;
        dayCollectedBrands.insert(g_spots[bestIdx].brand);
        g_collectedBrands.insert(g_spots[bestIdx].brand);
    }

    // Nếu còn bước và gần hết xăng: Di chuyển về phía xe tiếp tế
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
            }
        }
    }

    int rest = daySteps - stepsUsed;
    if (rest > 0) actions.push_back(-rest);

    return actions;
}

// Lập kế hoạch cho xe tiếp tế (Tanker)
static vector<int> planTankerRoute(int tPos, int daySteps,
                                    const vector<int>& traffic,
                                    const vector<tuple<int,int,int>>& patrolInfo) {
    if (patrolInfo.empty()) return {-daySteps};

    // Tìm xe tuần tra có lượng xăng thấp nhất
    int targetPos = -1;
    int minFuel   = INT_MAX;
    for (size_t k = 0; k < patrolInfo.size(); k++) {
        int pFuel = get<1>(patrolInfo[k]);
        if (pFuel < minFuel) {
            minFuel   = pFuel;
            targetPos = get<2>(patrolInfo[k]);
        }
    }

    // Nếu mọi xe đều dư xăng -> Đi về trọng tâm của nhóm
    if (g_maxFuel > 0 && minFuel > g_maxFuel / 2) {
        double sumR = 0, sumC = 0;
        int n = (int)patrolInfo.size();
        for (int k = 0; k < n; k++) {
            int endP = get<2>(patrolInfo[k]);
            sumR += endP / W;
            sumC += endP % W;
        }
        int cr = max(0, min(H - 1, (int)(sumR / n)));
        int cc = max(0, min(W - 1, (int)(sumC / n)));
        int centroid = cr * W + cc;
        if (centroid >= 0 && centroid < W * H && g_cells[centroid] != 3)
            targetPos = centroid;
    }

    if (targetPos < 0) return {-daySteps};

    DijkResult dijk = dijkstraAll(tPos, daySteps, INT_MAX, traffic, true);
    vector<int> path = reconstructPath(dijk, tPos, targetPos);

    vector<int> actions;
    int curPos = tPos, stepsUsed = 0;

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
    // Tìm danh sách tất cả các brand có mặt trong trận
    vector<int> brandList(g_allBrands.begin(), g_allBrands.end());
    map<int, int> patrolToBrand; // patrolId -> primaryBrand (-1 nếu không gán)
    set<int> assignedBrands;

    // Gán tham lam: Mỗi brand gán cho patrol có khoảng cách gần nhất
    for (int b : brandList) {
        int bestPatrol = -1;
        int minDistance = INT_MAX;

        // Tìm điểm gần nhất của brand b
        for (int pi : patrolIds) {
            if (patrolToBrand.count(pi)) continue; // Patrol này đã được gán brand khác
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

    // Các Patrol còn lại (nếu có): gán brand gần nhất
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

    // ── LẬP TUYẾN CHO TANKER ───────────────────────────────────────
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

    fprintf(stderr, "=== HEXUDON BOT v2.0 ===\n");
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
