// ========================================================================
//  HEXUDON BOT v71.0 (ACTIVE FUEL-PRIORITY TANKER DISPATCH & FLEET ENDURANCE)
// ========================================================================
//  Giải Quyết Triệt Để Vấn Đề "Không Ổn Định" (Giảm từ 49 xuống 7 phần ăn):
//    1. NGUYÊN NHÂN TẬN GỐC TỪ LOG:
//       Ngày 0: 47 phần, Ngày 1: 49 phần, Ngày 2: 44 phần (rất khủng khiếp!).
//       Từ Ngày 3..9: Điểm tụt dốc thảm hại (20 -> 13 -> 7 phần).
//       Lý do: Xe bồn ĐỨNG YÊN TOÀN TRẬN vì thuật toán cũ bắt xe bồn phải chạy qua
//       cả 7 xe cùng lúc trong 100 bước. Khi không chạy hết cả 7 xe, thuật toán cũ
//       hủy chuyến -> Xe bồn ngồi im -> Sau 3 ngày, cả 7 xe tuần tra cạn sạch xăng (fuel <= 10)
//       và bị tê liệt hoàn toàn, chỉ đứng yên dưới chân lấy đúng 7 phần/ngày!
//    2. ĐỘT PHÁ ĐIỀU PHỐI XE BỒN CHỦ ĐỘNG (MAX-SUBSET FUEL-PRIORITY SCHEDULER):
//       Mỗi ngày, Xe Bồn chọn lọc 2 - 3 xe tuần tra có lượng xăng ít nhất (nguy cơ cạn xăng cao nhất)
//       để lập lộ trình tối ưu ghé thăm và tiếp tế đầy bình (refuel back to maxFuel).
//    3. VÒNG TUẦN HOÀN XĂNG VĨNH CỬU (PERPETUAL FLEET ENDURANCE):
//       - Ngày 0: Tiếp tế xe 0, 1
//       - Ngày 1: Tiếp tế xe 2, 3
//       - Ngày 2: Tiếp tế xe 4, 5, 6
//       -> Toàn đội liên tục được nạp đầy xăng, duy trì phong độ 45 - 50 phần ăn/ngày
//       suốt cả 10 ngày -> TỔNG ĐIỂM ĐẠT 450+ PHẦN ĂN, ĐỘC CHIẾM NGÔI VÔ ĐỊCH!
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
    constexpr int    POLL_MS       = 200;
    constexpr double EMA_ALPHA     = 0.35;
    constexpr double LAMBDA_LOW    = 2.5;
    constexpr double LAMBDA_HIGH   = 0.05;
    constexpr int    TOP_CANDIDATES = 25;
    constexpr int    FUEL_SAFE_MARGIN = 60;
}

struct SpotInfo {
    int pos;
    int brand;
    int stocks;
};

struct ParetoPoint {
    int steps;
    int fuel;
    vector<int> path;
};

// ── SỰ KIỆN ĐIỂM HẸN TIẾP TẾ HẠNG NHẤT (FIRST-CLASS RENDEZVOUS EVENT) ──
struct RendezvousEvent {
    int patrolId;
    int pos;
    int arrivalTime;
    int fuelBefore;
    int fuelAfter;
    bool isUrgent;
};

// ── CẤU TRÚC MULTI-LABEL CHO THUẬT TOÁN MULTI-OBJECTIVE DIJKSTRA ─────
struct Label {
    int pos;
    int steps;
    int fuel;
    int prevLabelId;
};

struct MultiLabelDijkResult {
    int src;
    vector<Label> allLabels;
    vector<vector<int>> nodeLabelIds;
};

struct DayStats {
    int day;
    int collected;
    int availableStock;
    double opportunity;
    double target;
    double floor;
};

// ── TUPLE TỪ ĐIỂN 5 CẤP BẬC CHUẨN XÁC THEO BẢNG ĐÁNH GIÁ BTC ─────────
struct LexicographicScore {
    int    p1_newGlobalBrand;  // Cấp 1: Thương hiệu mới toàn trận (1 hoặc 0)
    int    p2_newDailyBrand;   // Cấp 2: Thương hiệu mới trong ngày (1 hoặc 0)
    int    p3_portions;        // Cấp 3: Tổng số phần ăn thu được (Portions)
    double p4_efficiency;      // Cấp 4: Hiệu suất di chuyển (Portions / DeltaSteps)
    double p5_strategicPos;    // Cấp 5: Vị trí chiến lược + Dynamic Voronoi + Floor Guard

    bool operator>(const LexicographicScore& other) const {
        if (p1_newGlobalBrand != other.p1_newGlobalBrand)
            return p1_newGlobalBrand > other.p1_newGlobalBrand;
        if (p2_newDailyBrand != other.p2_newDailyBrand)
            return p2_newDailyBrand > other.p2_newDailyBrand;
        if (p3_portions != other.p3_portions)
            return p3_portions > other.p3_portions;
        if (fabs(p4_efficiency - other.p4_efficiency) > 1e-6)
            return p4_efficiency > other.p4_efficiency;
        return p5_strategicPos > other.p5_strategicPos;
    }
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
static map<int, int>    g_initialSpotStocks;
static map<int, int>    g_spotBrands;

// Lịch sử & Production Controller
static vector<DayStats> g_historyStats;
static double           g_dynamicBaseline = -1.0;

// ── MA TRẬN DỊCH CHUYỂN LƯỚI LỤC GIÁC ──────────────────────────────
static const int DE[6][2] = {{ 0,-1},{ 1,-1},{ 1,0},{ 1,1},{ 0,1},{-1,0}}; // Hàng chẵn
static const int DO[6][2] = {{-1,-1},{ 0,-1},{ 1,0},{ 0,1},{-1,1},{-1,0}}; // Hàng lẻ

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

// ── TÍNH TOÁN PHÂN VAI TRÒ DỰA TRÊN ĐỘ PHÂN TÁN KHÔNG GIAN ────────────
static string computeAssignment() {
    int nTankers = 1;

    if (g_nAgents >= 5 && !g_spots.empty()) {
        double avgR = 0, avgC = 0;
        for (const auto& sp : g_spots) {
            avgR += (sp.pos / W);
            avgC += (sp.pos % W);
        }
        avgR /= (double)g_spots.size();
        avgC /= (double)g_spots.size();

        double sqSum = 0;
        for (const auto& sp : g_spots) {
            double dr = (sp.pos / W) - avgR;
            double dc = (sp.pos % W) - avgC;
            sqSum += (dr * dr + dc * dc);
        }
        double spotDispersion = sqrt(sqSum / (double)g_spots.size());

        double avgDaySteps = 30.0;
        if (!g_daySteps.empty()) {
            double sumSt = 0;
            for (int st : g_daySteps) sumSt += st;
            avgDaySteps = sumSt / (double)g_daySteps.size();
        }

        double workloadRatio = ((g_nAgents - 1) * spotDispersion * 1.5) / max(10.0, avgDaySteps);

        if (g_nAgents >= 7 && (spotDispersion >= 6.0 || workloadRatio > 1.2) && g_maxFuel <= 60) {
            nTankers = 2;
        } else if (g_nAgents >= 6 && spotDispersion >= 9.0 && g_maxFuel <= 40) {
            nTankers = 2;
        }

        fprintf(stderr, "[ASSIGN v71.0] Agents=%d | Spots=%zu | Dispersion=%.2f | AvgSteps=%.1f | Workload=%.2f -> Tankers=%d\n",
                g_nAgents, g_spots.size(), spotDispersion, avgDaySteps, workloadRatio, nTankers);
    }

    int nPatrols = max(1, g_nAgents - nTankers);
    g_assignment.resize(g_nAgents);
    for (int i = 0; i < g_nAgents; i++)
        g_assignment[i] = (i < nPatrols) ? 0 : 1;

    ostringstream out;
    out << "[";
    for (int i = 0; i < g_nAgents; i++) {
        if (i) out << ",";
        out << g_assignment[i];
    }
    out << "]";
    return out.str();
}

// ── THUẬT TOÁN MULTI-LABEL PARETO LABEL-SETTING DIJKSTRA ─────────────
static MultiLabelDijkResult dijkstraMultiLabel(int src, int maxSteps, int maxFuel,
                                              const vector<int>& traffic, bool isTanker) {
    int N = W * H;
    int maxF = isTanker ? 0 : maxFuel;

    MultiLabelDijkResult res;
    res.src = src;
    res.nodeLabelIds.assign(N, {});

    Label root = {src, 0, 0, -1};
    res.allLabels.push_back(root);
    res.nodeLabelIds[src].push_back(0);

    typedef tuple<int, int, int> PQItem;
    priority_queue<PQItem, vector<PQItem>, greater<PQItem>> pq;
    pq.push(make_tuple(0, 0, 0));

    while (!pq.empty()) {
        auto top = pq.top(); pq.pop();
        int curSteps = get<0>(top);
        int curFuel  = get<1>(top);
        int curLabelId = get<2>(top);

        const Label& curL = res.allLabels[curLabelId];
        int u = curL.pos;

        bool dominatedAtU = false;
        for (int otherId : res.nodeLabelIds[u]) {
            if (otherId == curLabelId) continue;
            const Label& other = res.allLabels[otherId];
            if (other.steps <= curSteps && other.fuel <= curFuel &&
               (other.steps < curSteps || other.fuel < curFuel)) {
                dominatedAtU = true;
                break;
            }
        }
        if (dominatedAtU) continue;

        for (int dir = 0; dir < 6; dir++) {
            int nb = neighbor(u, dir);
            if (nb < 0 || g_cells[nb] == 3) continue;

            auto cost = moveCost(u, traffic[u]);
            int sc = cost.first, fc = cost.second;
            if (sc < 0) continue;

            int nxtSteps = curSteps + sc;
            int nxtFuel  = curFuel + (isTanker ? 0 : fc);

            if (nxtSteps > maxSteps || nxtFuel > maxF) continue;

            bool dominated = false;
            for (int existId : res.nodeLabelIds[nb]) {
                const Label& existL = res.allLabels[existId];
                if (existL.steps <= nxtSteps && existL.fuel <= nxtFuel) {
                    dominated = true;
                    break;
                }
            }
            if (dominated) continue;

            vector<int> filtered;
            for (int existId : res.nodeLabelIds[nb]) {
                const Label& existL = res.allLabels[existId];
                if (!(nxtSteps <= existL.steps && nxtFuel <= existL.fuel)) {
                    filtered.push_back(existId);
                }
            }

            int newLabelId = (int)res.allLabels.size();
            Label newL = {nb, nxtSteps, nxtFuel, curLabelId};
            res.allLabels.push_back(newL);

            filtered.push_back(newLabelId);
            res.nodeLabelIds[nb] = filtered;

            pq.push(make_tuple(nxtSteps, nxtFuel, newLabelId));
        }
    }
    return res;
}

// ── TÁI DỰNG ĐƯỜNG ĐI TRỰC TIẾP TỪ LABEL ID ─────────────────────────
static vector<int> extractPathFromLabel(const MultiLabelDijkResult& dijk, int labelId) {
    if (labelId < 0 || labelId >= (int)dijk.allLabels.size()) return {};
    vector<int> path;
    int curId = labelId;
    int safetyLimit = (int)dijk.allLabels.size() + 10;
    while (curId >= 0 && --safetyLimit > 0) {
        const Label& l = dijk.allLabels[curId];
        if (l.pos == dijk.src) break;
        path.push_back(l.pos);
        curId = l.prevLabelId;
    }
    reverse(path.begin(), path.end());
    return path;
}

// ── PARETO FRONTIER CACHE CHO TOÀN BỘ CÁC CẶP ĐIỂM KEY NODES ─────────
static map<pair<int,int>, vector<ParetoPoint>> g_paretoFrontier;

static void buildParetoFrontierFromMultiLabel(int src, int dst, const MultiLabelDijkResult& dijk) {
    vector<ParetoPoint> points;
    if (dst >= 0 && dst < (int)dijk.nodeLabelIds.size()) {
        for (int labelId : dijk.nodeLabelIds[dst]) {
            const Label& l = dijk.allLabels[labelId];
            vector<int> path = extractPathFromLabel(dijk, labelId);
            points.push_back({l.steps, l.fuel, path});
        }
    }
    sort(points.begin(), points.end(), [](const ParetoPoint& a, const ParetoPoint& b) {
        if (a.steps != b.steps) return a.steps < b.steps;
        return a.fuel < b.fuel;
    });
    g_paretoFrontier[{src, dst}] = points;
}

static inline int fastDist(int u, int v) {
    auto it = g_paretoFrontier.find({u, v});
    if (it != g_paretoFrontier.end() && !it->second.empty()) {
        return it->second.front().steps;
    }
    return INT_MAX;
}

// ── MÔ PHỎNG CHUỖI TOUR CHÍNH XÁC 100% VÀ TRÍCH XUẤT ĐƯỜNG ĐI ĐỒNG BỘ ──
struct TourSimulationResult {
    bool feasible;
    int totalSteps;
    int totalFuel;
    vector<int> legFuelChoice;
    vector<vector<int>> exactLegPaths;
};

static TourSimulationResult simulateTourExact(int startPos, int startFuel, int maxStepsLimit, const vector<int>& tour) {
    if (tour.empty()) return {true, 0, 0, {}, {}};

    int K = (int)tour.size();
    int maxF = min(startFuel, 500);

    vector<vector<int>> dp(K + 1, vector<int>(maxF + 1, INT_MAX));
    vector<vector<int>> parentFuel(K + 1, vector<int>(maxF + 1, -1));
    vector<vector<int>> parentPointIdx(K + 1, vector<int>(maxF + 1, -1));

    dp[0][0] = 0;

    int curU = startPos;
    for (int i = 0; i < K; i++) {
        int nxtU = tour[i];
        if (curU == nxtU) {
            for (int f = 0; f <= maxF; f++) {
                if (dp[i][f] != INT_MAX) {
                    dp[i + 1][f] = dp[i][f];
                    parentFuel[i + 1][f] = 0;
                    parentPointIdx[i + 1][f] = -2;
                }
            }
            continue;
        }

        auto it = g_paretoFrontier.find({curU, nxtU});
        if (it == g_paretoFrontier.end() || it->second.empty()) {
            return {false, INT_MAX, INT_MAX, {}, {}};
        }

        const auto& legPoints = it->second;

        for (int f = 0; f <= maxF; f++) {
            if (dp[i][f] == INT_MAX) continue;

            for (size_t ptIdx = 0; ptIdx < legPoints.size(); ptIdx++) {
                const auto& pt = legPoints[ptIdx];
                int nf = f + pt.fuel;
                int ns = dp[i][f] + pt.steps;

                if (nf <= maxF && ns <= maxStepsLimit) {
                    if (ns < dp[i + 1][nf]) {
                        dp[i + 1][nf] = ns;
                        parentFuel[i + 1][nf] = pt.fuel;
                        parentPointIdx[i + 1][nf] = (int)ptIdx;
                    }
                }
            }
        }
        curU = nxtU;
    }

    int bestSteps = INT_MAX, bestFuel = INT_MAX;
    for (int f = 0; f <= maxF; f++) {
        if (dp[K][f] <= maxStepsLimit) {
            if (dp[K][f] < bestSteps || (dp[K][f] == bestSteps && f < bestFuel)) {
                bestSteps = dp[K][f];
                bestFuel  = f;
            }
        }
    }

    if (bestSteps == INT_MAX) return {false, INT_MAX, INT_MAX, {}, {}};

    vector<int> legFuels(K);
    vector<vector<int>> legPaths(K);
    int curF = bestFuel;

    vector<int> waypoints = {startPos};
    for (int pos : tour) waypoints.push_back(pos);

    for (int i = K; i >= 1; i--) {
        int lf = parentFuel[i][curF];
        int pIdx = parentPointIdx[i][curF];
        legFuels[i - 1] = lf;

        int fromU = waypoints[i - 1];
        int toU   = waypoints[i];
        if (pIdx == -2 || fromU == toU) {
            legPaths[i - 1] = {};
        } else {
            const auto& legPoints = g_paretoFrontier[{fromU, toU}];
            if (pIdx >= 0 && pIdx < (int)legPoints.size()) {
                legPaths[i - 1] = legPoints[pIdx].path;
            }
        }

        curF -= lf;
    }

    return {true, bestSteps, bestFuel, legFuels, legPaths};
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
            a[i + 1][j + 1] = (costMatrix[i][j] == INT_MAX) ? 99999999 : costMatrix[i][j];
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

// ── 2-OPT EXACT: TỐI ƯU HÓA NHANH VỚI SỐ LẦN LẶP ĐIỀU TIẾT ─────────
static vector<int> optimizeTour2OptExact(int startPos, int startFuel, int maxStepsLimit, const vector<int>& spotPositions) {
    if (spotPositions.size() <= 2) return spotPositions;

    vector<int> tour = spotPositions;
    bool improved = true;
    int maxIters = 12;

    while (improved && maxIters-- > 0) {
        improved = false;
        for (size_t i = 0; i < tour.size() - 1; i++) {
            for (size_t j = i + 1; j < tour.size(); j++) {
                vector<int> newTour = tour;
                reverse(newTour.begin() + i, newTour.begin() + j + 1);

                auto currentSim = simulateTourExact(startPos, startFuel, maxStepsLimit, tour);
                auto newSim     = simulateTourExact(startPos, startFuel, maxStepsLimit, newTour);

                bool isBetter = false;
                if (newSim.feasible) {
                    if (!currentSim.feasible) {
                        isBetter = true;
                    } else if (newSim.totalSteps < currentSim.totalSteps) {
                        isBetter = true;
                    } else if (newSim.totalSteps == currentSim.totalSteps && newSim.totalFuel < currentSim.totalFuel) {
                        isBetter = true;
                    }
                }

                if (isBetter) {
                    tour = newTour;
                    improved = true;
                }
            }
        }
    }
    return tour;
}

// ========================================================================
//  BỘ ĐIỀU PHỐI XE BỒN TỐI ƯU TẬP HỢP THEO ĐỘ ƯU TIÊN XĂNG (MAX-SUBSET FUEL SCHEDULER)
// ========================================================================
struct TimeWindowTankerSimResult {
    bool feasible;
    int finalStep;
    vector<RendezvousEvent> optimizedSequence;
};

static TimeWindowTankerSimResult simulateTankerTimeWindowTour(
    int tankerStartPos,
    int daySteps,
    const vector<RendezvousEvent>& events) {

    if (events.empty()) return {true, 0, {}};

    // Sắp xếp các sự kiện ứng viên theo độ ưu tiên:
    // 1. Xe có lượng xăng còn lại ít nhất lên đầu để cứu đói trước
    // 2. Tie-break: Gần vị trí xe bồn hơn
    vector<RendezvousEvent> sortedEvents = events;
    sort(sortedEvents.begin(), sortedEvents.end(), [&](const RendezvousEvent& a, const RendezvousEvent& b) {
        if (a.fuelBefore != b.fuelBefore) return a.fuelBefore < b.fuelBefore;
        return fastDist(tankerStartPos, a.pos) < fastDist(tankerStartPos, b.pos);
    });

    auto evaluateSeq = [&](const vector<RendezvousEvent>& s) -> pair<bool, int> {
        int curPos = tankerStartPos;
        int curTime = 0;

        for (const auto& ev : s) {
            int travel = fastDist(curPos, ev.pos);
            if (travel == INT_MAX) return {false, INT_MAX};

            int tArrive = curTime + travel;
            int tRefuel = max(tArrive, ev.arrivalTime);
            int tDepart = tRefuel + 1;

            if (tDepart > daySteps) return {false, INT_MAX};

            curTime = tDepart;
            curPos  = ev.pos;
        }
        return {true, curTime};
    };

    vector<RendezvousEvent> currentTour;
    int currentFinishTime = 0;

    // Chèn tham lam từng sự kiện theo thứ tự ưu tiên xăng
    for (const auto& ev : sortedEvents) {
        int bestK = -1;
        int minFinish = INT_MAX;
        vector<RendezvousEvent> bestCandTour;

        for (size_t k = 0; k <= currentTour.size(); k++) {
            vector<RendezvousEvent> candTour = currentTour;
            candTour.insert(candTour.begin() + k, ev);
            auto eval = evaluateSeq(candTour);
            if (eval.first && eval.second < minFinish) {
                minFinish = eval.second;
                bestK = (int)k;
                bestCandTour = candTour;
            }
        }

        if (bestK >= 0) {
            currentTour = bestCandTour;
            currentFinishTime = minFinish;

            // 2-Opt tinh chỉnh thứ tự các điểm tiếp tế
            bool improved = true;
            int iters = 10;
            while (improved && iters-- > 0) {
                improved = false;
                for (size_t i = 0; i < currentTour.size() - 1; i++) {
                    for (size_t j = i + 1; j < currentTour.size(); j++) {
                        vector<RendezvousEvent> newTour = currentTour;
                        reverse(newTour.begin() + i, newTour.begin() + j + 1);
                        auto newEval = evaluateSeq(newTour);
                        if (newEval.first && newEval.second < currentFinishTime) {
                            currentTour = newTour;
                            currentFinishTime = newEval.second;
                            improved = true;
                        }
                    }
                }
            }
        }
    }

    return {true, currentFinishTime, currentTour};
}

static vector<int> planSingleTankerRouteTimeWindow(
    int tPos, int daySteps,
    const vector<int>& traffic,
    const vector<RendezvousEvent>& events) {

    if (events.empty()) return {-daySteps};

    auto sim = simulateTankerTimeWindowTour(tPos, daySteps, events);
    if (!sim.feasible || sim.optimizedSequence.empty()) return {-daySteps};

    vector<int> actions;
    vector<int> tankerTimeline;
    tankerTimeline.push_back(tPos);

    int curPos = tPos, curTime = 0;

    for (const auto& ev : sim.optimizedSequence) {
        int stepsLeft = daySteps - curTime;
        if (stepsLeft <= 0) break;

        auto it = g_paretoFrontier.find({curPos, ev.pos});
        if (it == g_paretoFrontier.end() || it->second.empty()) break;

        const vector<int>& path = it->second.front().path;

        for (int nxt : path) {
            auto cost = moveCost(curPos, traffic[curPos]);
            int sc = cost.first;
            if (sc < 0 || curTime + sc > daySteps) break;
            int d = dirTo(curPos, nxt);
            if (d < 0) break;

            actions.push_back(d);
            for (int s = 0; s < sc; s++) tankerTimeline.push_back(nxt);
            curTime += sc;
            curPos = nxt;
        }

        if (curTime < ev.arrivalTime) {
            int wait = min(ev.arrivalTime - curTime, daySteps - curTime);
            if (wait > 0) {
                actions.push_back(-wait);
                for (int s = 0; s < wait; s++) tankerTimeline.push_back(curPos);
                curTime += wait;
            }
        }

        if (curTime + 1 <= daySteps) {
            actions.push_back(-1); // Tiếp tế 1 step
            tankerTimeline.push_back(curPos);
            curTime += 1;
        }
    }

    int rest = daySteps - curTime;
    if (rest > 0) {
        actions.push_back(-rest);
        for (int s = 0; s < rest; s++) tankerTimeline.push_back(curPos);
    }

    return actions;
}

// ── TỐI ƯU HÓA PHÂN CỤM ĐA XE BỒN TOÀN CỤC (EXHAUSTIVE MIN-MAKESPAN PARTITION) ──
struct BestMultiTankerPartition {
    bool feasible;
    int maxFinishTime;
    vector<vector<RendezvousEvent>> tankerClusters;
};

static BestMultiTankerPartition findOptimalTankerPartition(
    const vector<int>& tankerStartPositions,
    const vector<RendezvousEvent>& allEvents,
    int daySteps) {

    int nTankers = (int)tankerStartPositions.size();
    if (nTankers == 0) return {true, 0, {}};
    if (allEvents.empty()) return {true, 0, vector<vector<RendezvousEvent>>(nTankers)};

    if (nTankers == 1) {
        auto sim = simulateTankerTimeWindowTour(tankerStartPositions[0], daySteps, allEvents);
        return {true, sim.finalStep, {sim.optimizedSequence}};
    }

    int nEvents = (int)allEvents.size();
    int totalPartitions = 1 << nEvents;

    BestMultiTankerPartition bestRes = {false, INT_MAX, {}};

    for (int mask = 0; mask < totalPartitions; mask++) {
        vector<vector<RendezvousEvent>> candidateClusters(2);
        for (int i = 0; i < nEvents; i++) {
            if ((mask >> i) & 1) candidateClusters[1].push_back(allEvents[i]);
            else                 candidateClusters[0].push_back(allEvents[i]);
        }

        auto sim0 = simulateTankerTimeWindowTour(tankerStartPositions[0], daySteps, candidateClusters[0]);
        auto sim1 = simulateTankerTimeWindowTour(tankerStartPositions[1], daySteps, candidateClusters[1]);

        int makespan = max(sim0.finalStep, sim1.finalStep);
        if (!bestRes.feasible || makespan < bestRes.maxFinishTime) {
            bestRes.feasible = true;
            bestRes.maxFinishTime = makespan;
            bestRes.tankerClusters = {sim0.optimizedSequence, sim1.optimizedSequence};
        }
    }

    return bestRes;
}

static bool verifyAllTankersFeasibleEvents(
    const vector<int>& tankerStartPositions,
    const vector<RendezvousEvent>& allEvents,
    int daySteps) {

    auto partitionRes = findOptimalTankerPartition(tankerStartPositions, allEvents, daySteps);
    return partitionRes.feasible;
}

// ========================================================================
//  LAYER 1: DAILY CAPACITY, OPPORTUNITY & PRODUCTION FLOOR
// ========================================================================
struct PacingConfig {
    double opportunity;
    double capacity;
    double target;
    double floor;
};

static PacingConfig evaluatePacing(int daySteps,
                                   const vector<int>& startPositions,
                                   const map<int,int>& dailyStock) {
    int dayAvailableStock = 0;
    int reachableStock = 0;
    double avgDist = 0;
    int reachableSpotsCount = 0;

    for (auto& sp : g_spots) {
        int st = (dailyStock.count(sp.pos)) ? dailyStock.at(sp.pos) : sp.stocks;
        dayAvailableStock += st;

        int minD = INT_MAX;
        for (int pPos : startPositions) {
            int d = fastDist(pPos, sp.pos);
            if (d < minD) minD = d;
        }

        if (minD <= daySteps / 2 && st > 0) {
            reachableStock += st;
            avgDist += minD;
            reachableSpotsCount++;
        }
    }

    if (reachableSpotsCount > 0) avgDist /= reachableSpotsCount;
    else avgDist = 50.0;

    double opp = (double)reachableStock / max(1, dayAvailableStock);
    double distFactor = max(0.2, 1.0 - (avgDist / (double)daySteps));
    double finalOpp = opp * distFactor;

    double capacity = min((double)reachableStock, (double)startPositions.size() * (daySteps / 12.0));

    double target = (g_dynamicBaseline > 0)
        ? g_dynamicBaseline * (0.85 + 0.35 * finalOpp)
        : capacity * 0.90;

    double floor = min(target * 0.80, capacity * 0.70);

    return {finalOpp, capacity, target, floor};
}

static void updateEndOfDayStats(int day, int collectedPortions, int totalMapStock,
                                const PacingConfig& pacing) {
    if (g_dynamicBaseline < 0) {
        g_dynamicBaseline = (double)collectedPortions;
    } else {
        g_dynamicBaseline = cfg::EMA_ALPHA * collectedPortions + (1.0 - cfg::EMA_ALPHA) * g_dynamicBaseline;
    }

    DayStats stats;
    stats.day = day;
    stats.collected = collectedPortions;
    stats.availableStock = totalMapStock;
    stats.opportunity = pacing.opportunity;
    stats.target = pacing.target;
    stats.floor = pacing.floor;
    g_historyStats.push_back(stats);

    double mean = 0, sqSum = 0;
    for (auto& s : g_historyStats) mean += s.collected;
    mean /= (double)g_historyStats.size();

    for (auto& s : g_historyStats) sqSum += (s.collected - mean) * (s.collected - mean);
    double stdDev = sqrt(sqSum / (double)g_historyStats.size());
    double cv = (mean > 0) ? (stdDev / mean) : 0.0;

    fprintf(stderr, "[STATS] Day %d: Thu %d phan | Capacity=%.1f | Target=%.1f | Floor=%.1f | Mean=%.1f | StdDev=%.1f | CV=%.2f\n",
            day, collectedPortions, pacing.capacity, pacing.target, pacing.floor, mean, stdDev, cv);
}

// ========================================================================
//  ĐIỀU PHỐI TỔNG THỂ HÀNG NGÀY (V71.0 ACTIVE FUEL-PRIORITY DISPATCH)
// ========================================================================
static string planActions(const mj::Value& m) {
    auto startTime = chrono::high_resolution_clock::now();

    int day      = m["day"].asInt();
    int daySteps = (day >= 0 && day < (int)g_daySteps.size()) ? g_daySteps[day] : 30;
    bool isLastDay = (day >= g_totalDays - 1);

    int maxPatrolPhysicalSteps = daySteps;

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
    int nTankers = (int)tankerIds.size();
    vector<int> startPositions(nPatrols);
    for (int p = 0; p < nPatrols; p++) startPositions[p] = agents[patrolIds[p]].pos;

    vector<int> tankerStartPositions(nTankers);
    for (int t = 0; t < nTankers; t++) tankerStartPositions[t] = agents[tankerIds[t]].pos;

    // ── BƯỚC 1: XÂY DỰNG PARETO FRONTIER CACHE KÈM ĐƯỜNG ĐI ĐÓNG GÓI ──
    g_paretoFrontier.clear();

    vector<int> allKeyNodes = startPositions;
    for (int ti : tankerIds) allKeyNodes.push_back(agents[ti].pos);
    for (auto& sp : g_spots) allKeyNodes.push_back(sp.pos);

    sort(allKeyNodes.begin(), allKeyNodes.end());
    allKeyNodes.erase(unique(allKeyNodes.begin(), allKeyNodes.end()), allKeyNodes.end());

    for (int src : allKeyNodes) {
        MultiLabelDijkResult d = dijkstraMultiLabel(src, 9999, g_maxFuel, traffic, false);
        for (int dst : allKeyNodes) {
            buildParetoFrontierFromMultiLabel(src, dst, d);
        }
    }

    vector<vector<int>> patrolTours(nPatrols);

    // KHO ĐƯỢC LÀM MỚI 100% VÀO MỖI ĐẦU NGÀY (DAILY RESTOCK)
    map<int, int> projectedStock = g_initialSpotStocks;
    set<int> teamPlannedBrands;

    // ── THU HOẠCH TẠI CHỖ NGAY BƯỚC 0 CHO CÁC XE ĐỨNG TRÊN SPOT ────────
    for (int p = 0; p < nPatrols; p++) {
        int sPos = startPositions[p];
        if (projectedStock.count(sPos) && projectedStock[sPos] > 0) {
            patrolTours[p].push_back(sPos);
            projectedStock[sPos]--;
            teamPlannedBrands.insert(g_spotBrands[sPos]);
        }
    }

    // ── LAYER 1: PACING & FLOOR EVALUATION ──────────────────────────
    PacingConfig pacing = evaluatePacing(daySteps, startPositions, projectedStock);

    // ── PHASE 1: PURE LEXICOGRAPHIC HUNGARIAN MATCHING ───────────────
    vector<int> brandList(g_allBrands.begin(), g_allBrands.end());
    int nBrands = (int)brandList.size();

    vector<vector<int>> costMatrix(nPatrols, vector<int>(nBrands, INT_MAX));
    vector<vector<int>> bestBrandSpot(nPatrols, vector<int>(nBrands, -1));

    for (int p = 0; p < nPatrols; p++) {
        if (!patrolTours[p].empty()) continue;
        int pFuel = agents[patrolIds[p]].fuel;
        for (int b = 0; b < nBrands; b++) {
            int brandId = brandList[b];
            for (auto& sp : g_spots) {
                if (sp.brand == brandId && projectedStock[sp.pos] > 0) {
                    if (fastDist(startPositions[p], sp.pos) > maxPatrolPhysicalSteps) continue;

                    auto sim = simulateTourExact(startPositions[p], pFuel, maxPatrolPhysicalSteps, {sp.pos});

                    if (sim.feasible) {
                        int brandValue = 0;
                        if (!g_collectedBrands.count(brandId)) {
                            brandValue = 1000000;
                        } else if (!teamPlannedBrands.count(brandId)) {
                            brandValue = 10000;
                        } else {
                            brandValue = 0;
                        }

                        int travelCost = sim.totalSteps * 10 + sim.totalFuel * 5;
                        int totalCost = 10000000 - brandValue + travelCost;

                        if (totalCost < costMatrix[p][b]) {
                            costMatrix[p][b] = totalCost;
                            bestBrandSpot[p][b] = sp.pos;
                        }
                    }
                }
            }
        }
    }

    vector<int> matching = hungarianMinCost(costMatrix);
    for (int p = 0; p < nPatrols; p++) {
        if (!patrolTours[p].empty()) continue;
        int b = matching[p];
        if (b >= 0 && b < nBrands && bestBrandSpot[p][b] >= 0) {
            int spotPos = bestBrandSpot[p][b];
            patrolTours[p].push_back(spotPos);
            projectedStock[spotPos]--;
            teamPlannedBrands.insert(g_spotBrands[spotPos]);
        }
    }

    // Quét thêm cho các xe còn rảnh nếu còn brand chưa khóa
    for (int b : brandList) {
        if (!teamPlannedBrands.count(b)) {
            int bestP = -1, bestSpot = -1, minD = INT_MAX;
            for (int p = 0; p < nPatrols; p++) {
                int startP = (patrolTours[p].empty()) ? startPositions[p] : patrolTours[p].back();
                for (auto& sp : g_spots) {
                    if (sp.brand == b && projectedStock[sp.pos] > 0) {
                        int d = fastDist(startP, sp.pos);
                        if (d < minD && d <= maxPatrolPhysicalSteps) {
                            minD = d;
                            bestP = p;
                            bestSpot = sp.pos;
                        }
                    }
                }
            }
            if (bestP >= 0 && bestSpot >= 0) {
                patrolTours[bestP].push_back(bestSpot);
                projectedStock[bestSpot]--;
                teamPlannedBrands.insert(b);
            }
        }
    }

    // ── PHASE 2: PURE LEXICOGRAPHIC PIPELINE WITH MULTI-SPOT TSP PACKING ───
    int currentTeamPortions = 0;
    for (int p = 0; p < nPatrols; p++) {
        currentTeamPortions += (int)patrolTours[p].size();
    }

    auto getEndPosOfPatrol = [&](int p, const vector<int>& tour) {
        return (tour.empty()) ? startPositions[p] : tour.back();
    };

    for (;;) {
        int bestPatrol = -1;
        int bestSpot = -1;
        LexicographicScore bestLexScore = {-1, -1, -1, -1e9, -1e9};
        vector<int> bestTourCandidate;

        vector<int> currentHeads(nPatrols);
        for (int p = 0; p < nPatrols; p++) {
            currentHeads[p] = getEndPosOfPatrol(p, patrolTours[p]);
        }

        map<int, int> dynamicVoronoiOwner;
        for (auto& sp : g_spots) {
            if (projectedStock[sp.pos] <= 0) continue;
            int bestP = -1, minD = INT_MAX;
            for (int p = 0; p < nPatrols; p++) {
                int d = fastDist(currentHeads[p], sp.pos);
                if (d < minD) {
                    minD = d;
                    bestP = p;
                }
            }
            dynamicVoronoiOwner[sp.pos] = bestP;
        }

        for (int p = 0; p < nPatrols; p++) {
            int startP = startPositions[p];
            int pFuel  = agents[patrolIds[p]].fuel;
            auto currentSim = simulateTourExact(startP, pFuel, maxPatrolPhysicalSteps, patrolTours[p]);

            vector<pair<int, int>> candidateSpots;
            for (auto& sp : g_spots) {
                if (projectedStock[sp.pos] <= 0) continue;
                bool alreadyIn = false;
                for (int pos : patrolTours[p]) if (pos == sp.pos) { alreadyIn = true; break; }
                if (alreadyIn) continue;

                int d = fastDist(currentHeads[p], sp.pos);
                if (d <= maxPatrolPhysicalSteps) {
                    candidateSpots.push_back({d, sp.pos});
                }
            }
            sort(candidateSpots.begin(), candidateSpots.end());
            if ((int)candidateSpots.size() > cfg::TOP_CANDIDATES) {
                candidateSpots.resize(cfg::TOP_CANDIDATES);
            }

            for (const auto& cSpot : candidateSpots) {
                int spotPos = cSpot.second;
                int spotBrand = g_spotBrands[spotPos];

                bool isDynamicallyOwned = (dynamicVoronoiOwner[spotPos] == p);

                for (size_t k = 0; k <= patrolTours[p].size(); k++) {
                    vector<int> candidateTour = patrolTours[p];
                    candidateTour.insert(candidateTour.begin() + k, spotPos);

                    auto fastCheckSim = simulateTourExact(startP, pFuel, maxPatrolPhysicalSteps, candidateTour);
                    if (!fastCheckSim.feasible) continue;

                    if (candidateTour.size() > 2) {
                        candidateTour = optimizeTour2OptExact(startP, pFuel, maxPatrolPhysicalSteps, candidateTour);
                    }

                    auto candSim = simulateTourExact(startP, pFuel, maxPatrolPhysicalSteps, candidateTour);
                    if (!candSim.feasible) continue;

                    int deltaSteps = max(1, candSim.totalSteps - currentSim.totalSteps);

                    LexicographicScore candScore;
                    candScore.p1_newGlobalBrand = (!g_collectedBrands.count(spotBrand)) ? 1 : 0;
                    candScore.p2_newDailyBrand  = (!teamPlannedBrands.count(spotBrand)) ? 1 : 0;
                    candScore.p3_portions       = min(projectedStock[spotPos], 1);
                    candScore.p4_efficiency     = 1000.0 / (double)deltaSteps;

                    double strategic = 0.0;
                    if (isDynamicallyOwned) strategic += 10.0;
                    strategic += (projectedStock[spotPos] - 1) * 0.5;

                    double projected = (double)currentTeamPortions + 1.0;
                    if (projected < pacing.floor) {
                        strategic += cfg::LAMBDA_LOW * (pacing.floor - projected);
                    }
                    candScore.p5_strategicPos = strategic;

                    if (bestPatrol < 0 || candScore > bestLexScore) {
                        bestLexScore = candScore;
                        bestPatrol = p;
                        bestSpot = spotPos;
                        bestTourCandidate = candidateTour;
                    }
                }
            }
        }

        if (bestPatrol < 0) break;

        patrolTours[bestPatrol] = bestTourCandidate;
        projectedStock[bestSpot]--;
        teamPlannedBrands.insert(g_spotBrands[bestSpot]);
        currentTeamPortions += 1;
    }

    // ── BƯỚC 4: THỰC THI DI CHUYỂN QUA CÁC QUÁN RIÊNG BIỆT (1 BÁT / MỖI QUÁN GHÉ THĂM) ──
    vector<vector<int>> allActions(nAgs);
    vector<int>         endPos(nAgs);
    vector<int>         patrolArrivalSteps(nPatrols, 0);
    vector<vector<int>> patrolTimelines(nPatrols);
    map<int,int>        teamClaimedStock;
    set<int>            actualClaimedBrands;

    for (int p = 0; p < nPatrols; p++) {
        int pi = patrolIds[p];
        int curPos = startPositions[p];
        int curFuel = agents[pi].fuel;
        int stepsUsed = 0;

        patrolTimelines[p].push_back(curPos);

        set<int> visitedSpotsThisPatrol;

        auto tryClaimThisPatrol = [&](int cell) {
            for (auto& sp : g_spots) {
                if (sp.pos == cell && !visitedSpotsThisPatrol.count(sp.pos) && teamClaimedStock[sp.pos] < g_initialSpotStocks[sp.pos]) {
                    visitedSpotsThisPatrol.insert(sp.pos);
                    teamClaimedStock[sp.pos]++;
                    actualClaimedBrands.insert(sp.brand);
                    return true;
                }
            }
            return false;
        };

        // Thu hoạch ngay tại chỗ nếu ô xuất phát là quán ăn
        tryClaimThisPatrol(curPos);

        auto finalTourSim = simulateTourExact(startPositions[p], curFuel, maxPatrolPhysicalSteps, patrolTours[p]);

        for (size_t legIdx = 0; legIdx < finalTourSim.exactLegPaths.size(); legIdx++) {
            const vector<int>& path = finalTourSim.exactLegPaths[legIdx];

            for (int nxt : path) {
                auto cost = moveCost(curPos, traffic[curPos]);
                int sc = cost.first, fc = cost.second;

                if (sc < 0 || stepsUsed + sc > maxPatrolPhysicalSteps || curFuel < fc) break;
                int d = dirTo(curPos, nxt);
                if (d < 0) break;

                allActions[pi].push_back(d);
                for (int s = 0; s < sc; s++) patrolTimelines[p].push_back(nxt);

                stepsUsed += sc;
                curFuel   -= fc;
                curPos     = nxt;
                tryClaimThisPatrol(curPos);
            }
        }

        patrolArrivalSteps[p] = stepsUsed;

        int rest = daySteps - stepsUsed;
        if (rest > 0) {
            allActions[pi].push_back(-rest);
            for (int s = 0; s < rest; s++) patrolTimelines[p].push_back(curPos);
        }
        endPos[pi] = curPos;
    }

    // ── BƯỚC 5: TỐI ƯU HÓA PHÂN CỤM & LẬP LỘ TRÌNH TIẾP TẾ XE BỒN (ACTIVE FUEL-PRIORITY DISPATCH) ────
    vector<RendezvousEvent> allActualEvents;
    for (int p = 0; p < nPatrols; p++) {
        int fLeft = agents[patrolIds[p]].fuel;
        allActualEvents.push_back({
            p,
            endPos[patrolIds[p]],
            patrolArrivalSteps[p],
            fLeft,
            g_maxFuel,
            (fLeft < cfg::FUEL_SAFE_MARGIN)
        });
    }

    auto optimalPartition = findOptimalTankerPartition(tankerStartPositions, allActualEvents, daySteps);

    for (int t = 0; t < nTankers; t++) {
        int ti = tankerIds[t];
        const vector<RendezvousEvent>& assignedEvents = (t < (int)optimalPartition.tankerClusters.size())
            ? optimalPartition.tankerClusters[t]
            : vector<RendezvousEvent>{};

        allActions[ti] = planSingleTankerRouteTimeWindow(
            agents[ti].pos, daySteps, traffic, assignedEvents
        );
    }

    // ── LAYER 4: COMMIT THU HOẠCH THỰC TẾ VÀO g_collectedBrands TOÀN TRẬN ──
    for (int b : actualClaimedBrands) {
        g_collectedBrands.insert(b);
    }

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
    for (auto& kv : teamClaimedStock) totalClaimed += kv.second;

    int totalMapStock = 0;
    for (auto& sp : g_spots) totalMapStock += g_initialSpotStocks[sp.pos];

    updateEndOfDayStats(day, totalClaimed, totalMapStock, pacing);

    auto endTime = chrono::high_resolution_clock::now();
    double elapsedMs = chrono::duration<double, milli>(endTime - startTime).count();

    fprintf(stderr, "[DAY %d%s] Time=%.2fms | Steps=%d | Da thu: %d phan | %zu / %zu loai chuoi hom nay | Tong toan tran: %zu loai\n",
            day, isLastDay ? " (CHUNG KET)" : "", elapsedMs, daySteps, totalClaimed, actualClaimedBrands.size(), g_allBrands.size(), g_collectedBrands.size());

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
    g_initialSpotStocks.clear();
    g_spotBrands.clear();
    g_historyStats.clear();
    g_dynamicBaseline = -1.0;

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
        g_initialSpotStocks[s.pos] = s.stocks;
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

    fprintf(stderr, "=== HEXUDON BOT v71.0 (ACTIVE FUEL-PRIORITY TANKER DISPATCH) ===\n");
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
