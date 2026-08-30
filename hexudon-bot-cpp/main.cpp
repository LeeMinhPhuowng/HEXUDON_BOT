// Bot mẫu HEXUDON (format BTC gốc) — C++17, transport HTTP polling. KHÔNG thư viện ngoài (raw socket).
// Chạy:  ./bot <URL> <MATCH_ID> <TOKEN>      (vd ./bot http://judge:8099 m-0001 <token>)
// Build: g++ -std=c++17 -O2 -o bot main.cpp   (Windows nối thêm -lws2_32)
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "minijson.hpp"
#include "http.hpp"
using namespace std;

// Hướng BTC gốc: 0 trên-trái,1 trên-phải,2 phải,3 dưới-phải,4 dưới-trái,5 trái.
// Hình học EVEN-R (hàng CHẴN lệch phải — khớp BTC Q1). DE=hàng chẵn, DO=hàng lẻ.
static const int DE[6][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,0}};
static const int DO[6][2] = {{-1,-1},{0,-1},{1,0},{0,1},{-1,1},{-1,0}};

static int W, H, g_nAgents;
static vector<int> g_cells;    // phẳng row*W+col (0 đất,1 đường,2 núi,3 ao)
static vector<int> g_spots;    // pos các điểm udon
static vector<int> g_daySteps; // số bước mỗi ngày

static int neighbor(int pos, int d) {
    if (W <= 0) return -1;
    int r = pos / W, c = pos % W;
    const int (*dl)[2] = (r % 2) ? DO : DE;
    int nc = c + dl[d][0], nr = r + dl[d][1];
    if (nc < 0 || nc >= W || nr < 0 || nr >= H) return -1;
    return nr * W + nc;
}
static pair<int,int> moveCost(int pos, int status) { // Bảng 1 cố định; {-1,-1}=ao
    switch (g_cells[pos]) {
        case 0: return {2, 1};
        case 2: return {3, 2};
        case 1: return status == 1 ? make_pair(2,2) : status == 2 ? make_pair(4,2) : make_pair(1,2);
    }
    return {-1, -1};
}
static vector<int> bfs(int src, const set<int>& targets) {
    vector<int> prev(g_cells.size(), -2);
    prev[src] = -1;
    deque<int> q{src};
    while (!q.empty()) {
        int c = q.front(); q.pop_front();
        if (c != src && targets.count(c)) {
            vector<int> path;
            for (; c != src; c = prev[c]) path.push_back(c);
            return vector<int>(path.rbegin(), path.rend());
        }
        for (int d = 0; d < 6; d++) {
            int nb = neighbor(c, d);
            if (nb < 0 || prev[nb] != -2 || g_cells[nb] == 3) continue;
            prev[nb] = c; q.push_back(nb);
        }
    }
    return {};
}
static int dirTo(int a, int to) {
    for (int d = 0; d < 6; d++) if (neighbor(a, d) == to) return d;
    return -1;
}

// parseSetup: đọc setup, đặt globals, TRẢ mảng loại agent (phẳng "[0,..,1]") để POST /assignment.
static string parseSetup(const mj::Value& m) {
    const mj::Value& mp = m["map"];
    W = mp["width"].asInt(); H = mp["height"].asInt();
    g_cells.assign((size_t)W * H, 0);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            g_cells[r * W + c] = mp["cells"][r][c].asInt();
    g_spots.clear();
    for (size_t i = 0; i < m["spots"].size(); i++) g_spots.push_back(m["spots"][i]["pos"].asInt());
    g_daySteps.clear();
    for (size_t i = 0; i < m["daySteps"].size(); i++) g_daySteps.push_back(m["daySteps"][i].asInt());
    g_nAgents = (int)m["agents"].size();
    // Gán loại: xe cuối tiếp tế (1), còn lại tuần tra (0).
    ostringstream out; out << "[";
    for (int i = 0; i < g_nAgents; i++) { if (i) out << ","; out << (i == g_nAgents - 1 && g_nAgents > 1 ? 1 : 0); }
    out << "]";
    return out.str();
}

// planActions: từ day_state, sinh kế hoạch "[[..],..]" (xe tuần tra BFS tới spot, đệm đứng yên đủ bước).
static string planActions(const mj::Value& m) {
    int day = m["day"].asInt();
    int steps = (day >= 0 && day < (int)g_daySteps.size()) ? g_daySteps[day] : 30;
    map<int,int> status;
    for (size_t i = 0; i < m["traffics"].size(); i++)
        status[m["traffics"][i]["pos"].asInt()] = m["traffics"][i]["status"].asInt();
    set<int> open(g_spots.begin(), g_spots.end());
    const mj::Value& ags = m["agents"];
    ostringstream out; out << "[";
    for (size_t i = 0; i < ags.size(); i++) {
        if (i) out << ",";
        int kind = ags[i]["kind"].asInt(), pos = ags[i]["pos"].asInt();
        int fuel = ags[i]["fuel"].isNull() ? (1 << 30) : ags[i]["fuel"].asInt();
        vector<int> seq; int used = 0;
        if (kind == 0) { // xe tuần tra: BFS tới spot gần nhất còn kho
            int cur = pos;
            for (int nxt : bfs(pos, open)) {
                auto c = moveCost(cur, status.count(cur) ? status[cur] : 0);
                if (c.first < 0 || used + c.first > steps || fuel < c.second) break;
                int d = dirTo(cur, nxt);
                if (d < 0) break;
                seq.push_back(d); used += c.first; fuel -= c.second; cur = nxt;
            }
        }
        out << "[";
        for (size_t k = 0; k < seq.size(); k++) { if (k) out << ","; out << seq[k]; }
        int rest = steps - used;
        if (rest > 0) { if (!seq.empty()) out << ","; out << -rest; } // đệm đứng yên cho ĐỦ daySteps
        out << "]";
    }
    out << "]";
    return out.str();
}

static void sleepMs(int ms) { this_thread::sleep_for(chrono::milliseconds(ms)); }

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "dung: %s <URL> <MATCH_ID> <TOKEN>
", argv[0]); return 2; }
    string base = string(argv[1]) + "/api/v1/matches/" + argv[2];
    string token = argv[3];
    const int POLL_MS = 250; // >= 200ms tránh 429

    // 1) SETUP — chờ tới khi lấy được (425 = bản đồ chưa mở / trận chưa tới giờ).
    string assignBody;
    for (;;) {
        auto r = http::request(base, "GET", "/setup", token, "");
        if (r.status == 200) { auto v = mj::parse(r.body); assignBody = parseSetup(*v); break; }
        if (r.status == 0 || r.status == 425 || r.status == 429) { sleepMs(POLL_MS); continue; }
        fprintf(stderr, "GET /setup -> HTTP %d (token sai / match khong hop le?)
", r.status); return 1;
    }
    fprintf(stderr, "nhan setup: %dx%d o, %zu diem, %d xe
", W, H, g_spots.size(), g_nAgents);

    // 2) ASSIGNMENT — gửi loại agent (cố định cả trận).
    for (;;) {
        auto r = http::request(base, "POST", "/assignment", token, assignBody);
        if (r.status == 200) break;
        if (r.status == 0 || r.status == 429) { sleepMs(POLL_MS); continue; }
        fprintf(stderr, "POST /assignment -> HTTP %d
", r.status); return 1;
    }

    // 3) VÒNG NGÀY — poll /state; mỗi ngày mới gửi kế hoạch. Hết trận -> /result trả 200.
    int lastDay = -1;
    for (;;) {
        auto r = http::request(base, "GET", "/state", token, "");
        if (r.status == 200) {
            auto v = mj::parse(r.body);
            int day = (*v)["day"].asInt();
            if (day != lastDay) {
                string acts = planActions(*v);
                auto pr = http::request(base, "POST", "/actions", token, acts);
                if (pr.status == 200) { lastDay = day; fprintf(stderr, "ngay %d: da gui ke hoach
", day); }
                // 429/lỗi: giữ lastDay để vòng sau gửi lại.
            }
        } else if (r.status != 429 && r.status != 0) {
            auto rr = http::request(base, "GET", "/result", token, "");
            if (rr.status == 200) { fprintf(stderr, "ket thuc tran
"); break; }
        }
        sleepMs(POLL_MS);
    }
    return 0;
}
