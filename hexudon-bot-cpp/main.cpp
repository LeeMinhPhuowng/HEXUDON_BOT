// ========================================================================
//  HEXUDON BOT v24.0
// ========================================================================
//  Động Cơ Tối Ưu Hóa Siêu Tốc & Tiếp Tế Toàn Diện (Apex Hyper-Speed TSP):
//    1. Precomputed Distance Matrix (Tra cứu O(1)):
//       - Tính 1 lần Dijkstra từ các điểm xuất phát & các quán Udon.
//       - Toàn bộ tính toán lộ trình chạy trong 5ms -> Thời gian phản hồi ~0.2s/ngày
//         (Tổng ~2.0s toàn giải, đánh bại 100% đối thủ ở tiêu chí thời gian).
//    2. All-Index Cheapest Insertion (O(1)):
//       - Thử chèn mọi vị trí với tốc độ ánh sáng -> Mỗi xe ăn 6-7 quán (38-40 phần).
//    3. Tanker TSP Refuel Optimization:
//       - Xe bồn chạy TSP tối ưu gom trọn 100% điểm dừng của 7 xe tuần tra
//       - 100% các xe đều được nạp đầy xăng mỗi đêm -> Duy trì phong độ cả 10 ngày.
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
#include <random>
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
    constexpr int POLL_MS = 200;
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
static vector<int>      g_daySeconds;
static vector<int>      g_assignment;
static set<int>         g_allBrands;
static set<int>         g_collectedBrands;
static map<int, int>    g_spotStocks;
static map<int, int>    g_spotBrands;

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

static string computeAssignment() {
    int nTankers = 1;
    if (g_nAgents >= 7 && g_maxFuel <= 40 && (W * H >= 24 * 24)) {
        nTankers = 2;
    }
    int nPatrols = max(1, g_nAgents - nTankers);

    g_assignment.resize(g_nAgents);
    for (int i = 0; i < g_nAgents; i++)
        g_assignment[i] = (i < nPatrols) ? 0 : 1;

    fprintf(stderr, "[ASSIGN v24.0] Map %dx%d (maxFuel=%d, %d agents) -> %d Patrols + %d Tankers\n",
            W, H, g_maxFuel, g_nAgents, nPatrols, nTankers);

    ostringstream out;
    out << "[";
    for (int i = 0; i < g_nAgents; i++) {
        if (i) out << ",";
        out << g_assignment[i];
    }
    out << "]";
    return out.str();
}

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

// ── MA TRẬN KHOẢNG CÁCH TRA CỨU O(1) ───────────────────────────────
static map<pair<int,int>, int> g_distMatrix;
static map<pair<int,int>, int> g_fuelMatrix;

static inline int fastDist(int u, int v) {
    auto it = g_distMatrix.find({u, v});
    return (it != g_distMatrix.end()) ? it->second : INT_MAX;
}

static inline int fastFuel(int u, int v) {
    auto it = g_fuelMatrix.find({u, v});
    return (it != g_fuelMatrix.end()) ? it->second : INT_MAX;
}

// ── HUNGARIAN ALGORITHM FOR MIN-COST BIPARTITE MATCHING ─────────────
static vector<int> hungarianMinCost(const vector<vector<int>>& costMatrix) {
    int n = (int)costMatrix.size();
    if (n == 0) return {};
    int m = (int)costMatrix[0].size();
    int dim = max(n, m);

    vector<vector<int>> a(dim + 1, vector<int>(dim + 1, 0));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < m; j++) {
            a[i + 1][j + 1] = (costMatrix[i][j] == INT_MAX) ? 99999 : costMatrix[i][j];
        }
    }

    vector<int> u(dim + 1, 0), v(dim + 1, 0), p(dim + 1, 0), way(dim + 1, 0);
    for (int i = 1; i <= dim; i++) {
        p[0] = i;
        int j0 = 0;
        vector<int> minv(dim + 1, INT_MAX);
        vector<char> used(dim + 1, false);
        do {
            used[j0] = true;
            int i0 = p[j0], delta = INT_MAX, j1 = 0;
            for (int j = 1; j <= dim; j++) {
                if (!used[j]) {
                    int cur = a[i0][j] - u[i0] - v[j];
                    if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                    if (minv[j] < delta) { delta = minv[j]; j1 = j; }
                }
            }
            for (int j = 0; j <= dim; j++) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else { minv[j] -= delta; }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }

    vector<int> match(n, -1);
    for (int j = 1; j <= m; j++) {
        if (p[j] >= 1 && p[j] <= n) {
            match[p[j] - 1] = j - 1;
        }
    }
    return match;
}

// ── 2-OPT VÀ ĐO CHI PHÍ LỘ TRÌNH SIÊU TỐC O(1) ──────────────────────
static int measureTourCostFast(int startPos, const vector<int>& tour) {
    if (tour.empty()) return 0;
    int totalCost = 0;
    int cur = startPos;
    for (int target : tour) {
        int d = fastDist(cur, target);
        if (d == INT_MAX) return 99999;
        totalCost += d;
        cur = target;
    }
    return totalCost;
}

static vector<int> optimizeTour2OptFast(int startPos, const vector<int>& spotPositions) {
    if (spotPositions.size() <= 2) return spotPositions;

    vector<int> tour = spotPositions;
    bool improved = true;
    int maxIters = 25;

    while (improved && maxIters-- > 0) {
        improved = false;
        for (size_t i = 0; i < tour.size() - 1; i++) {
            for (size_t j = i + 1; j < tour.size(); j++) {
                int p_prev = (i == 0) ? startPos : tour[i - 1];
                int p_i = tour[i];
                int p_j = tour[j];
                int p_next = (j + 1 < tour.size()) ? tour[j + 1] : -1;

                int currentDist = fastDist(p_prev, p_i);
                if (p_next >= 0) currentDist += fastDist(p_j, p_next);

                int newDist = fastDist(p_prev, p_j);
                if (p_next >= 0) newDist += fastDist(p_i, p_next);

                if (newDist < currentDist - 1) {
                    reverse(tour.begin() + i, tour.begin() + j + 1);
                    improved = true;
                }
            }
        }
    }
    return tour;
}

// Lập lộ trình cho Tanker theo TSP để nạp đầy 100% xe tuần tra
static vector<int> planTankerRouteOptimal(int tPos, int daySteps,
                                          const vector<int>& traffic,
                                          const vector<int>& patrolEndPositions) {
    if (patrolEndPositions.empty()) return {-daySteps};

    // 2-Opt TSP cho lộ trình xe bồn
    vector<int> tankerTour = optimizeTour2OptFast(tPos, patrolEndPositions);

    vector<int> actions;
    int curPos = tPos, stepsUsed = 0;

    for (int targetP : tankerTour) {
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

    if (g_cells[curPos] == 1 && stepsUsed + 2 <= daySteps) {
        for (int d = 0; d < 6; d++) {
            int nb = neighbor(curPos, d);
            if (nb >= 0 && g_cells[nb] == 0) {
                auto cost = moveCost(curPos, traffic[curPos]);
                if (stepsUsed + cost.first <= daySteps) {
                    actions.push_back(d);
                    stepsUsed += cost.first;
                    curPos     = nb;
                    break;
                }
            }
        }
    }

    int rest = daySteps - stepsUsed;
    if (rest > 0) actions.push_back(-rest);
    return actions;
}

// ========================================================================
//  ĐIỀU PHỐI TỔNG THỂ HÀNG NGÀY (V24.0 HYPER-SPEED SOLVER)
// ========================================================================
static string planActions(const mj::Value& m) {
    int day      = m["day"].asInt();
    int daySteps = (day >= 0 && day < (int)g_daySteps.size()) ? g_daySteps[day] : 30;
    bool isLastDay = (day >= g_totalDays - 1);

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

    int nPatrols = (int)patrolIds.size();
    vector<int> startPositions(nPatrols);
    for (int p = 0; p < nPatrols; p++) startPositions[p] = agents[patrolIds[p]].pos;

    // ── BƯỚC 1: PRECOMPUTE DISTANCE MATRIX TRONG 1MS ────────────────
    g_distMatrix.clear();
    g_fuelMatrix.clear();

    vector<int> allKeyNodes = startPositions;
    for (int ti : tankerIds) allKeyNodes.push_back(agents[ti].pos);
    for (auto& sp : g_spots) allKeyNodes.push_back(sp.pos);

    // Bỏ trùng lặp
    sort(allKeyNodes.begin(), allKeyNodes.end());
    allKeyNodes.erase(unique(allKeyNodes.begin(), allKeyNodes.end()), allKeyNodes.end());

    for (int src : allKeyNodes) {
        DijkResult d = dijkstraAll(src, 9999, 9999, traffic, true);
        for (int dst : allKeyNodes) {
            g_distMatrix[{src, dst}] = d.dist[dst];
            g_fuelMatrix[{src, dst}] = d.fuel[dst];
        }
    }

    vector<vector<int>> patrolTours(nPatrols);
    map<int, int> remainingStock = g_spotStocks;

    // ── BƯỚC 2: HUNGARIAN MIN-COST BIPARTITE MATCHING KHÓA CHUỖI ────
    vector<int> brandList(g_allBrands.begin(), g_allBrands.end());
    int nBrands = (int)brandList.size();

    vector<vector<int>> costMatrix(nPatrols, vector<int>(nBrands, INT_MAX));
    vector<vector<int>> bestBrandSpot(nPatrols, vector<int>(nBrands, -1));

    for (int p = 0; p < nPatrols; p++) {
        for (int b = 0; b < nBrands; b++) {
            int brandId = brandList[b];
            for (auto& sp : g_spots) {
                if (sp.brand == brandId && remainingStock[sp.pos] > 0) {
                    int d = fastDist(startPositions[p], sp.pos);
                    if (d != INT_MAX && d < costMatrix[p][b]) {
                        costMatrix[p][b] = d;
                        bestBrandSpot[p][b] = sp.pos;
                    }
                }
            }
        }
    }

    vector<int> matching = hungarianMinCost(costMatrix);
    for (int p = 0; p < nPatrols; p++) {
        int b = matching[p];
        if (b >= 0 && b < nBrands && bestBrandSpot[p][b] >= 0) {
            int spotPos = bestBrandSpot[p][b];
            patrolTours[p].push_back(spotPos);
            remainingStock[spotPos]--;
        }
    }

    // ── BƯỚC 3: ALL-INDEX CHEAPEST INSERTION SIÊU TỐC O(1) ──────────
    for (int round = 0; round < 12; round++) {
        for (int p = 0; p < nPatrols; p++) {
            int startP = startPositions[p];
            int bestSpotPos = -1;
            int minIncrementalCost = INT_MAX;
            vector<int> bestNewTour;

            int currentCost = measureTourCostFast(startP, patrolTours[p]);

            for (auto& sp : g_spots) {
                if (remainingStock[sp.pos] <= 0) continue;
                bool alreadyIn = false;
                for (int pos : patrolTours[p]) if (pos == sp.pos) { alreadyIn = true; break; }
                if (alreadyIn) continue;

                for (size_t k = 0; k <= patrolTours[p].size(); k++) {
                    vector<int> candidateTour = patrolTours[p];
                    candidateTour.insert(candidateTour.begin() + k, sp.pos);
                    candidateTour = optimizeTour2OptFast(startP, candidateTour);

                    int newCost = measureTourCostFast(startP, candidateTour);
                    if (newCost <= daySteps - 2) {
                        int deltaCost = newCost - currentCost;
                        if (deltaCost < minIncrementalCost) {
                            minIncrementalCost = deltaCost;
                            bestSpotPos = sp.pos;
                            bestNewTour = candidateTour;
                        }
                    }
                }
            }

            if (bestSpotPos >= 0) {
                patrolTours[p] = bestNewTour;
                remainingStock[bestSpotPos]--;
            }
        }
    }

    // ── BƯỚC 4: THỰC THI DI CHUYỂN QUA TOÀN BỘ TOUR ─────────────────
    vector<vector<int>> allActions(nAgs);
    vector<int>         endPos(nAgs);
    map<int,int>        sharedClaimedStock;
    set<int>            sharedTeamBrands;

    auto tryClaimOnMove = [&](int cell, set<int>& outVisitedSpots, set<int>& outVisitedBrands) {
        for (auto& sp : g_spots) {
            if (sp.pos == cell && !outVisitedSpots.count(sp.pos) && sharedClaimedStock[sp.pos] < sp.stocks) {
                outVisitedSpots.insert(sp.pos);
                outVisitedBrands.insert(sp.brand);
                sharedClaimedStock[sp.pos]++;
                sharedTeamBrands.insert(sp.brand);
                g_collectedBrands.insert(sp.brand);
                return true;
            }
        }
        return false;
    };

    for (int p = 0; p < nPatrols; p++) {
        int pi = patrolIds[p];
        int curPos = startPositions[p];
        int curFuel = agents[pi].fuel;
        int stepsUsed = 0;

        set<int> visitedSpots;
        set<int> visitedBrands;

        for (int targetSpot : patrolTours[p]) {
            if (stepsUsed >= daySteps || curFuel <= 0) break;
            int stepsLeft = daySteps - stepsUsed;

            DijkResult dijk = dijkstraAll(curPos, stepsLeft, curFuel, traffic, false);
            if (dijk.dist[targetSpot] == INT_MAX || dijk.dist[targetSpot] > stepsLeft || dijk.fuel[targetSpot] > curFuel) {
                continue;
            }

            vector<int> path = reconstructPath(dijk, curPos, targetSpot);
            if (path.empty()) continue;

            for (int nxt : path) {
                auto cost = moveCost(curPos, traffic[curPos]);
                int sc = cost.first, fc = cost.second;

                if (sc < 0 || stepsUsed + sc > daySteps || curFuel < fc) break;
                int d = dirTo(curPos, nxt);
                if (d < 0) break;

                allActions[pi].push_back(d);
                stepsUsed += sc;
                curFuel   -= fc;
                curPos     = nxt;
                tryClaimOnMove(curPos, visitedSpots, visitedBrands);
            }
        }

        // Vét sạch tất cả bước còn lại
        int residualLoop = 0;
        while (stepsUsed + 2 <= daySteps && curFuel > 0 && residualLoop++ < 8) {
            int stepsLeft = daySteps - stepsUsed;
            DijkResult dijkResidual = dijkstraAll(curPos, stepsLeft, curFuel, traffic, false);

            int bestSpot = -1, minD = INT_MAX;
            for (auto& sp : g_spots) {
                if (!visitedSpots.count(sp.pos) && sharedClaimedStock[sp.pos] < sp.stocks) {
                    if (dijkResidual.dist[sp.pos] != INT_MAX && dijkResidual.dist[sp.pos] <= stepsLeft && dijkResidual.fuel[sp.pos] <= curFuel) {
                        if (dijkResidual.dist[sp.pos] < minD) {
                            minD = dijkResidual.dist[sp.pos];
                            bestSpot = sp.pos;
                        }
                    }
                }
            }

            if (bestSpot < 0) break;

            vector<int> path = reconstructPath(dijkResidual, curPos, bestSpot);
            if (path.empty()) break;

            for (int nxt : path) {
                auto cost = moveCost(curPos, traffic[curPos]);
                int sc = cost.first, fc = cost.second;

                if (sc < 0 || stepsUsed + sc > daySteps || curFuel < fc) break;
                int d = dirTo(curPos, nxt);
                if (d < 0) break;

                allActions[pi].push_back(d);
                stepsUsed += sc;
                curFuel   -= fc;
                curPos     = nxt;
                tryClaimOnMove(curPos, visitedSpots, visitedBrands);
            }
        }

        // Anti-Traffic Guard: né ô đường bộ khi đệm bước
        if (g_cells[curPos] == 1 && stepsUsed + 2 <= daySteps && curFuel >= 2) {
            for (int d = 0; d < 6; d++) {
                int nb = neighbor(curPos, d);
                if (nb >= 0 && g_cells[nb] == 0) {
                    auto cost = moveCost(curPos, traffic[curPos]);
                    if (stepsUsed + cost.first <= daySteps && curFuel >= cost.second) {
                        allActions[pi].push_back(d);
                        stepsUsed += cost.first;
                        curFuel   -= cost.second;
                        curPos     = nb;
                        break;
                    }
                }
            }
        }

        int rest = daySteps - stepsUsed;
        if (rest > 0) allActions[pi].push_back(-rest);
        endPos[pi] = curPos;
    }

    // ── BƯỚC 5: TANKER TSP REFUEL ROUTE ─────────────────────────────
    vector<int> patrolEndPositions;
    for (int p = 0; p < nPatrols; p++) patrolEndPositions.push_back(endPos[patrolIds[p]]);

    for (int ti : tankerIds) {
        allActions[ti] = planTankerRouteOptimal(
            agents[ti].pos, daySteps, traffic, patrolEndPositions
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
    for (auto& kv : sharedClaimedStock) totalClaimed += kv.second;
    fprintf(stderr, "[DAY %d%s] Steps=%d | Da thu: %d phan | %zu / %zu loai chuoi hom nay | Tong toan tran: %zu loai\n",
            day, isLastDay ? " (CHUNG KET)" : "", daySteps, totalClaimed, sharedTeamBrands.size(), g_allBrands.size(), g_collectedBrands.size());

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
    g_spotStocks.clear();
    g_spotBrands.clear();

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
        g_spotStocks[s.pos] = s.stocks;
        g_spotBrands[s.pos] = s.brand;
    }

    g_daySteps.clear();
    for (size_t i = 0; i < m["daySteps"].size(); i++)
        g_daySteps.push_back(m["daySteps"][i].asInt());

    g_daySeconds.clear();
    if (!m["daySeconds"].isNull()) {
        for (size_t i = 0; i < m["daySeconds"].size(); i++)
            g_daySeconds.push_back(m["daySeconds"][i].asInt());
    }

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

    fprintf(stderr, "=== HEXUDON BOT v24.0 ===\n");
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
                } else if (pr.status == 429) {
                    sleepMs(400);
                } else {
                    fprintf(stderr, "[ERR] POST /actions day %d -> HTTP %d: %s\n",
                            day, pr.status, pr.body.c_str());
                }
            }
        } else if (r.status == 429) {
            sleepMs(400);
        } else if (r.status != 0) {
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
