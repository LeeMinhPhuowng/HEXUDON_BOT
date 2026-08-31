// ========================================================================
//  HEXUDON BOT v1.1 — Hệ thống Điều phối Đa tác nhân Tối ưu Hóa Đột Phá
// ========================================================================
//  Cải tiến trọng tâm v1.1:
//    • Sửa triệt để tên trường JSON API BTC (brand, stocks, fuelLimits)
//    • Daily Diversity First: Tối đa hóa 60/60 điểm lũy kế chuỗi Udon mỗi ngày
//    • Multi-Target Max-Portion Greedy: Tối ưu thu 300+ phần Udon toàn trận
//    • Phân chia kho độc lập theo từng xe trong ngày (per-patrol single visit)
//    • Điều phối Tanker tiếp tế chủ động đón đầu xe cạn xăng cuối ngày
// ========================================================================
#include <algorithm>
#include <chrono>
#include <climits>
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

// ========================================================================
//  §1  THAM SỐ ĐIỀU CHỈNH CHIẾN THUẬT (cfg namespace)
// ========================================================================
namespace cfg {
    // ── Trọng số chọn mục tiêu (scoreSpot) ──────────────────────────
    constexpr double W_GLOBAL_DIVERSITY = 10000.0; // Thưởng chuỗi CHƯA TỪNG ĂN toàn trận
    constexpr double W_DAILY_DIVERSITY  = 5000.0;  // Thưởng chuỗi HÔM NAY CHƯA ĂN (Tiêu chí #2)
    constexpr double W_PORTION_BASE     = 200.0;   // Thưởng cơ bản cho mỗi phần Udon (Tiêu chí #3)
    constexpr double W_DISTANCE         = 1.5;     // Phạt mỗi step di chuyển
    constexpr double W_CENTER_BIAS      = 0.3;     // Kéo về trung tâm cuối ngày
    constexpr double W_TANKER_PROX      = 0.5;     // Kéo về xe tiếp tế cuối ngày

    // ── Quản lý nhiên liệu ──────────────────────────────────────────
    constexpr double FUEL_RESERVE       = 0.05;    // Giữ lại 5% xăng tối thiểu (hạn chế lãng phí)
    constexpr double END_DAY_PHASE      = 0.75;    // Bật kéo về Tanker khi dùng >75% steps

    // ── Giao tiếp API ───────────────────────────────────────────────
    constexpr int    POLL_MS            = 250;     // Chu kỳ poll an toàn tránh HTTP 429
}

// ========================================================================
//  §2  CẤU TRÚC DỮ LIỆU
// ========================================================================
struct SpotInfo {
    int pos;           // Vị trí ô phẳng (row * W + col)
    int brand;         // ID chuỗi nhượng quyền Udon (chuẩn BTC)
    int stocks;        // Số phần kho tối đa mỗi ngày
};

struct DijkResult {
    vector<int> dist;  // dist[v] = số steps ít nhất tới ô v (INT_MAX nếu không tới được)
    vector<int> fuel;  // fuel[v] = lượng xăng tiêu tốn trên đường ngắn nhất
    vector<int> prev;  // prev[v] = ô trước đó để truy vết đường đi
};

// ========================================================================
//  §3  TRẠNG THÁI TOÀN CỤC (GLOBAL STATE)
// ========================================================================
static int W = 0, H = 0;              // Kích thước bản đồ
static int g_nAgents   = 0;           // Tổng số tác nhân
static int g_maxFuel   = 40;          // Dung lượng xăng xe tuần tra
static int g_totalDays = 0;           // Tổng số ngày trận đấu

static vector<int>      g_cells;      // 0: đồng bằng, 1: đường, 2: núi, 3: ao
static vector<SpotInfo> g_spots;      // Danh sách tất cả điểm Udon
static vector<int>      g_daySteps;   // Ngân sách steps từng ngày
static vector<int>      g_assignment; // Vai trò: 0 = tuần tra, 1 = tiếp tế

// Theo dõi độ đa dạng chuỗi Udon
static set<int> g_collectedBrands;    // Toàn trận (không bao giờ reset)

// ========================================================================
//  §4  HỆ TỌA ĐỘ LƯỚI LỤC GIÁC (EVEN-R OFFSET)
// ========================================================================
//  0: Trên-Trái, 1: Trên-Phải, 2: Phải, 3: Dưới-Phải, 4: Dưới-Trái, 5: Trái
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
    return {-1, -1};                               // Ao hoặc không hợp lệ
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

// ========================================================================
//  §5  THUẬT TOÁN DIJKSTRA TỐI ƯU CÓ TRỌNG SỐ
// ========================================================================
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
            if (nb < 0 || g_cells[nb] == 3) continue; // Cấm đi vào Ao

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

// ========================================================================
//  §6  PHÂN BỔ VAI TRÒ TÁC NHÂN (ASSIGNMENT)
// ========================================================================
static string computeAssignment() {
    // Chiến lược thực chiến: 1 Tanker phục vụ (N-1) Patrols
    // Đảm bảo tối đa hóa lực lượng đi ăn Udon (7 Patrols với đội 8 xe)
    int nTankers = (g_nAgents >= 7 && (W * H >= 32 * 32)) ? 2 : 1;
    if (g_nAgents <= 3) nTankers = 1;
    int nPatrols = max(1, g_nAgents - nTankers);

    g_assignment.resize(g_nAgents);
    for (int i = 0; i < g_nAgents; i++)
        g_assignment[i] = (i < nPatrols) ? 0 : 1;

    fprintf(stderr, "[ASSIGN] %d Patrols (san Udon) + %d Tankers (tiep te)\n", nPatrols, nTankers);

    ostringstream out;
    out << "[";
    for (int i = 0; i < g_nAgents; i++) {
        if (i) out << ",";
        out << g_assignment[i];
    }
    out << "]";
    return out.str();
}

// ========================================================================
//  §7  LẬP TUYẾN TUẦN TRA ĐA MỤC TIÊU (PATROL PLANNING)
// ========================================================================
static double scoreSpot(const SpotInfo& spot, const DijkResult& dijk,
                         int stepsUsed, int daySteps, int tankerPos,
                         const set<int>& dayCollectedBrands) {
    if (dijk.dist[spot.pos] == INT_MAX) return -1e18;

    double score = cfg::W_PORTION_BASE;

    // ── 1. ĐA DẠNG TOÀN TRẬN (Tiêu chí #1) ─────────────────────────
    if (!g_collectedBrands.count(spot.brand)) {
        score += cfg::W_GLOBAL_DIVERSITY;
    }

    // ── 2. ĐA DẠNG TRONG NGÀY (Tiêu chí #2 - Quyết định 60/60) ─────
    if (!dayCollectedBrands.count(spot.brand)) {
        score += cfg::W_DAILY_DIVERSITY;
    }

    // ── 3. PHẠT KHOẢNG CÁCH (Khoảng cách ngắn hơn = điểm cao hơn) ──
    score -= dijk.dist[spot.pos] * cfg::W_DISTANCE;

    // ── 4. KÉO VỀ VỊ TRÍ THUẬN LỢI CUỐI NGÀY ──────────────────────
    double progress = daySteps > 0 ? (double)stepsUsed / daySteps : 0.0;
    if (progress > cfg::END_DAY_PHASE) {
        double bias = (progress - cfg::END_DAY_PHASE) / (1.0 - cfg::END_DAY_PHASE);
        score -= hexDist(spot.pos, mapCenter()) * cfg::W_CENTER_BIAS * bias;
        if (tankerPos >= 0)
            score -= hexDist(spot.pos, tankerPos) * cfg::W_TANKER_PROX * bias;
    }

    return score;
}

static vector<int> planPatrolRoute(int pos, int fuel, int daySteps,
                                    const vector<int>& traffic,
                                    map<int,int>& claimedStock,
                                    set<int>& dayCollectedBrands,
                                    int tankerPos, bool isLastDay) {
    vector<int> actions;
    int stepsUsed = 0;
    int curPos    = pos;
    int curFuel   = fuel;
    set<int> visitedSpotsByMe; // Mỗi xe chỉ thu 1 lần/điểm/ngày

    int fuelReserve = isLastDay ? 0 : (int)(g_maxFuel * cfg::FUEL_RESERVE);
    if (curFuel <= fuelReserve) fuelReserve = 0;

    // Vòng lặp tham lam liên tục: Ăn xong điểm này $\rightarrow$ Tìm ngay điểm tiếp theo
    while (stepsUsed < daySteps && curFuel > fuelReserve) {
        int stepsLeft  = daySteps - stepsUsed;
        int fuelBudget = curFuel - fuelReserve;
        if (fuelBudget <= 0) break;

        DijkResult dijk = dijkstraAll(curPos, stepsLeft, fuelBudget, traffic, false);

        int    bestIdx   = -1;
        double bestScore = -1e18;

        for (int i = 0; i < (int)g_spots.size(); i++) {
            const SpotInfo& sp = g_spots[i];

            // Bỏ qua nếu xe này đã ăn điểm này hôm nay
            if (visitedSpotsByMe.count(sp.pos)) continue;
            // Bỏ qua nếu kho cả đội hôm nay đã hết
            if (claimedStock[sp.pos] >= sp.stocks) continue;
            // Bỏ qua nếu không tới được hoặc thiếu xăng
            if (dijk.dist[sp.pos] == INT_MAX || dijk.fuel[sp.pos] > fuelBudget) continue;

            double sc = scoreSpot(sp, dijk, stepsUsed, daySteps, tankerPos, dayCollectedBrands);
            if (sc > bestScore) {
                bestScore = sc;
                bestIdx   = i;
            }
        }

        if (bestIdx < 0) break; // Không còn điểm nào khả thi $\rightarrow$ Dừng

        // Đi theo đường Dijkstra đến điểm Udon tốt nhất
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

        // Ghi nhận đã thu thập thành công
        visitedSpotsByMe.insert(g_spots[bestIdx].pos);
        claimedStock[g_spots[bestIdx].pos]++;
        dayCollectedBrands.insert(g_spots[bestIdx].brand);
        g_collectedBrands.insert(g_spots[bestIdx].brand);
    }

    // Nếu còn bước và gần hết xăng: Cố gắng di chuyển về phía xe tiếp tế (nếu có thể)
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

    // Đệm bước đứng yên đảm bảo tổng steps = daySteps
    int rest = daySteps - stepsUsed;
    if (rest > 0) actions.push_back(-rest);

    return actions;
}

// ========================================================================
//  §8  LẬP TUYẾN XE TIẾP TẾ (TANKER PLANNING - ESCORT / RENDEZVOUS)
// ========================================================================
static vector<int> planTankerRoute(int tPos, int daySteps,
                                    const vector<int>& traffic,
                                    const vector<tuple<int,int,int>>& patrolInfo) {
    if (patrolInfo.empty()) return {-daySteps};

    // 1. Ưu tiên tìm xe tuần tra CẠN XĂNG NHẤT
    int targetPos = -1;
    int minFuel   = INT_MAX;
    for (size_t k = 0; k < patrolInfo.size(); k++) {
        int pFuel = get<1>(patrolInfo[k]);
        if (pFuel < minFuel) {
            minFuel   = pFuel;
            targetPos = get<2>(patrolInfo[k]); // Đích đến là vị trí KẾT THÚC của Patrol đó
        }
    }

    // 2. Nếu mọi xe tuần tra đều dư dả xăng (>50%) -> Di chuyển về trọng tâm của nhóm
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

    // Dijkstra vô hạn xăng cho xe tiếp tế
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
//  §9  ĐIỀU PHỐI TỔNG THỂ HÀNG NGÀY (ACTION ORCHESTRATOR)
// ========================================================================
static string planActions(const mj::Value& m) {
    int day      = m["day"].asInt();
    int daySteps = (day >= 0 && day < (int)g_daySteps.size()) ? g_daySteps[day] : 30;
    bool isLastDay = (day >= g_totalDays - 1);

    // ── 1. Đọc trạng thái giao thông ───────────────────────────────
    vector<int> traffic(W * H, 0);
    for (size_t i = 0; i < m["traffics"].size(); i++) {
        int tpos = m["traffics"][i]["pos"].asInt();
        int tst  = m["traffics"][i]["status"].asInt();
        if (tpos >= 0 && tpos < W * H) traffic[tpos] = tst;
    }

    // ── 2. Đọc trạng thái tác nhân ─────────────────────────────────
    struct AgentState { int pos, kind, fuel; };
    const mj::Value& ags = m["agents"];
    int nAgs = (int)ags.size();
    vector<AgentState> agents(nAgs);
    for (int i = 0; i < nAgs; i++) {
        agents[i].pos  = ags[i]["pos"].asInt();
        agents[i].kind = ags[i]["kind"].asInt();
        agents[i].fuel = ags[i]["fuel"].isNull() ? (1 << 30) : ags[i]["fuel"].asInt();
    }

    // Tự động phát hiện maxFuel nếu chưa có
    if (g_maxFuel <= 0) {
        for (int i = 0; i < nAgs; i++)
            if (agents[i].kind == 0)
                g_maxFuel = max(g_maxFuel, agents[i].fuel);
        if (g_maxFuel <= 0) g_maxFuel = 40;
    }

    // Phân chia danh sách ID Patrol và Tanker
    vector<int> patrolIds, tankerIds;
    for (int i = 0; i < nAgs; i++) {
        if (agents[i].kind == 0) patrolIds.push_back(i);
        else                     tankerIds.push_back(i);
    }

    // Gán cụm: Mỗi Tanker phụ trách nhóm Patrol gần nhất
    vector<vector<int>> clusters(tankerIds.size());
    if (!tankerIds.empty()) {
        for (int pi : patrolIds) {
            int bestT = 0, bestD = INT_MAX;
            for (int t = 0; t < (int)tankerIds.size(); t++) {
                int d = hexDist(agents[pi].pos, agents[tankerIds[t]].pos);
                if (d < bestD) { bestD = d; bestT = t; }
            }
            clusters[bestT].push_back(pi);
        }
    }

    vector<vector<int>> allActions(nAgs);
    vector<int>         endPos(nAgs);
    map<int,int>        claimedStock;        // Reset mỗi ngày: đếm phần Udon cả đội đã thu
    set<int>            dayCollectedBrands;  // Reset mỗi ngày: đếm các chuỗi Udon đã thu hôm nay

    auto nearestTankerPos = [&](int patrolPos) -> int {
        int best = -1, bestD = INT_MAX;
        for (int ti : tankerIds) {
            int d = hexDist(patrolPos, agents[ti].pos);
            if (d < bestD) { bestD = d; best = agents[ti].pos; }
        }
        return best;
    };

    // ── BƯỚC 1: Lập kế hoạch cho Patrols ───────────────────────────
    // Sắp xếp: Xe ít xăng hơn lập tuyến trước để ưu tiên chọn quán gần
    vector<int> patrolOrder = patrolIds;
    sort(patrolOrder.begin(), patrolOrder.end(), [&](int a, int b) {
        return agents[a].fuel < agents[b].fuel;
    });

    for (int pi : patrolOrder) {
        int tPos = nearestTankerPos(agents[pi].pos);

        allActions[pi] = planPatrolRoute(
            agents[pi].pos, agents[pi].fuel, daySteps, traffic,
            claimedStock, dayCollectedBrands, tPos, isLastDay
        );

        // Mô phỏng vị trí kết thúc ngày của Patrol
        int cur = agents[pi].pos;
        for (int a : allActions[pi]) {
            if (a >= 0) {
                int nb = neighbor(cur, a);
                if (nb >= 0) cur = nb;
            }
        }
        endPos[pi] = cur;
    }

    // ── BƯỚC 2: Lập kế hoạch cho Tankers ───────────────────────────
    for (int t = 0; t < (int)tankerIds.size(); t++) {
        int ti = tankerIds[t];
        vector<tuple<int,int,int>> clusterInfo;
        for (int pi : clusters[t]) {
            clusterInfo.push_back(make_tuple(
                agents[pi].pos, agents[pi].fuel, endPos[pi]
            ));
        }
        allActions[ti] = planTankerRoute(
            agents[ti].pos, daySteps, traffic, clusterInfo
        );
    }

    // ── BƯỚC 3: Đóng gói JSON ──────────────────────────────────────
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
    fprintf(stderr, "[DAY %d] Steps=%d | Da an: %d phan | %zu loai chuoi hom nay | Tong toan tran: %zu loai\n",
            day, daySteps, totalClaimed, dayCollectedBrands.size(), g_collectedBrands.size());

    return out.str();
}

// ========================================================================
//  §10  PARSE SETUP & GAME LOOP
// ========================================================================
static string parseSetup(const mj::Value& m) {
    const mj::Value& mp = m["map"];
    W = mp["width"].asInt();
    H = mp["height"].asInt();
    g_cells.assign((size_t)W * H, 0);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            g_cells[r * W + c] = mp["cells"][r][c].asInt();

    // ── Đọc danh sách điểm Udon chuẩn xác theo API BTC ─────────────
    g_spots.clear();
    for (size_t i = 0; i < m["spots"].size(); i++) {
        SpotInfo s;
        s.pos = m["spots"][i]["pos"].asInt();

        // Kiểm tra đúng trường "brand" của BTC
        if      (!m["spots"][i]["brand"].isNull())     s.brand = m["spots"][i]["brand"].asInt();
        else if (!m["spots"][i]["franchise"].isNull()) s.brand = m["spots"][i]["franchise"].asInt();
        else if (!m["spots"][i]["chain"].isNull())     s.brand = m["spots"][i]["chain"].asInt();
        else if (!m["spots"][i]["type"].isNull())      s.brand = m["spots"][i]["type"].asInt();
        else s.brand = 0;

        // Kiểm tra đúng trường "stocks" của BTC
        if      (!m["spots"][i]["stocks"].isNull())    s.stocks = m["spots"][i]["stocks"].asInt();
        else if (!m["spots"][i]["stock"].isNull())     s.stocks = m["spots"][i]["stock"].asInt();
        else s.stocks = 1;

        if (s.stocks <= 0) s.stocks = 1;
        g_spots.push_back(s);
    }

    g_daySteps.clear();
    for (size_t i = 0; i < m["daySteps"].size(); i++)
        g_daySteps.push_back(m["daySteps"][i].asInt());
    g_totalDays = (int)g_daySteps.size();

    // Kiểm tra đúng trường "fuelLimits" của BTC
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

// ========================================================================
//  MAIN FUNCTION
// ========================================================================
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

    // ── Phase 1: Chờ và nhận Setup ─────────────────────────────────
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

    set<int> allBrands;
    for (auto& s : g_spots) allBrands.insert(s.brand);
    fprintf(stderr, "=== HEXUDON BOT v1.1 ===\n");
    fprintf(stderr, "[SETUP] Map %dx%d | %zu spots | %zu loai brand Udon | %d agents | maxFuel=%d | %d days\n",
            W, H, g_spots.size(), allBrands.size(), g_nAgents, g_maxFuel, g_totalDays);

    // ── Phase 2: Gửi Assignment ────────────────────────────────────
    for (;;) {
        auto r = http::request(base, "POST", "/assignment", token, assignBody);
        if (r.status == 200) break;
        if (r.status == 0 || r.status == 429) { sleepMs(cfg::POLL_MS); continue; }
        fprintf(stderr, "POST /assignment -> HTTP %d\n", r.status);
        return 1;
    }
    fprintf(stderr, "[ASSIGNED] %s\n", assignBody.c_str());

    // ── Phase 3: Game Loop từng ngày ───────────────────────────────
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
            g_collectedBrands.size(), allBrands.size());
    return 0;
}
