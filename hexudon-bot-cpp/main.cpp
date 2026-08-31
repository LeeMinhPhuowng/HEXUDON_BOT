// ========================================================================
//  HEXUDON BOT v1.0 — Hệ thống Điều phối Đa tác nhân Tối ưu
// ========================================================================
//  Chiến lược tổng hợp:
//    • Dijkstra có trọng số địa hình  (thay BFS)
//    • Greedy đa mục tiêu            (ghé nhiều spot / ngày)
//    • Franchise Diversity Scoring    (ưu tiên loại udon MỚI)
//    • Tanker Escort / Rendezvous    (xe tiếp tế chủ động)
//    • End-of-Day Positioning        (vị trí & xăng cuối ngày)
//    • Dynamic Assignment            (tỉ lệ tuần tra:tiếp tế theo bản đồ)
// ========================================================================
//  Build:
//    Windows MSVC : cl /std:c++17 /O2 main.cpp (auto-link winhttp)
//    Windows g++  : g++ -std=c++17 -O2 -o bot main.cpp -lwinhttp
//    Linux / Mac  : g++ -std=c++17 -O2 -o bot main.cpp
//  Run:
//    ./bot <URL> <MATCH_ID> <TOKEN>
//    ./bot -url <URL> -match <MATCH_ID> -token <TOKEN>
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
//  §1  THAM SỐ ĐIỀU CHỈNH — Thay đổi nhanh khi thi đấu thực tế
// ========================================================================
//  Tất cả "magic numbers" tập trung ở đây. Khi cần tinh chỉnh chiến thuật,
//  chỉ sửa namespace này rồi build lại — không cần đọc logic bên dưới.
// ========================================================================
namespace cfg {
    // ── Trọng số chọn mục tiêu (§7 scoreSpot) ──────────────────────
    constexpr double W_DIVERSITY    = 1000.0;  // Thưởng franchise CHƯA thu (rất lớn)
    constexpr double W_DISTANCE     = 1.0;     // Phạt mỗi step khoảng cách
    constexpr double W_CENTER_BIAS  = 0.5;     // Hút về trung tâm bản đồ cuối ngày
    constexpr double W_TANKER_PROX  = 0.3;     // Hút về xe tiếp tế cuối ngày

    // ── Quản lý nhiên liệu (§7 planPatrolRoute) ────────────────────
    constexpr double FUEL_RESERVE   = 0.20;    // Dự trữ 20% xăng cho ngày sau

    // ── Pha cuối ngày (§7 scoreSpot) ────────────────────────────────
    constexpr double END_DAY_PHASE  = 0.60;    // Bật bias vị trí khi dùng >60% steps

    // ── Phân tích bản đồ & gán vai trò (§6) ────────────────────────
    constexpr double HARD_TERRAIN   = 0.25;    // Núi+ao > 25% → bản đồ khó
    constexpr double EASY_TERRAIN   = 0.10;    // Núi+ao < 10% → bản đồ dễ
    constexpr int    PPT_HARD       = 2;       // 1 tiếp tế : 2 tuần tra (khó)
    constexpr int    PPT_MEDIUM     = 3;       // 1 tiếp tế : 3 tuần tra (vừa)
    constexpr int    PPT_EASY       = 4;       // 1 tiếp tế : 4 tuần tra (dễ)

    // ── Giao tiếp API ───────────────────────────────────────────────
    constexpr int    POLL_MS        = 250;     // Nghỉ ≥200ms giữa request (tránh 429)
}

// ========================================================================
//  §2  CẤU TRÚC DỮ LIỆU
// ========================================================================

/// Thông tin một điểm udon trên bản đồ.
struct SpotInfo {
    int pos;           // Vị trí (flat index = row*W + col)
    int franchise;     // ID loại chuỗi udon — dùng tính đa dạng
    int maxStock;      // Sức chứa kho / ngày / đội (reset đầu mỗi ngày)
};

/// Kết quả Dijkstra từ 1 nguồn ra toàn bản đồ.
struct DijkResult {
    vector<int> dist;  // dist[v] = min steps từ nguồn đến v (INT_MAX nếu không đến được)
    vector<int> fuel;  // fuel[v] = xăng tiêu hao trên đường min-step
    vector<int> prev;  // prev[v] = ô trước đó trên đường ngắn nhất (-1 nếu là nguồn)
};

// ========================================================================
//  §3  TRẠNG THÁI TOÀN CỤC
// ========================================================================

static int W = 0, H = 0;              // Kích thước bản đồ (cột, hàng)
static int g_nAgents   = 0;           // Tổng số tác nhân
static int g_maxFuel   = 0;           // Dung lượng xăng tối đa (xe tuần tra)
static int g_totalDays = 0;           // Số ngày thi đấu

static vector<int>      g_cells;      // Lưới địa hình: 0=đồng bằng 1=đường 2=núi 3=ao
static vector<SpotInfo> g_spots;      // Danh sách điểm udon
static vector<int>      g_daySteps;   // Ngân sách steps từng ngày
static vector<int>      g_assignment; // Vai trò: 0=tuần tra, 1=tiếp tế

/// Theo dõi xuyên suốt trận đấu — KHÔNG reset giữa các ngày.
static set<int> g_collectedFranchises;

// ========================================================================
//  §4  LÕI LƯỚI LỤC GIÁC (even-r offset)
// ========================================================================
//
//  Quy ước hướng (chiều kim đồng hồ):
//    0 = trên-trái    1 = trên-phải   2 = phải
//    3 = dưới-phải    4 = dưới-trái   5 = trái
//
//  Hàng CHẴN (0,2,4…) lệch phải nửa ô so với hàng lẻ.
//  DE = delta cho hàng chẵn, DO = delta cho hàng lẻ.
//  Mỗi phần tử = {Δcol, Δrow}.
// ========================================================================

static const int DE[6][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,0}};
static const int DO[6][2] = {{-1,-1},{0,-1},{1,0},{0,1},{-1,1},{-1,0}};

/// Trả flat index của ô kề theo hướng d, hoặc -1 nếu ngoài bản đồ.
static int neighbor(int pos, int d) {
    if (W <= 0) return -1;
    int r = pos / W, c = pos % W;
    const int (*dl)[2] = (r & 1) ? DO : DE;
    int nc = c + dl[d][0], nr = r + dl[d][1];
    if (nc < 0 || nc >= W || nr < 0 || nr >= H) return -1;
    return nr * W + nc;
}

/// Chi phí rời khỏi ô `pos`. Trả {steps, fuel}. {-1,-1} = ao / không đi được.
/// Quy tắc BTC: chi phí tính theo ÔC NGUỒN (ô đang đứng), không phải ô đích.
static pair<int,int> moveCost(int pos, int trafficStatus) {
    switch (g_cells[pos]) {
        case 0: return {2, 1};                       // Đồng bằng
        case 2: return {3, 2};                       // Núi
        case 1:                                      // Đường bộ
            if (trafficStatus == 1) return {2, 2};   //   đông đúc
            if (trafficStatus == 2) return {4, 2};   //   ùn tắc
            return {1, 2};                           //   thông thoáng
    }
    return {-1, -1};                                 // Ao / không hợp lệ
}

/// Tìm hướng d (0-5) sao cho neighbor(a,d)==b. Trả -1 nếu không kề.
static int dirTo(int a, int b) {
    for (int d = 0; d < 6; d++) if (neighbor(a, d) == b) return d;
    return -1;
}

/// Khoảng cách hex (số hop tối thiểu, bỏ qua địa hình).
/// Chuyển even-r offset → cube coordinates rồi dùng Chebyshev 3D.
static int hexDist(int posA, int posB) {
    int ra = posA / W, ca = posA % W;
    int rb = posB / W, cb = posB % W;
    // even-r to cube:  q = col - (row + (row&1)) / 2,  r = row,  s = -q-r
    int qa = ca - (ra + (ra & 1)) / 2;
    int qb = cb - (rb + (rb & 1)) / 2;
    int sa = -qa - ra, sb = -qb - rb;
    return (abs(qa - qb) + abs(ra - rb) + abs(sa - sb)) / 2;
}

/// Vị trí trung tâm bản đồ (flat index).
static int mapCenter() { return (H / 2) * W + (W / 2); }

// ========================================================================
//  §5  TÌM ĐƯỜNG — Dijkstra có trọng số theo địa hình & giao thông
// ========================================================================
//
//  Thay thế hoàn toàn BFS không trọng số của bot gốc.
//  • Priority queue min-heap theo tổng STEPS (thời gian).
//  • Tiebreak: ít FUEL hơn (tiết kiệm xăng).
//  • isTanker=true → bỏ qua chi phí fuel (xe tiếp tế xăng vô hạn).
//  • Giới hạn: không mở rộng node vượt maxSteps hoặc maxFuel.
//
//  Độ phức tạp: O(N log N) với N = W*H ≤ 1024. Rất nhanh.
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

    // Min-heap: {totalSteps, position}
    priority_queue<pair<int,int>, vector<pair<int,int>>, greater<pair<int,int>>> pq;
    pq.push({0, src});

    while (!pq.empty()) {
        auto top = pq.top(); pq.pop();
        int d = top.first, u = top.second;
        if (d > res.dist[u]) continue;         // entry cũ, bỏ qua

        for (int dir = 0; dir < 6; dir++) {
            int nb = neighbor(u, dir);
            if (nb < 0 || g_cells[nb] == 3) continue;   // ngoài biên hoặc ao

            auto cost = moveCost(u, traffic[u]);         // chi phí RỜI KHỎI u
            int sc = cost.first, fc = cost.second;
            if (sc < 0) continue;                        // u là ao (phòng vệ)

            int nd = d + sc;
            int nf = res.fuel[u] + (isTanker ? 0 : fc);
            if (nd > maxSteps) continue;
            if (!isTanker && nf > maxFuel) continue;

            // Ưu tiên ít steps; cùng steps → ưu tiên ít fuel
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

/// Truy vết đường đi từ src đến dst. Trả vector các ô (KHÔNG gồm src).
static vector<int> reconstructPath(const DijkResult& dijk, int src, int dst) {
    if (dst < 0 || dst >= (int)dijk.dist.size() || dijk.dist[dst] == INT_MAX)
        return {};
    vector<int> path;
    int safetyLimit = (int)dijk.dist.size();   // chống vòng lặp vô hạn
    for (int v = dst; v != src; v = dijk.prev[v]) {
        if (v < 0 || --safetyLimit < 0) return {};
        path.push_back(v);
    }
    reverse(path.begin(), path.end());
    return path;
}

// ========================================================================
//  §6  PHÂN TÍCH BẢN ĐỒ & GÁN VAI TRÒ ĐỘNG
// ========================================================================
//
//  Tỉ lệ tuần tra : tiếp tế KHÔNG cố định. Tính dựa trên:
//    1. Tỉ lệ địa hình khó (núi + ao)  → terrain difficulty
//    2. Chi phí fuel trung bình / bước  → fuel pressure
//    3. Quy mô bản đồ                  → map scale
//
//  Bản đồ khó / lớn → cần nhiều tiếp tế hơn (1:2).
//  Bản đồ dễ / nhỏ → ít tiếp tế hơn (1:3 hoặc 1:4).
//  LUÔN ≥ 1 tiếp tế, LUÔN ≥ 1 tuần tra.
// ========================================================================

/// Phân tích bản đồ, trả patrol-per-tanker ratio (2, 3, hoặc 4).
static int analyzePatrolPerTanker() {
    int total = W * H;
    int mountains = 0, ponds = 0;
    double totalFuelCost = 0.0;
    int walkable = 0;

    for (int i = 0; i < total; i++) {
        if (g_cells[i] == 2) mountains++;
        else if (g_cells[i] == 3) ponds++;
        if (g_cells[i] != 3) {
            auto cost = moveCost(i, 0); // giả sử traffic thông thoáng
            if (cost.first > 0) { totalFuelCost += cost.second; walkable++; }
        }
    }

    double terrainDiff = (double)(mountains + ponds) / max(total, 1);
    double avgFuelCost = walkable > 0 ? totalFuelCost / walkable : 1.0;
    double mapScale    = (double)total / 256.0;   // chuẩn hóa theo bản đồ 16×16

    // Điểm khó tổng hợp — điều chỉnh hệ số nếu cần
    double score = terrainDiff * 2.0 + avgFuelCost * 0.5 + mapScale * 0.2;

    fprintf(stderr, "[MAP] terrain=%.0f%% avgFuel=%.1f scale=%.1f -> score=%.2f\n",
            terrainDiff * 100, avgFuelCost, mapScale, score);

    if (score >= 1.0) return cfg::PPT_HARD;    // 1:2
    if (score <= 0.5) return cfg::PPT_EASY;    // 1:4
    return cfg::PPT_MEDIUM;                    // 1:3
}

/// Tính assignment, trả JSON "[0,0,...,1]" cho POST /assignment.
static string computeAssignment() {
    int ppt = analyzePatrolPerTanker();
    // Số tanker = tổng agent / (1 + patrol_per_tanker), tối thiểu 1
    int nTankers = max(1, g_nAgents / (1 + ppt));
    int nPatrols = g_nAgents - nTankers;

    // Đảm bảo tối thiểu 1 mỗi loại
    if (nPatrols < 1) { nPatrols = 1; nTankers = g_nAgents - 1; }
    if (nTankers < 1) { nTankers = 1; nPatrols = g_nAgents - 1; }

    // Gán: nPatrols xe đầu = tuần tra (0), còn lại = tiếp tế (1)
    g_assignment.resize(g_nAgents);
    for (int i = 0; i < g_nAgents; i++)
        g_assignment[i] = (i < nPatrols) ? 0 : 1;

    fprintf(stderr, "[ASSIGN] %d patrol + %d tanker (1:%d)\n", nPatrols, nTankers, ppt);

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
//  §7  LẬP TUYẾN TUẦN TRA — Greedy đa mục tiêu + ưu tiên đa dạng
// ========================================================================
//
//  Vòng lặp tham lam:
//    1. Dijkstra(vị trí hiện tại) → tính khoảng cách đến MỌI spot
//    2. Chấm điểm tất cả spot khả dụng (diversity + distance + positioning)
//    3. Chọn spot điểm cao nhất → đi theo đường Dijkstra
//    4. Cập nhật vị trí, xăng, franchise đã thu → lặp lại
//    5. Dừng khi hết bước / hết xăng / không spot nào đáng đi
//
//  Khác biệt với bot gốc:
//    • Bot gốc: BFS → 1 spot → đứng yên cả ngày
//    • Bot mới: Dijkstra → N spot → tận dụng toàn bộ daySteps
// ========================================================================

/// Chấm điểm 1 spot để xếp hạng ưu tiên. Score cao = nên ghé trước.
/// Trả -1e18 nếu spot không đến được.
static double scoreSpot(const SpotInfo& spot, const DijkResult& dijk,
                         int stepsUsed, int daySteps, int tankerPos) {
    if (dijk.dist[spot.pos] == INT_MAX) return -1e18;

    double score = 0.0;

    // ── (1) THƯỞNG ĐA DẠNG ──────────────────────────────────────
    // Loại franchise chưa thu → ưu tiên CỰC CAO (tiêu chí #1 BTC).
    if (!g_collectedFranchises.count(spot.franchise))
        score += cfg::W_DIVERSITY;

    // ── (2) PHẠT KHOẢNG CÁCH ────────────────────────────────────
    // Spot gần hơn (ít steps) → điểm cao hơn.
    score -= dijk.dist[spot.pos] * cfg::W_DISTANCE;

    // ── (3) BIAS VỊ TRÍ CUỐI NGÀY ──────────────────────────────
    // Khi đã dùng > 60% steps → bắt đầu kéo về trung tâm + gần tanker.
    // Mục đích: kết thúc ngày ở vị trí tốt cho ngày sau.
    double progress = daySteps > 0 ? (double)stepsUsed / daySteps : 0.0;
    if (progress > cfg::END_DAY_PHASE) {
        double bias = (progress - cfg::END_DAY_PHASE) / (1.0 - cfg::END_DAY_PHASE);
        // Gần trung tâm → linh hoạt cho ngày mai
        score -= hexDist(spot.pos, mapCenter()) * cfg::W_CENTER_BIAS * bias;
        // Gần tanker → nạp xăng dễ hơn
        if (tankerPos >= 0)
            score -= hexDist(spot.pos, tankerPos) * cfg::W_TANKER_PROX * bias;
    }

    return score;
}

/// Lập tuyến đường cho 1 xe tuần tra trong 1 ngày.
///
/// @param pos          Vị trí hiện tại
/// @param fuel         Xăng hiện tại
/// @param daySteps     Ngân sách steps ngày hôm nay
/// @param traffic      Trạng thái giao thông mỗi ô [W*H]
/// @param claimedStock Bản đồ pos→lượt đã claim (chia sẻ giữa các patrol)
/// @param tankerPos    Vị trí tanker gần nhất (-1 nếu không có)
/// @param isLastDay    True nếu là ngày cuối → không giữ xăng dự trữ
///
/// @return  Vector actions: số dương = hướng đi, số âm = đứng yên N steps.
///          Tổng steps trong actions luôn = daySteps (pad bằng wait).
///
/// Side-effects: cập nhật claimedStock và g_collectedFranchises.
static vector<int> planPatrolRoute(int pos, int fuel, int daySteps,
                                    const vector<int>& traffic,
                                    map<int,int>& claimedStock,
                                    int tankerPos, bool isLastDay) {
    vector<int> actions;
    int stepsUsed = 0;
    int curPos    = pos;
    int curFuel   = fuel;

    // ── Tính mức xăng dự trữ ────────────────────────────────────
    // Giữ lại % xăng cho ngày sau, trừ ngày cuối (chi hết).
    // Nếu xăng hiện tại ≤ mức dự trữ → buộc phải chi hết (không đứng chết).
    int fuelReserve = isLastDay ? 0 : (int)(g_maxFuel * cfg::FUEL_RESERVE);
    if (curFuel <= fuelReserve) fuelReserve = 0;

    // ── Vòng lặp tham lam: tìm spot → đi → tìm tiếp ────────────
    while (stepsUsed < daySteps && curFuel > fuelReserve) {
        int stepsLeft  = daySteps - stepsUsed;
        int fuelBudget = curFuel - fuelReserve;
        if (fuelBudget <= 0) break;

        // Dijkstra từ vị trí hiện tại, giới hạn bởi steps & fuel còn lại
        DijkResult dijk = dijkstraAll(curPos, stepsLeft, fuelBudget, traffic, false);

        // Chấm điểm tất cả spot → chọn tốt nhất
        int    bestIdx   = -1;
        double bestScore = -1e18;
        for (int i = 0; i < (int)g_spots.size(); i++) {
            const SpotInfo& sp = g_spots[i];

            // Bỏ qua spot đã hết stock cho đội mình ngày nay
            if (claimedStock[sp.pos] >= sp.maxStock) continue;
            // Bỏ qua spot không đến được
            if (dijk.dist[sp.pos] == INT_MAX) continue;
            // Bỏ qua nếu xăng không đủ
            if (dijk.fuel[sp.pos] > fuelBudget) continue;

            double sc = scoreSpot(sp, dijk, stepsUsed, daySteps, tankerPos);
            if (sc > bestScore) {
                bestScore = sc;
                bestIdx   = i;
            }
        }

        if (bestIdx < 0) break;   // Không spot nào đáng hoặc đến được → dừng

        // ── Đi theo đường Dijkstra đến spot đã chọn ────────────
        vector<int> path = reconstructPath(dijk, curPos, g_spots[bestIdx].pos);
        if (path.empty()) break;

        bool pathOk = true;
        for (int nxt : path) {
            auto cost = moveCost(curPos, traffic[curPos]);
            int sc = cost.first, fc = cost.second;

            // Kiểm tra an toàn (phòng vệ dù Dijkstra đã đảm bảo)
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

        // ── Ghi nhận spot đã ghé ────────────────────────────────
        claimedStock[g_spots[bestIdx].pos]++;
        g_collectedFranchises.insert(g_spots[bestIdx].franchise);
    }

    // ── Đệm wait cho đủ daySteps (bắt buộc theo format BTC) ────
    int rest = daySteps - stepsUsed;
    if (rest > 0) actions.push_back(-rest);

    return actions;
}

// ========================================================================
//  §8  LẬP TUYẾN TIẾP TẾ — Escort / Rendezvous chủ động
// ========================================================================
//
//  Xe tiếp tế KHÔNG đứng yên (khác bot gốc). Chiến lược:
//    1. Tìm xe tuần tra XĂng THẤP NHẤT trong cụm được giao
//    2. Di chuyển về phía vị trí KẾT THÚC dự kiến của xe đó
//       (gặp giữa đường thay vì đuổi theo)
//    3. Nếu tất cả xe tuần tra đủ xăng (>50%) → di chuyển về
//       trọng tâm cụm xe tuần tra (vị trí trung tâm chờ đợi)
//
//  Xe tiếp tế di chuyển KHÔNG tốn xăng → Dijkstra chỉ tối ưu steps.
//  Khi ở cùng ô với xe tuần tra ≥ 1 bước → server tự nạp đầy xăng.
// ========================================================================

/// Lập tuyến cho 1 xe tiếp tế.
///
/// @param tPos       Vị trí tiếp tế hiện tại
/// @param daySteps   Ngân sách steps
/// @param traffic    Vector trạng thái giao thông [W*H]
/// @param patrolInfo Vector {vị_trí_hiện_tại, xăng, vị_trí_cuối_dự_kiến}
///                   của các xe tuần tra trong cụm
///
/// @return  Vector actions (hướng + wait), tổng = daySteps.
static vector<int> planTankerRoute(int tPos, int daySteps,
                                    const vector<int>& traffic,
                                    const vector<tuple<int,int,int>>& patrolInfo) {
    // Không có patrol nào → đứng yên
    if (patrolInfo.empty()) return {-daySteps};

    // ── Tìm patrol KHẨN CẤP NHẤT (xăng thấp nhất) ─────────────
    int targetPos = -1;
    int minFuel   = INT_MAX;
    for (size_t k = 0; k < patrolInfo.size(); k++) {
        int pFuel = get<1>(patrolInfo[k]);
        if (pFuel < minFuel) {
            minFuel   = pFuel;
            targetPos = get<2>(patrolInfo[k]);  // Nhắm đến vị trí KẾT THÚC
        }
    }

    // ── Nếu tất cả patrol đủ xăng → di chuyển về trọng tâm cụm ─
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
        // Dùng centroid nếu ô đó không phải ao
        if (centroid >= 0 && centroid < W * H && g_cells[centroid] != 3)
            targetPos = centroid;
    }

    if (targetPos < 0) return {-daySteps};

    // ── Dijkstra không giới hạn fuel (xe tiếp tế = xăng vô hạn) ─
    DijkResult dijk = dijkstraAll(tPos, daySteps, INT_MAX, traffic, /*isTanker=*/true);
    vector<int> path = reconstructPath(dijk, tPos, targetPos);

    // ── Build actions ───────────────────────────────────────────
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
//  §9  ĐIỀU PHỐI TỔNG HỢP — Kết hợp Patrol + Tanker + Coordination
// ========================================================================
//
//  Luồng xử lý mỗi ngày:
//    1. Parse trạng thái (vị trí, xăng, giao thông)
//    2. Phân cụm: gán tanker[t] → patrol[] gần nhất
//    3. Lập tuyến PATROL trước (xe ít xăng chọn trước → ưu tiên sống còn)
//    4. Mô phỏng vị trí cuối patrol → truyền cho tanker
//    5. Lập tuyến TANKER dựa trên kế hoạch patrol
//    6. Build JSON actions → gửi server
//
//  Đây là HÀM CỐT LÕI — gọi mỗi ngày, quyết định toàn bộ chiến thuật.
// ========================================================================

static string planActions(const mj::Value& m) {
    int day      = m["day"].asInt();
    int daySteps = (day >= 0 && day < (int)g_daySteps.size()) ? g_daySteps[day] : 30;
    bool isLastDay = (day >= g_totalDays - 1);

    // ── Parse trạng thái giao thông ─────────────────────────────
    vector<int> traffic(W * H, 0);
    for (size_t i = 0; i < m["traffics"].size(); i++) {
        int tpos = m["traffics"][i]["pos"].asInt();
        int tst  = m["traffics"][i]["status"].asInt();
        if (tpos >= 0 && tpos < W * H) traffic[tpos] = tst;
    }

    // ── Parse trạng thái agents ─────────────────────────────────
    struct AgentState { int pos, kind, fuel; };
    const mj::Value& ags = m["agents"];
    int nAgs = (int)ags.size();
    vector<AgentState> agents(nAgs);
    for (int i = 0; i < nAgs; i++) {
        agents[i].pos  = ags[i]["pos"].asInt();
        agents[i].kind = ags[i]["kind"].asInt();
        agents[i].fuel = ags[i]["fuel"].isNull() ? (1 << 30) : ags[i]["fuel"].asInt();
    }

    // ── Auto-detect max fuel (lần đầu tiên) ─────────────────────
    if (g_maxFuel == 0) {
        for (int i = 0; i < nAgs; i++)
            if (agents[i].kind == 0)
                g_maxFuel = max(g_maxFuel, agents[i].fuel);
        if (g_maxFuel == 0) g_maxFuel = 100;  // fallback an toàn
        fprintf(stderr, "[FUEL] maxFuel = %d\n", g_maxFuel);
    }

    // ── Phân loại agent ID ──────────────────────────────────────
    vector<int> patrolIds, tankerIds;
    for (int i = 0; i < nAgs; i++) {
        if (agents[i].kind == 0) patrolIds.push_back(i);
        else                     tankerIds.push_back(i);
    }

    // ── Gán cụm: mỗi tanker → danh sách patrol gần nhất ────────
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

    // ── Chuẩn bị kế hoạch cho tất cả agents ────────────────────
    vector<vector<int>> allActions(nAgs);
    vector<int>         endPos(nAgs);
    map<int,int>        claimedStock;  // chia sẻ giữa các patrol (stock coordination)

    // Helper: tìm tanker gần nhất cho 1 patrol
    auto nearestTankerPos = [&](int patrolPos) -> int {
        int best = -1, bestD = INT_MAX;
        for (int ti : tankerIds) {
            int d = hexDist(patrolPos, agents[ti].pos);
            if (d < bestD) { bestD = d; best = agents[ti].pos; }
        }
        return best;
    };

    // ── BƯỚC 1: Lập tuyến PATROL ────────────────────────────────
    // Sắp xếp: xe ÍT XĂNG nhất lập kế hoạch trước (ưu tiên chọn spot gần).
    vector<int> patrolOrder = patrolIds;
    sort(patrolOrder.begin(), patrolOrder.end(), [&](int a, int b) {
        return agents[a].fuel < agents[b].fuel;
    });

    for (int pi : patrolOrder) {
        int tPos = nearestTankerPos(agents[pi].pos);

        allActions[pi] = planPatrolRoute(
            agents[pi].pos, agents[pi].fuel, daySteps, traffic,
            claimedStock, tPos, isLastDay
        );

        // Mô phỏng vị trí cuối ngày (để tanker biết đích đến)
        int cur = agents[pi].pos;
        for (int a : allActions[pi]) {
            if (a >= 0) {
                int nb = neighbor(cur, a);
                if (nb >= 0) cur = nb;
            }
        }
        endPos[pi] = cur;
    }

    // ── BƯỚC 2: Lập tuyến TANKER (dựa vào kế hoạch patrol) ─────
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

    // ── BƯỚC 3: Build JSON output ───────────────────────────────
    ostringstream out;
    out << "[";
    for (int i = 0; i < nAgs; i++) {
        if (i) out << ",";
        out << "[";
        vector<int>& acts = allActions[i];
        if (acts.empty()) {
            // An toàn: nếu không có action nào → đứng yên cả ngày
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

    // ── Logging (debug trong thi đấu) ───────────────────────────
    int totalClaimed = 0;
    for (auto& kv : claimedStock) totalClaimed += kv.second;
    fprintf(stderr, "[DAY %d] steps=%d spots=%d", day, daySteps, totalClaimed);
    for (int pi : patrolIds)
        fprintf(stderr, " P%d@%d(f%d->%d)", pi, agents[pi].pos,
                agents[pi].fuel, endPos[pi]);
    for (int ti : tankerIds)
        fprintf(stderr, " T%d@%d", ti, agents[ti].pos);
    fprintf(stderr, " | fran=%zu\n", g_collectedFranchises.size());

    return out.str();
}

// ========================================================================
//  §10  KHỞI TẠO & VÒNG LẶP CHÍNH
// ========================================================================

/// Parse setup response → khởi tạo globals + tính assignment.
/// Trả JSON body cho POST /assignment.
static string parseSetup(const mj::Value& m) {
    // ── Bản đồ ──────────────────────────────────────────────────
    const mj::Value& mp = m["map"];
    W = mp["width"].asInt();
    H = mp["height"].asInt();
    g_cells.assign((size_t)W * H, 0);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            g_cells[r * W + c] = mp["cells"][r][c].asInt();

    // ── Điểm udon (bao gồm franchise) ──────────────────────────
    g_spots.clear();
    for (size_t i = 0; i < m["spots"].size(); i++) {
        SpotInfo s;
        s.pos = m["spots"][i]["pos"].asInt();

        // Thử nhiều tên trường cho franchise — điều chỉnh nếu API khác
        if      (!m["spots"][i]["franchise"].isNull()) s.franchise = m["spots"][i]["franchise"].asInt();
        else if (!m["spots"][i]["chain"].isNull())     s.franchise = m["spots"][i]["chain"].asInt();
        else if (!m["spots"][i]["type"].isNull())      s.franchise = m["spots"][i]["type"].asInt();
        else s.franchise = (int)i;   // fallback: mỗi spot = 1 loại riêng

        s.maxStock = m["spots"][i]["stock"].isNull() ? 1 : m["spots"][i]["stock"].asInt();
        if (s.maxStock <= 0) s.maxStock = 1;

        g_spots.push_back(s);
    }

    // ── Ngân sách steps mỗi ngày ────────────────────────────────
    g_daySteps.clear();
    for (size_t i = 0; i < m["daySteps"].size(); i++)
        g_daySteps.push_back(m["daySteps"][i].asInt());
    g_totalDays = (int)g_daySteps.size();

    // ── Fuel capacity (nếu setup cung cấp) ──────────────────────
    if (!m["fuelCapacity"].isNull())
        g_maxFuel = m["fuelCapacity"].asInt();
    else
        g_maxFuel = 0;  // sẽ auto-detect từ state ngày đầu

    // ── Agents ──────────────────────────────────────────────────
    g_nAgents = (int)m["agents"].size();

    // ── Reset tracking ──────────────────────────────────────────
    g_collectedFranchises.clear();

    // ── Tính assignment ─────────────────────────────────────────
    return computeAssignment();
}

static void sleepMs(int ms) {
    this_thread::sleep_for(chrono::milliseconds(ms));
}

// ========================================================================
//  MAIN — Vòng lặp 3 giai đoạn: Setup → Assignment → Game Loop
// ========================================================================

int main(int argc, char** argv) {
    // ── Parse command line ──────────────────────────────────────
    string url, matchId, token;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-url" && i + 1 < argc)                          url     = argv[++i];
        else if ((arg == "-match" || arg == "-m") && i + 1 < argc)  matchId = argv[++i];
        else if ((arg == "-token" || arg == "-t") && i + 1 < argc)  token   = argv[++i];
        else if (arg == "-transport" && i + 1 < argc)               i++;  // bỏ qua
        else if (arg[0] != '-') {
            if      (url.empty())     url     = arg;
            else if (matchId.empty()) matchId = arg;
            else if (token.empty())   token   = arg;
        }
    }
    if (url.empty() || matchId.empty() || token.empty()) {
        fprintf(stderr, "Usage: %s <URL> <MATCH_ID> <TOKEN>\n", argv[0]);
        fprintf(stderr, "   or: %s -url <URL> -match <MATCH_ID> -token <TOKEN>\n", argv[0]);
        return 2;
    }
    string base = url + "/api/v1/matches/" + matchId;

    // ════════════════════════════════════════════════════════════
    //  PHASE 1: SETUP — Poll cho tới khi bản đồ mở (425 = chưa tới giờ)
    // ════════════════════════════════════════════════════════════
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

    // In thông tin trận
    set<int> allFranchises;
    for (auto& s : g_spots) allFranchises.insert(s.franchise);
    fprintf(stderr, "=== HEXUDON BOT v1.0 ===\n");
    fprintf(stderr, "[SETUP] Map %dx%d | %zu spots | %zu loai udon | %d agents | %d days\n",
            W, H, g_spots.size(), allFranchises.size(), g_nAgents, g_totalDays);

    // ════════════════════════════════════════════════════════════
    //  PHASE 2: ASSIGNMENT — Gửi vai trò agent (1 lần duy nhất)
    // ════════════════════════════════════════════════════════════
    for (;;) {
        auto r = http::request(base, "POST", "/assignment", token, assignBody);
        if (r.status == 200) break;
        if (r.status == 0 || r.status == 429) { sleepMs(cfg::POLL_MS); continue; }
        fprintf(stderr, "POST /assignment -> HTTP %d\n", r.status);
        return 1;
    }
    fprintf(stderr, "[ASSIGNED] %s\n", assignBody.c_str());

    // ════════════════════════════════════════════════════════════
    //  PHASE 3: GAME LOOP — Poll state + gửi kế hoạch mỗi ngày
    // ════════════════════════════════════════════════════════════
    int lastDay = -1;
    for (;;) {
        auto r = http::request(base, "GET", "/state", token, "");
        if (r.status == 200) {
            auto v = mj::parse(r.body);
            int day = (*v)["day"].asInt();

            if (day != lastDay) {
                // ── Lập kế hoạch & gửi ──────────────────────────
                string acts = planActions(*v);
                auto pr = http::request(base, "POST", "/actions", token, acts);

                if (pr.status == 200) {
                    lastDay = day;
                } else {
                    // Plan bị reject → log lỗi (TODO: rollback g_collectedFranchises)
                    fprintf(stderr, "[ERR] POST /actions day %d -> HTTP %d: %s\n",
                            day, pr.status, pr.body.c_str());
                }
            }
        } else if (r.status != 429 && r.status != 0) {
            // Trận kết thúc hoặc lỗi khác → thử lấy kết quả
            auto rr = http::request(base, "GET", "/result", token, "");
            if (rr.status == 200) {
                fprintf(stderr, "[RESULT] %s\n", rr.body.c_str());
                break;
            }
        }
        sleepMs(cfg::POLL_MS);
    }

    // ── Tổng kết ────────────────────────────────────────────────
    fprintf(stderr, "=== KET THUC ===\n");
    fprintf(stderr, "  Loai udon thu duoc: %zu / %zu\n",
            g_collectedFranchises.size(), allFranchises.size());
    return 0;
}
