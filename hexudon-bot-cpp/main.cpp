// ========================================================================
//  HEXUDON BOT v80.0 (MID-ROUTE MULTI-RENDEZVOUS & GLOBAL MARGINAL UDON PLANNER)
// ========================================================================
//  Đột Phá Kiến Trúc Tối Thượng: Phá Bỏ Rào Cản Bình Xăng & Kết Nối Xe Tuần Tra - Xe Bồn
//    1. MID-ROUTE MULTI-RENDEZVOUS:
//       Xe tuần tra không còn bị giới hạn bởi lượng xăng đầu ngày (pFuel).
//       Tour di chuyển được mở rộng linh hoạt: [S1, S2, R1, S3, S4, R2, S5].
//       Tại điểm hẹn R, xe bồn tiếp tế nạp đầy g_maxFuel ngay giữa ngày với Zero-Wait,
//       cho phép xe tuần tra chạy 100% công suất bước đi suốt cả ngày!
//    2. GLOBAL MARGINAL UDON PLANNER:
//       Quy hoạch toàn cục cân đối giữa lợi ích thu hoạch thêm (Delta Udon)
//       và chi phí tổng hợp: Delta Steps = Patrol Steps + alpha * Tanker Steps.
//       Tối ưu hóa tổng lượng tô ăn được toàn đội mà không gây quá tải cho xe bồn.
//    3. ACTIVE TIME-WINDOW LOGISTICS:
//       Xe bồn phục vụ cả điểm hẹn giữa chặng (ưu tiên tuyệt đối) và điểm tiếp tế cuối ngày,
//       đảm bảo đội xe tuần tra duy trì năng lượng bền vững suốt 10 ngày trận đấu!
// ========================================================================
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
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
    constexpr int    POLL_MS          = 215; // Tối ưu hóa chu kỳ Polling (>= 200ms an toàn theo quy định BTC)
    constexpr double EMA_ALPHA        = 0.35;
    constexpr double LAMBDA_LOW       = 2.5;
    constexpr double LAMBDA_HIGH      = 0.05;
    constexpr int    TOP_CANDIDATES   = 25;
    constexpr double FUEL_SAFE_RATIO  = 0.35; // Ngưỡng an toàn xăng động (35% maxFuel)
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
    bool isMidRoute;

    RendezvousEvent() : patrolId(0), pos(0), arrivalTime(0), fuelBefore(0), fuelAfter(0), isUrgent(false), isMidRoute(false) {}
    RendezvousEvent(int pid, int p, int arr, int fb, int fa, bool urg = false, bool mid = false)
        : patrolId(pid), pos(p), arrivalTime(arr), fuelBefore(fb), fuelAfter(fa), isUrgent(urg), isMidRoute(mid) {}
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
    int maxF = min(startFuel, 250);

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

    auto calcFirstArrivalScore = [&](const vector<int>& t) -> int {
        int score = 0;
        int K = (int)t.size();
        for (int idx = 0; idx < K; idx++) {
            int spot = t[idx];
            int b = g_spotBrands[spot];
            int weight = K - idx; // Càng ở đầu tour trọng số càng cao
            if (!g_collectedBrands.count(b)) {
                score += 100 * weight; // Brand chưa từng thu hoạch toàn trận
            } else {
                score += 10 * weight;
            }
        }
        return score;
    };

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
                    } else if (newSim.totalSteps == currentSim.totalSteps) {
                        if (newSim.totalFuel < currentSim.totalFuel) {
                            isBetter = true;
                        } else if (newSim.totalFuel == currentSim.totalFuel) {
                            // TIE-BREAKER: Tối ưu hóa First-Arrival Time cho thương hiệu quý
                            if (calcFirstArrivalScore(newTour) > calcFirstArrivalScore(tour)) {
                                isBetter = true;
                            }
                        }
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
    int totalPatrolWait;
    vector<RendezvousEvent> optimizedSequence;
};

static TimeWindowTankerSimResult simulateTankerTimeWindowTour(
    int tankerStartPos,
    int daySteps,
    const vector<RendezvousEvent>& events) {

    if (events.empty()) return {true, 0, 0, {}};

    // Sắp xếp các sự kiện ứng viên theo độ ưu tiên kết hợp:
    // 1. Điểm hẹn giữa chặng lên đầu tiên (bắt buộc phải phục vụ)
    // 2. Xe khẩn cấp (isUrgent)
    // 3. Đối với điểm hẹn giữa chặng: ưu tiên thời điểm đến sớm hơn
    // 4. Xe có lượng xăng còn lại ít nhất (fuelBefore)
    // 5. Slack deadline hẹp hơn (ít thời gian rảnh rỗi trước khi hết ngày)
    // 6. Gần vị trí xe bồn hơn
    vector<RendezvousEvent> sortedEvents = events;
    sort(sortedEvents.begin(), sortedEvents.end(), [&](const RendezvousEvent& a, const RendezvousEvent& b) {
        if (a.isMidRoute != b.isMidRoute) return a.isMidRoute > b.isMidRoute;
        if (a.isUrgent != b.isUrgent) return a.isUrgent > b.isUrgent;
        if (a.isMidRoute && a.arrivalTime != b.arrivalTime) return a.arrivalTime < b.arrivalTime;
        if (a.fuelBefore != b.fuelBefore) return a.fuelBefore < b.fuelBefore;
        int distA = fastDist(tankerStartPos, a.pos);
        int distB = fastDist(tankerStartPos, b.pos);
        int slackA = (distA == INT_MAX) ? -1 : (daySteps - (distA + 1));
        int slackB = (distB == INT_MAX) ? -1 : (daySteps - (distB + 1));
        if (slackA != slackB) return slackA < slackB; // Slack hẹp hơn ưu tiên trước
        return distA < distB;
    });

    struct SeqEval {
        bool feasible;
        int finishTime;
        int patrolWait;
        int cost;
    };

    auto evaluateSeq = [&](const vector<RendezvousEvent>& s) -> SeqEval {
        int curPos = tankerStartPos;
        int curTime = 0;
        int totalPatrolWait = 0;

        for (const auto& ev : s) {
            int travel = fastDist(curPos, ev.pos);
            if (travel == INT_MAX) return {false, INT_MAX, INT_MAX, INT_MAX};

            int tArrive = curTime + travel;
            if (tArrive > ev.arrivalTime) {
                totalPatrolWait += (tArrive - ev.arrivalTime); // Patrol bị bắt đứng chờ
            }
            int tRefuel = max(tArrive, ev.arrivalTime);
            int tDepart = tRefuel + 1;

            if (tDepart > daySteps) return {false, INT_MAX, INT_MAX, INT_MAX};

            curTime = tDepart;
            curPos  = ev.pos;
        }
        // HÀM MỤC TIÊU ZERO-WAIT: Ưu tiên tối đa việc Tanker đến trước hoặc cùng lúc Patrol (WAIT = 0)
        int cost = curTime * 10 + totalPatrolWait * 25;
        return {true, curTime, totalPatrolWait, cost};
    };

    vector<RendezvousEvent> currentTour;
    int currentFinishTime = 0;
    int currentPatrolWait = 0;
    int currentCost = INT_MAX;

    // Chèn tham lam từng sự kiện theo hàm chi phí Zero-Wait
    for (const auto& ev : sortedEvents) {
        int bestK = -1;
        int minCost = INT_MAX;
        int bestFinish = INT_MAX;
        int bestWait = INT_MAX;
        vector<RendezvousEvent> bestCandTour;

        for (size_t k = 0; k <= currentTour.size(); k++) {
            vector<RendezvousEvent> candTour = currentTour;
            candTour.insert(candTour.begin() + k, ev);
            auto eval = evaluateSeq(candTour);
            if (eval.feasible && eval.cost < minCost) {
                minCost = eval.cost;
                bestFinish = eval.finishTime;
                bestWait = eval.patrolWait;
                bestK = (int)k;
                bestCandTour = candTour;
            }
        }

        if (bestK >= 0) {
            currentTour = bestCandTour;
            currentFinishTime = bestFinish;
            currentPatrolWait = bestWait;
            currentCost = minCost;

            // 2-Opt tinh chỉnh thứ tự các điểm tiếp tế triệt tiêu WAIT
            bool improved = true;
            int iters = 12;
            while (improved && iters-- > 0) {
                improved = false;
                for (size_t i = 0; i < currentTour.size() - 1; i++) {
                    for (size_t j = i + 1; j < currentTour.size(); j++) {
                        vector<RendezvousEvent> newTour = currentTour;
                        reverse(newTour.begin() + i, newTour.begin() + j + 1);
                        auto newEval = evaluateSeq(newTour);
                        if (newEval.feasible && newEval.cost < currentCost) {
                            currentTour = newTour;
                            currentFinishTime = newEval.finishTime;
                            currentPatrolWait = newEval.patrolWait;
                            currentCost = newEval.cost;
                            improved = true;
                        }
                    }
                }
            }
        } else if (ev.isMidRoute) {
            // Điểm hẹn giữa chặng là bắt buộc! Nếu xe bồn không kịp đến, toàn bộ phương án này không khả thi!
            return {false, INT_MAX, INT_MAX, {}};
        }
    }

    return {true, currentFinishTime, currentPatrolWait, currentTour};
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

    // Đối với 2 xe bồn: Vét cạn toàn bộ 2^nEvents phân hoạch để tìm min (Makespan + 2.5 * Wait)
    if (nTankers == 2) {
        int totalPartitions = 1 << nEvents;
        BestMultiTankerPartition bestRes = {false, INT_MAX, {}};
        int bestCost = INT_MAX;

        for (int mask = 0; mask < totalPartitions; mask++) {
            vector<vector<RendezvousEvent>> candidateClusters(2);
            for (int i = 0; i < nEvents; i++) {
                if ((mask >> i) & 1) candidateClusters[1].push_back(allEvents[i]);
                else                 candidateClusters[0].push_back(allEvents[i]);
            }

            auto sim0 = simulateTankerTimeWindowTour(tankerStartPositions[0], daySteps, candidateClusters[0]);
            auto sim1 = simulateTankerTimeWindowTour(tankerStartPositions[1], daySteps, candidateClusters[1]);

            if (sim0.feasible && sim1.feasible) {
                int makespan = max(sim0.finalStep, sim1.finalStep);
                int totalWait = sim0.totalPatrolWait + sim1.totalPatrolWait;
                int partitionCost = makespan * 10 + totalWait * 25;

                if (!bestRes.feasible || partitionCost < bestCost) {
                    bestRes.feasible = true;
                    bestCost = partitionCost;
                    bestRes.maxFinishTime = makespan;
                    bestRes.tankerClusters = {sim0.optimizedSequence, sim1.optimizedSequence};
                }
            }
        }
        return bestRes;
    }

    // Tổng quát cho nTankers >= 3: Phân phối min-makespan & zero-wait tối ưu
    vector<vector<RendezvousEvent>> clusters(nTankers);
    vector<RendezvousEvent> sortedEvents = allEvents;
    sort(sortedEvents.begin(), sortedEvents.end(), [](const RendezvousEvent& a, const RendezvousEvent& b) {
        if (a.isUrgent != b.isUrgent) return a.isUrgent > b.isUrgent;
        return a.fuelBefore < b.fuelBefore;
    });

    for (const auto& ev : sortedEvents) {
        int bestT = -1;
        int minResultingCost = INT_MAX;
        for (int t = 0; t < nTankers; t++) {
            vector<RendezvousEvent> candCluster = clusters[t];
            candCluster.push_back(ev);
            auto sim = simulateTankerTimeWindowTour(tankerStartPositions[t], daySteps, candCluster);
            if (sim.feasible) {
                int candCost = sim.finalStep * 10 + sim.totalPatrolWait * 25;
                if (candCost < minResultingCost) {
                    minResultingCost = candCost;
                    bestT = t;
                }
            }
        }
        if (bestT >= 0) {
            clusters[bestT].push_back(ev);
        } else {
            int minLen = INT_MAX, minT = 0;
            for (int t = 0; t < nTankers; t++) {
                if ((int)clusters[t].size() < minLen) {
                    minLen = (int)clusters[t].size();
                    minT = t;
                }
            }
            clusters[minT].push_back(ev);
        }
    }

    int maxFinish = 0;
    vector<vector<RendezvousEvent>> finalClusters(nTankers);
    for (int t = 0; t < nTankers; t++) {
        auto sim = simulateTankerTimeWindowTour(tankerStartPositions[t], daySteps, clusters[t]);
        finalClusters[t] = sim.optimizedSequence;
        maxFinish = max(maxFinish, sim.finalStep);
    }
    return {true, maxFinish, finalClusters};
}

// ========================================================================
//  KIẾN TRÚC TIẾP TẾ GIỮA CHẶNG: MÔ PHỎNG TOUR CÙNG ĐIỂM HẸN (MID-ROUTE RENDEZVOUS)
// ========================================================================
struct TourWithRendezvousResult {
    bool feasible;
    bool hasRendezvous;
    int totalSteps;
    int totalFuel;
    int rendezvousSpotIdx; // Vị trí quán trong tour diễn ra tiếp tế (-1 nếu không có)
    int rendezvousPos;     // Tọa độ ô tiếp tế
    int rendezvousTime;    // Bước thời gian diễn ra tiếp tế
    int fuelBeforeRefuel;  // Lượng xăng còn lại trước khi nạp
    int assignedTanker;    // Chỉ số xe bồn phục vụ (0 .. nTankers-1)
    int tankerCost;        // Chi phí thời gian xe bồn
    int patrolWait;        // Số bước patrol phải đợi xe bồn (thường = 0)
    vector<vector<int>> exactLegPaths; // Toàn bộ đường đi từng chặng
};

static TourWithRendezvousResult simulateTourWithRendezvous(
    int startPos,
    int startFuel,
    int maxStepsLimit,
    const vector<int>& tour,
    int patrolId,
    int daySteps,
    const vector<int>& tankerStartPositions,
    const vector<vector<RendezvousEvent>>& tankerPlannedEvents) {

    if (tour.empty()) {
        return {true, false, 0, 0, -1, -1, 0, 0, -1, 0, 0, {}};
    }

    // 1. Kiểm tra nếu đi được độc lập bằng xăng hiện tại (không cần gọi xe bồn giữa chặng)
    auto noRendezSim = simulateTourExact(startPos, startFuel, maxStepsLimit, tour);
    if (noRendezSim.feasible) {
        return {true, false, noRendezSim.totalSteps, noRendezSim.totalFuel, -1, -1, 0, 0, -1, 0, 0, noRendezSim.exactLegPaths};
    }

    // Nếu không có xe bồn hoặc tour chỉ có 1 quán mà không đi nổi -> Thất bại
    int nTankers = (int)tankerStartPositions.size();
    if (nTankers == 0 || tour.size() <= 1) {
        return {false, false, INT_MAX, INT_MAX, -1, -1, 0, 0, -1, INT_MAX, INT_MAX, {}};
    }

    // 2. Thử nghiệm từng quán r trong tour làm điểm hẹn tiếp tế (0 <= r < tour.size() - 1)
    TourWithRendezvousResult bestResult = {false, false, INT_MAX, INT_MAX, -1, -1, 0, 0, -1, INT_MAX, INT_MAX, {}};
    double bestMarginalCost = 1e9;

    int K = (int)tour.size();
    for (int r = 0; r < K - 1; r++) {
        // Chặng 1: Từ xuất phát đến quán tour[r] bằng xăng hiện tại
        vector<int> subTour1(tour.begin(), tour.begin() + r + 1);
        auto sim1 = simulateTourExact(startPos, startFuel, maxStepsLimit, subTour1);
        if (!sim1.feasible) continue;

        int tR = sim1.totalSteps;
        int fuelBefore = startFuel - sim1.totalFuel;
        int rPos = tour[r];

        // Chặng 2: Từ quán tour[r] đến hết tour với bình xăng đầy (g_maxFuel)
        // Xe bồn gặp tại rPos. Tiếp tế tốn ít nhất 1 step.
        int stepsRemaining = maxStepsLimit - tR - 1;
        if (stepsRemaining <= 0) continue;

        vector<int> subTour2(tour.begin() + r + 1, tour.end());
        auto sim2 = simulateTourExact(rPos, g_maxFuel, stepsRemaining, subTour2);
        if (!sim2.feasible) continue;

        // 3. Tìm xe bồn có khả năng đến rPos vào thời điểm tR với Zero-Wait (hoặc wait <= 1)
        int bestTanker = -1;
        int minTankerCost = INT_MAX;
        int bestPatrolWait = INT_MAX;

        for (int t = 0; t < nTankers; t++) {
            // Lọc bỏ sự kiện tiếp tế giữa chặng cũ của chính patrol này nếu có
            vector<RendezvousEvent> candEvents;
            for (const auto& ev : tankerPlannedEvents[t]) {
                if (ev.patrolId != patrolId) {
                    candEvents.push_back(ev);
                }
            }
            candEvents.push_back({patrolId, rPos, tR, fuelBefore, g_maxFuel, false, true});

            auto tSim = simulateTankerTimeWindowTour(tankerStartPositions[t], daySteps, candEvents);
            if (tSim.feasible && tSim.totalPatrolWait <= 1) {
                int tCost = tSim.finalStep;
                if (tCost < minTankerCost) {
                    minTankerCost = tCost;
                    bestTanker = t;
                    bestPatrolWait = tSim.totalPatrolWait;
                }
            }
        }

        if (bestTanker != -1) {
            int totalPatrolSteps = sim1.totalSteps + bestPatrolWait + 1 + sim2.totalSteps;
            int totalPatrolFuel  = sim1.totalFuel + sim2.totalFuel;
            double marginalCost  = (double)totalPatrolSteps + 0.4 * (double)minTankerCost;

            if (marginalCost < bestMarginalCost) {
                bestMarginalCost = marginalCost;

                vector<vector<int>> combinedPaths = sim1.exactLegPaths;
                for (const auto& leg : sim2.exactLegPaths) {
                    combinedPaths.push_back(leg);
                }

                bestResult = {
                    true, true, totalPatrolSteps, totalPatrolFuel,
                    r, rPos, tR, fuelBefore,
                    bestTanker, minTankerCost, bestPatrolWait,
                    combinedPaths
                };
            }
        }
    }

    return bestResult;
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
    // Phase 1 đã ghép Hungarian tối ưu 1 quán/xe có tính khả thi cao nhất.
    // Toàn bộ các quán tiếp theo (multi-spot TSP packing) được ủy quyền cho Phase 2
    // để kiểm tra nghiệm khả thi (simulateTourExact) nghiêm ngặt về cả Fuel & Steps.

    // ── PHASE 2: PURE LEXICOGRAPHIC PIPELINE WITH MULTI-SPOT TSP PACKING & MID-ROUTE RENDEZVOUS ───
    int currentTeamPortions = 0;
    for (int p = 0; p < nPatrols; p++) {
        currentTeamPortions += (int)patrolTours[p].size();
    }

    vector<TourWithRendezvousResult> patrolTourResults(nPatrols);
    vector<vector<RendezvousEvent>> tankerPlannedEvents(nTankers);

    for (int p = 0; p < nPatrols; p++) {
        int startP = startPositions[p];
        int pFuel  = agents[patrolIds[p]].fuel;
        bool startsOnSpot = false;
        for (auto& sp : g_spots) if (sp.pos == startP) { startsOnSpot = true; break; }
        int pMaxSteps = maxPatrolPhysicalSteps - (startsOnSpot ? 1 : 0);

        patrolTourResults[p] = simulateTourWithRendezvous(
            startP, pFuel, pMaxSteps, patrolTours[p],
            p, daySteps, tankerStartPositions, tankerPlannedEvents
        );
    }

    auto getEndPosOfPatrol = [&](int p, const vector<int>& tour) {
        return (tour.empty()) ? startPositions[p] : tour.back();
    };

    for (;;) {
        auto curNow = chrono::high_resolution_clock::now();
        if (chrono::duration<double, milli>(curNow - startTime).count() > 1500.0) {
            break; // Chrono Watchdog: Đảm bảo phản hồi luôn dưới 1.8 giây, an toàn tuyệt đối dưới mốc 15s của BTC!
        }

        int bestPatrol = -1;
        int bestSpot = -1;
        LexicographicScore bestLexScore = {-1, -1, -1, -1e9, -1e9};
        vector<int> bestTourCandidate;
        TourWithRendezvousResult bestSimResult;

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
            bool startsOnSpot = false;
            for (auto& sp : g_spots) {
                if (sp.pos == startP) { startsOnSpot = true; break; }
            }
            int pMaxSteps = maxPatrolPhysicalSteps - (startsOnSpot ? 1 : 0);

            const auto& currentSim = patrolTourResults[p];
            int currentSteps = currentSim.feasible ? currentSim.totalSteps : 0;

            struct CandidateEntry {
                int brandPriority; // 2: new global, 1: new daily, 0: duplicate
                int detourDist;
                int spotPos;
                int bestK;

                bool operator<(const CandidateEntry& other) const {
                    if (brandPriority != other.brandPriority)
                        return brandPriority > other.brandPriority; // Ưu tiên brand mới lên trước
                    return detourDist < other.detourDist;           // Detour phát sinh thấp hơn lên trước
                }
            };

            vector<CandidateEntry> candidateSpots;
            for (auto& sp : g_spots) {
                if (projectedStock[sp.pos] <= 0) continue;
                bool alreadyIn = false;
                for (int pos : patrolTours[p]) if (pos == sp.pos) { alreadyIn = true; break; }
                if (alreadyIn) continue;

                int bestK = (int)patrolTours[p].size();
                int minExtraDist = INT_MAX;

                if (patrolTours[p].empty()) {
                    bestK = 0;
                    minExtraDist = fastDist(startP, sp.pos);
                } else {
                    for (size_t k = 0; k <= patrolTours[p].size(); k++) {
                        int prevPos = (k == 0) ? startP : patrolTours[p][k - 1];
                        int nextPos = (k == patrolTours[p].size()) ? -1 : patrolTours[p][k];
                        int extra = fastDist(prevPos, sp.pos);
                        if (nextPos != -1) {
                            extra += fastDist(sp.pos, nextPos) - fastDist(prevPos, nextPos);
                        }
                        if (extra < minExtraDist) {
                            minExtraDist = extra;
                            bestK = (int)k;
                        }
                    }
                }

                if (minExtraDist <= pMaxSteps) {
                    int bPri = 0;
                    if (!g_collectedBrands.count(sp.brand)) bPri = 2;
                    else if (!teamPlannedBrands.count(sp.brand)) bPri = 1;

                    candidateSpots.push_back({bPri, minExtraDist, sp.pos, bestK});
                }
            }
            sort(candidateSpots.begin(), candidateSpots.end());
            if ((int)candidateSpots.size() > cfg::TOP_CANDIDATES) {
                candidateSpots.resize(cfg::TOP_CANDIDATES);
            }

            for (const auto& cSpot : candidateSpots) {
                int spotPos = cSpot.spotPos;
                int spotBrand = g_spotBrands[spotPos];
                int bestK = cSpot.bestK;

                bool isDynamicallyOwned = (dynamicVoronoiOwner[spotPos] == p);

                vector<int> candidateTour = patrolTours[p];
                candidateTour.insert(candidateTour.begin() + bestK, spotPos);

                auto candSim = simulateTourWithRendezvous(
                    startP, pFuel, pMaxSteps, candidateTour,
                    p, daySteps, tankerStartPositions, tankerPlannedEvents
                );
                if (!candSim.feasible) continue;

                int deltaSteps = max(1, candSim.totalSteps - currentSteps);
                double effectiveCost = candSim.hasRendezvous
                    ? ((double)deltaSteps + 0.4 * (double)candSim.tankerCost)
                    : (double)deltaSteps;

                LexicographicScore candScore;
                candScore.p1_newGlobalBrand = (!g_collectedBrands.count(spotBrand)) ? 1 : 0;
                candScore.p2_newDailyBrand  = (!teamPlannedBrands.count(spotBrand)) ? 1 : 0;
                candScore.p3_portions       = min(projectedStock[spotPos], 1);
                candScore.p4_efficiency     = 1000.0 / max(1.0, effectiveCost);

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
                    bestSimResult = candSim;
                }
            }
        }

        if (bestPatrol < 0) break;

        // CHỈ CHẠY 2-OPT CHO TOUR ĐỘC LẬP KHÔNG RENDEZVOUS
        if (!bestSimResult.hasRendezvous && bestTourCandidate.size() > 2) {
            int startP = startPositions[bestPatrol];
            int pFuel  = agents[patrolIds[bestPatrol]].fuel;
            bool startsOnSpot = false;
            for (auto& sp : g_spots) if (sp.pos == startP) { startsOnSpot = true; break; }
            int pMaxSteps = maxPatrolPhysicalSteps - (startsOnSpot ? 1 : 0);

            auto optTour = optimizeTour2OptExact(startP, pFuel, pMaxSteps, bestTourCandidate);
            auto optSim  = simulateTourExact(startP, pFuel, pMaxSteps, optTour);
            if (optSim.feasible) {
                bestTourCandidate = optTour;
                bestSimResult = {true, false, optSim.totalSteps, optSim.totalFuel, -1, -1, 0, 0, -1, 0, 0, optSim.exactLegPaths};
            }
        }

        patrolTours[bestPatrol] = bestTourCandidate;
        patrolTourResults[bestPatrol] = bestSimResult;

        // Cập nhật cam kết của xe bồn: Xóa sự kiện cũ của bestPatrol trên mọi xe bồn
        for (int t = 0; t < nTankers; t++) {
            vector<RendezvousEvent> updatedEvents;
            for (const auto& ev : tankerPlannedEvents[t]) {
                if (ev.patrolId != bestPatrol) {
                    updatedEvents.push_back(ev);
                }
            }
            tankerPlannedEvents[t] = updatedEvents;
        }

        // Nếu tour mới có điểm hẹn tiếp tế, ghi nhận vào xe bồn được chỉ định
        if (bestSimResult.hasRendezvous && bestSimResult.assignedTanker >= 0 && bestSimResult.assignedTanker < nTankers) {
            tankerPlannedEvents[bestSimResult.assignedTanker].push_back({
                bestPatrol,
                bestSimResult.rendezvousPos,
                bestSimResult.rendezvousTime,
                bestSimResult.fuelBeforeRefuel,
                g_maxFuel,
                false,
                true // isMidRoute
            });
        }

        projectedStock[bestSpot]--;
        teamPlannedBrands.insert(g_spotBrands[bestSpot]);
        currentTeamPortions += 1;
    }

    // ── BƯỚC 4: THỰC THI DI CHUYỂN QUA CÁC QUÁN RIÊNG BIỆT (1 BÁT / MỖI QUÁN GHÉ THĂM) ──
    vector<vector<int>> allActions(nAgs);
    vector<int>         endPos(nAgs);
    vector<int>         patrolArrivalSteps(nPatrols, 0);
    vector<int>         actualPatrolEndFuel(nPatrols, 0);
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

        // Thu hoạch ngay tại chỗ nếu ô xuất phát là quán ăn (CẦN PHÁT HÀNH ĐỘNG -1 ĐỂ SERVER GHI NHẬN)
        if (tryClaimThisPatrol(curPos)) {
            if (stepsUsed < maxPatrolPhysicalSteps) {
                allActions[pi].push_back(-1);
                patrolTimelines[p].push_back(curPos);
                stepsUsed += 1;
            }
        }

        const auto& tourSim = patrolTourResults[p];

        for (size_t legIdx = 0; legIdx < tourSim.exactLegPaths.size(); legIdx++) {
            const vector<int>& path = tourSim.exactLegPaths[legIdx];

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

            // TIẾP TẾ GIỮA CHẶNG: Khi đến quán hẹn tiếp tế, chờ nạp xăng và reset curFuel về g_maxFuel
            if (tourSim.hasRendezvous && (int)legIdx == tourSim.rendezvousSpotIdx) {
                int refuelWait = 1 + max(0, tourSim.patrolWait);
                if (stepsUsed + refuelWait <= maxPatrolPhysicalSteps) {
                    allActions[pi].push_back(-refuelWait);
                    for (int s = 0; s < refuelWait; s++) patrolTimelines[p].push_back(curPos);
                    stepsUsed += refuelWait;
                    curFuel = g_maxFuel; // Tiếp tế thành công!
                }
            }
        }

        patrolArrivalSteps[p]  = stepsUsed;
        actualPatrolEndFuel[p] = curFuel; // Lưu lại lượng xăng thực tế còn lại sau chặng đua

        int rest = daySteps - stepsUsed;
        if (rest > 0) {
            allActions[pi].push_back(-rest);
            for (int s = 0; s < rest; s++) patrolTimelines[p].push_back(curPos);
        }
        endPos[pi] = curPos;
    }

    // ── BƯỚC 5: TỐI ƯU HÓA TIẾP TẾ XE BỒN (MID-ROUTE + END-OF-DAY ACTIVE DISPATCH) ────
    vector<RendezvousEvent> endOfDayCandidates;
    int urgentThreshold = max(10, (int)(cfg::FUEL_SAFE_RATIO * g_maxFuel));
    for (int p = 0; p < nPatrols; p++) {
        int fLeft = actualPatrolEndFuel[p];
        if (fLeft < g_maxFuel) {
            endOfDayCandidates.push_back({
                p,
                endPos[patrolIds[p]],
                patrolArrivalSteps[p],
                fLeft,
                g_maxFuel,
                (fLeft < urgentThreshold),
                false // isMidRoute = false
            });
        }
    }

    sort(endOfDayCandidates.begin(), endOfDayCandidates.end(), [&](const RendezvousEvent& a, const RendezvousEvent& b) {
        if (a.isUrgent != b.isUrgent) return a.isUrgent > b.isUrgent;
        return a.fuelBefore < b.fuelBefore;
    });

    vector<vector<RendezvousEvent>> finalTankerEvents = tankerPlannedEvents;
    for (const auto& ev : endOfDayCandidates) {
        int bestT = -1;
        int minCost = INT_MAX;
        for (int t = 0; t < nTankers; t++) {
            vector<RendezvousEvent> candList = finalTankerEvents[t];
            candList.push_back(ev);
            auto tSim = simulateTankerTimeWindowTour(tankerStartPositions[t], daySteps, candList);
            if (tSim.feasible) {
                int cost = tSim.finalStep * 10 + tSim.totalPatrolWait * 25;
                if (cost < minCost) {
                    minCost = cost;
                    bestT = t;
                }
            }
        }
        if (bestT >= 0) {
            finalTankerEvents[bestT].push_back(ev);
        }
    }

    for (int t = 0; t < nTankers; t++) {
        int ti = tankerIds[t];
        allActions[ti] = planSingleTankerRouteTimeWindow(
            agents[ti].pos, daySteps, traffic, finalTankerEvents[t]
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

static void recordMatchBenchmark(const string& resultJson) {
    string csvPath = "match_history.csv";
    string jsonlPath = "match_history.jsonl";

    {
        ifstream testParent("../benchmark.py");
        if (testParent.good()) {
            csvPath = "../match_history.csv";
            jsonlPath = "../match_history.jsonl";
        }
    }

    int myRank = 0, udonTypes = 0, dailyTypesSum = 0, udonTotal = 0, respMsTotal = 0;
    string teamId = "";
    int oppRank = 0, oppUdonTypes = 0, oppDailySum = 0, oppUdonTotal = 0, oppRespMs = 0;
    string oppTeamId = "";

    try {
        auto resObj = mj::parse(resultJson);
        if (resObj) {
            const auto& standings = (*resObj)["standings"];
            if (standings.size() > 0) {
                const auto& s0 = standings[0];
                myRank = s0["rank"].asInt();
                teamId = s0["team_id"].str;
                udonTypes = s0["udon_types"].asInt();
                dailyTypesSum = s0["daily_types_sum"].asInt();
                udonTotal = s0["udon_total"].asInt();
                respMsTotal = s0["response_ms_total"].asInt();
            }
            if (standings.size() > 1) {
                const auto& s1 = standings[1];
                oppRank = s1["rank"].asInt();
                oppTeamId = s1["team_id"].str;
                oppUdonTypes = s1["udon_types"].asInt();
                oppDailySum = s1["daily_types_sum"].asInt();
                oppUdonTotal = s1["udon_total"].asInt();
                oppRespMs = s1["response_ms_total"].asInt();
            }
        }
    } catch (...) {}

    // Tính các chỉ số điều tiết sản lượng chuyên sâu (Pacing & Stability Metrics)
    double mean = 0, sqSum = 0;
    double sumCap = 0, sumTarget = 0, sumFloor = 0;
    int floorViolations = 0;
    vector<int> portions;
    for (auto& s : g_historyStats) {
        portions.push_back(s.collected);
        mean += s.collected;
        sumCap += s.opportunity;
        sumTarget += s.target;
        sumFloor += s.floor;
        if (s.collected < s.floor) floorViolations++;
    }
    double nStats = (double)max(1, (int)g_historyStats.size());
    double avgCap = sumCap / nStats;
    double avgTarget = sumTarget / nStats;
    double avgFloor = sumFloor / nStats;
    mean /= nStats;

    for (auto& s : g_historyStats) {
        sqSum += (s.collected - mean) * (s.collected - mean);
    }
    double stdDev = sqrt(sqSum / nStats);
    double cv = (mean > 0) ? (stdDev / mean) : 0.0;
    double targetAttainment = (avgTarget > 0) ? (mean / avgTarget * 100.0) : 100.0;

    time_t t = time(nullptr);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", localtime(&t));
    string ts = timeBuf;

    string version = "v80.0";

    // 1. Ghi JSONL với chi tiết chuyên sâu từng ngày
    {
        ofstream fJsonl(jsonlPath.c_str(), ios::app);
        if (fJsonl.is_open()) {
            fJsonl << "{\"timestamp\": \"" << ts << "\", "
                   << "\"version\": \"" << version << "\", "
                   << "\"map_w\": " << W << ", "
                   << "\"map_h\": " << H << ", "
                   << "\"total_spots\": " << g_spots.size() << ", "
                   << "\"total_brands\": " << g_allBrands.size() << ", "
                   << "\"total_agents\": " << g_nAgents << ", "
                   << "\"max_fuel\": " << g_maxFuel << ", "
                   << "\"total_days\": " << g_totalDays << ", "
                   << "\"rank\": " << myRank << ", "
                   << "\"team_id\": \"" << teamId << "\", "
                   << "\"udon_types\": " << udonTypes << ", "
                   << "\"daily_types_sum\": " << dailyTypesSum << ", "
                   << "\"udon_total\": " << udonTotal << ", "
                   << "\"response_ms_total\": " << respMsTotal << ", "
                   << "\"mean_day\": " << mean << ", "
                   << "\"std_dev\": " << stdDev << ", "
                   << "\"daily_cv\": " << cv << ", "
                   << "\"avg_capacity\": " << avgCap << ", "
                   << "\"avg_target\": " << avgTarget << ", "
                   << "\"avg_floor\": " << avgFloor << ", "
                   << "\"target_attainment_pct\": " << targetAttainment << ", "
                   << "\"floor_violations\": " << floorViolations << ", "
                   << "\"daily_portions\": [";
            for (size_t i = 0; i < portions.size(); i++) {
                if (i > 0) fJsonl << ", ";
                fJsonl << portions[i];
            }
            fJsonl << "], \"daily_details\": [";
            for (size_t i = 0; i < g_historyStats.size(); i++) {
                if (i > 0) fJsonl << ", ";
                fJsonl << "{\"day\": " << g_historyStats[i].day
                       << ", \"collected\": " << g_historyStats[i].collected
                       << ", \"capacity\": " << g_historyStats[i].opportunity
                       << ", \"target\": " << g_historyStats[i].target
                       << ", \"floor\": " << g_historyStats[i].floor << "}";
            }
            fJsonl << "], "
                   << "\"opponent_rank\": " << oppRank << ", "
                   << "\"opponent_team_id\": \"" << oppTeamId << "\", "
                   << "\"opponent_udon_types\": " << oppUdonTypes << ", "
                   << "\"opponent_daily_sum\": " << oppDailySum << ", "
                   << "\"opponent_udon_total\": " << oppUdonTotal << ", "
                   << "\"opponent_response_ms\": " << oppRespMs
                   << "}\n";
            fJsonl.flush();
        }
    }

    // 2. Ghi CSV
    {
        bool fileExists = false;
        {
            ifstream check(csvPath.c_str());
            if (check.is_open() && check.peek() != ifstream::traits_type::eof()) {
                fileExists = true;
            }
        }
        ofstream fCsv(csvPath.c_str(), ios::app);
        if (fCsv.is_open()) {
            if (!fileExists) {
                fCsv << "timestamp,version,map_w,map_h,total_spots,total_brands,total_agents,max_fuel,total_days,rank,udon_types,daily_types_sum,udon_total,response_ms_total,mean_day,std_dev,daily_cv,avg_capacity,avg_target,avg_floor,target_attainment_pct,floor_violations\n";
            }
            fCsv << ts << ","
                 << version << ","
                 << W << ","
                 << H << ","
                 << g_spots.size() << ","
                 << g_allBrands.size() << ","
                 << g_nAgents << ","
                 << g_maxFuel << ","
                 << g_totalDays << ","
                 << myRank << ","
                 << udonTypes << ","
                 << dailyTypesSum << ","
                 << udonTotal << ","
                 << respMsTotal << ","
                 << mean << ","
                 << stdDev << ","
                 << cv << ","
                 << avgCap << ","
                 << avgTarget << ","
                 << avgFloor << ","
                 << targetAttainment << ","
                 << floorViolations << "\n";
            fCsv.flush();
        }
    }

    fprintf(stderr, "\n============================================================\n");
    fprintf(stderr, "[AUTO-BENCHMARK] Da tu dong luu ket qua tran dau toan dien!\n");
    fprintf(stderr, "  - File: %s & %s\n", csvPath.c_str(), jsonlPath.c_str());
    fprintf(stderr, "  - Official: Rank %d | Udon: %d/%zu | Daily Brands: %d | Tong: %d phan | Resp: %d ms\n",
            myRank, udonTypes, g_allBrands.size(), dailyTypesSum, udonTotal, respMsTotal);
    fprintf(stderr, "  - Pacing: Cap=%.1f | Target=%.1f | Floor=%.1f | Mean=%.1f | CV=%.2f | Dat: %.1f%%\n",
            avgCap, avgTarget, avgFloor, mean, cv, targetAttainment);
    if (floorViolations > 0) {
        fprintf(stderr, "  - Canh bao: Co %d ngay bi tut duoi nguong an toan Floor!\n", floorViolations);
    } else {
        fprintf(stderr, "  - On dinh: 100%% cac ngay deu dat vuot nguong an toan Floor (0 violations)\n");
    }
    fprintf(stderr, "============================================================\n\n");
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
    http::Client client(base, token);

    string assignBody;
    for (;;) {
        auto r = client.request("GET", "/setup");
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

    fprintf(stderr, "=== HEXUDON BOT v80.0 (MID-ROUTE MULTI-RENDEZVOUS & GLOBAL MARGINAL UDON PLANNER) ===\n");
    fprintf(stderr, "[SETUP] Map %dx%d | %zu spots | %zu brands | %d agents | maxFuel=%d | %d days\n",
            W, H, g_spots.size(), g_allBrands.size(), g_nAgents, g_maxFuel, g_totalDays);

    for (;;) {
        auto r = client.request("POST", "/assignment", assignBody);
        if (r.status == 200) break;
        if (r.status == 0 || r.status == 429) { sleepMs(cfg::POLL_MS); continue; }
        fprintf(stderr, "POST /assignment -> HTTP %d\n", r.status);
        return 1;
    }
    fprintf(stderr, "[ASSIGNED] %s\n", assignBody.c_str());
    sleepMs(100); // Giữ khoảng cách 100ms sau assignment để xả sạch rate-limit bucket trước khi Day 0 bắt đầu

    int lastDay = -1;
    for (;;) {
        auto r = client.request("GET", "/state");
        if (r.status == 200) {
            auto v = mj::parse(r.body);
            int day = (*v)["day"].asInt();

            if (day != lastDay) {
                auto dayStartTime = chrono::steady_clock::now();
                int dayAllowedSec = (day >= 0 && day < (int)g_daySeconds.size()) ? g_daySeconds[day] : 10;
                if (dayAllowedSec <= 0) dayAllowedSec = 10;
                auto dayDeadline = dayStartTime + chrono::milliseconds(dayAllowedSec * 1000 - 300); // Trừ 300ms an toàn

                string acts = planActions(*v);

                // GỬI ACTIONS VỚI DEADLINE WATCHDOG & RAPID EMERGENCY FLUSH
                bool postSuccess = false;
                int backoffMs = 80;
                for (int retry = 0; retry < 12; retry++) {
                    auto now = chrono::steady_clock::now();
                    long long msLeft = chrono::duration_cast<chrono::milliseconds>(dayDeadline - now).count();

                    // Nếu còn <= 1500ms trước deadline: KÍCH HOẠT RAPID FLUSH (timeout ngắn 800ms, retry dồn dập 40ms)
                    bool isEmergency = (msLeft <= 1500);
                    int reqTimeout = isEmergency ? 800 : 1500;

                    auto pr = client.request("POST", "/actions", acts, reqTimeout);
                    if (pr.status == 200) {
                        postSuccess = true;
                        break;
                    }

                    if (isEmergency) {
                        fprintf(stderr, "[EMERGENCY WATCHDOG] Day %d con lai %lldms! Rapid retry %d/12...\n", day, msLeft, retry + 1);
                        sleepMs(40); // Bắn dồn dập cách nhau 40ms
                        continue;
                    }

                    if (pr.status == 429) {
                        fprintf(stderr, "[RATE-LIMIT] POST /actions day %d -> HTTP 429, retry %d/12 sau %dms...\n", day, retry + 1, backoffMs);
                        sleepMs(backoffMs);
                        backoffMs = min(1500, backoffMs * 2);
                        continue;
                    }

                    fprintf(stderr, "[ERR] POST /actions day %d -> HTTP %d: %s\n",
                            day, pr.status, pr.body.c_str());
                    sleepMs(backoffMs);
                    backoffMs = min(1500, backoffMs * 2);
                }

                if (!postSuccess) {
                    // Cứu nguy cận hạn tuyệt đối: Bắn gói Safe Fallback Action đứng yên để giữ điểm
                    auto now = chrono::steady_clock::now();
                    long long msLeft = chrono::duration_cast<chrono::milliseconds>(dayDeadline - now).count();
                    if (msLeft > 0) {
                        int daySteps = (day >= 0 && day < (int)g_daySteps.size()) ? g_daySteps[day] : 30;
                        ostringstream safeOut;
                        safeOut << "[";
                        for (int i = 0; i < g_nAgents; i++) {
                            if (i) safeOut << ",";
                            safeOut << "[-" << daySteps << "]";
                        }
                        safeOut << "]";
                        fprintf(stderr, "[SAFE FALLBACK] Ban goi hanh dong an toan de cuu diem Day %d!\n", day);
                        auto pr = client.request("POST", "/actions", safeOut.str(), 600);
                        if (pr.status == 200) postSuccess = true;
                    }
                }

                if (postSuccess) {
                    lastDay = day; // CHỈ KHÓA DAY KHI XÁC NHẬN SERVER ĐÃ NHẬN THÀNH CÔNG (HTTP 200)
                } else {
                    fprintf(stderr, "[WARN] POST /actions day %d chua thanh cong sau 12 lan thu, se retry o chu ky tiep theo...\n", day);
                }
            }
        } else if (r.status == 429) {
            sleepMs(400); // Backoff nhanh nếu GET /state bị rate-limit
        } else if (r.status != 0) {
            auto rr = client.request("GET", "/result");
            if (rr.status == 200) {
                fprintf(stderr, "[RESULT] %s\n", rr.body.c_str());
                const char* envRunner = getenv("HEXUDON_BENCHMARK_RUNNER");
                if (!envRunner || string(envRunner) != "1") {
                    recordMatchBenchmark(rr.body);
                }
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
