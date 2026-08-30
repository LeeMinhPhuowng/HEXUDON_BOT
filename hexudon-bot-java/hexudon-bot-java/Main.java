// Bot mẫu HEXUDON (format BTC gốc) — Java 11+, HTTP polling, chỉ dùng JDK.
// Build & chạy:  javac Main.java MiniJson.java && java Main <URL> <MATCH> <TOKEN>
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class Main {
    // Hướng BTC gốc: 0 trên-trái,1 trên-phải,2 phải,3 dưới-phải,4 dưới-trái,5 trái.
    // Hình học EVEN-R (hàng CHẴN lệch phải — khớp BTC Q1). DE=hàng chẵn, DO=hàng lẻ.
    static final int[][] DE = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,0}};
    static final int[][] DO = {{-1,-1},{0,-1},{1,0},{0,1},{-1,1},{-1,0}};

    static int W, H;
    static int[] cells;            // phẳng row*W+col (0 đất,1 đường,2 núi,3 ao)
    static Set<Integer> spots = new HashSet<>();
    static List<Integer> daySteps = new ArrayList<>();

    static int neighbor(int pos, int d) {
        int r = pos / W, c = pos % W;
        int[][] dl = (r % 2 == 1) ? DO : DE;
        int nc = c + dl[d][0], nr = r + dl[d][1];
        if (nc < 0 || nc >= W || nr < 0 || nr >= H) return -1;
        return nr * W + nc;
    }
    // moveCost Bảng 1 cố định: [bước, nhiên liệu]; null nếu ao.
    static int[] moveCost(int pos, int status) {
        switch (cells[pos]) {
            case 0: return new int[]{2, 1};
            case 2: return new int[]{3, 2};
            case 1: return status == 1 ? new int[]{2,2} : status == 2 ? new int[]{4,2} : new int[]{1,2};
        }
        return null;
    }
    static List<Integer> bfs(int src, Set<Integer> targets) {
        int[] prev = new int[cells.length];
        java.util.Arrays.fill(prev, -2);
        prev[src] = -1;
        Deque<Integer> q = new ArrayDeque<>();
        q.add(src);
        while (!q.isEmpty()) {
            int c = q.poll();
            if (c != src && targets.contains(c)) {
                List<Integer> path = new ArrayList<>();
                for (int x = c; x != src; x = prev[x]) path.add(0, x);
                return path;
            }
            for (int d = 0; d < 6; d++) {
                int nb = neighbor(c, d);
                if (nb < 0 || prev[nb] != -2 || cells[nb] == 3) continue;
                prev[nb] = c; q.add(nb);
            }
        }
        return new ArrayList<>();
    }
    static int dirTo(int a, int to) {
        for (int d = 0; d < 6; d++) if (neighbor(a, d) == to) return d;
        return -1;
    }

    static HttpClient http = HttpClient.newHttpClient();
    static String api, token;

    static HttpResponse<String> call(String method, String path, String body) throws Exception {
        HttpRequest.Builder b = HttpRequest.newBuilder(URI.create(api + path))
            .header("Authorization", "Bearer " + token);
        if (body != null) b.header("Content-Type", "application/json").method(method, HttpRequest.BodyPublishers.ofString(body));
        else b.method(method, HttpRequest.BodyPublishers.noBody());
        return http.send(b.build(), HttpResponse.BodyHandlers.ofString());
    }

    static String planDay(Object st) {
        int day = MiniJson.asInt(MiniJson.get(st, "day"));
        int steps = (day >= 0 && day < daySteps.size()) ? daySteps.get(day) : 30;
        Map<Integer,Integer> status = new HashMap<>();
        for (Object t : MiniJson.arr(MiniJson.get(st, "traffics")))
            status.put(MiniJson.asInt(MiniJson.get(t, "pos")), MiniJson.asInt(MiniJson.get(t, "status")));
        StringBuilder out = new StringBuilder("[");
        List<Object> ags = MiniJson.arr(MiniJson.get(st, "agents"));
        for (int i = 0; i < ags.size(); i++) {
            if (i > 0) out.append(",");
            Object a = ags.get(i);
            int kind = MiniJson.asInt(MiniJson.get(a, "kind")), pos = MiniJson.asInt(MiniJson.get(a, "pos"));
            Object f = MiniJson.get(a, "fuel");
            int fuel = (f == null) ? Integer.MAX_VALUE : MiniJson.asInt(f);
            List<Integer> seq = new ArrayList<>();
            int used = 0;
            if (kind == 0) {                       // xe tuần tra: BFS tới spot
                int cur = pos;
                for (int nxt : bfs(pos, spots)) {
                    int[] c = moveCost(cur, status.getOrDefault(cur, 0));
                    if (c == null || used + c[0] > steps || fuel < c[1]) break;
                    int d = dirTo(cur, nxt);
                    if (d < 0) break;
                    seq.add(d); used += c[0]; fuel -= c[1]; cur = nxt;
                }
            }
            out.append("[");
            for (int k = 0; k < seq.size(); k++) { if (k > 0) out.append(","); out.append(seq.get(k)); }
            int rest = steps - used;
            if (rest > 0) { if (!seq.isEmpty()) out.append(","); out.append(-rest); } // đệm đủ daySteps
            out.append("]");
        }
        return out.append("]").toString();
    }

    public static void main(String[] args) throws Exception {
        api = args[0] + "/api/v1/matches/" + args[1];
        token = args[2];
        // 1) Setup (425 = chưa mở bản đồ -> thử lại)
        Object setup;
        while (true) {
            HttpResponse<String> r = call("GET", "/setup", null);
            if (r.statusCode() == 200) { setup = MiniJson.parse(r.body()); break; }
            Thread.sleep(250);
        }
        Object map = MiniJson.get(setup, "map");
        W = MiniJson.asInt(MiniJson.get(map, "width"));
        H = MiniJson.asInt(MiniJson.get(map, "height"));
        cells = new int[W * H];
        List<Object> rows = MiniJson.arr(MiniJson.get(map, "cells"));
        for (int r = 0; r < H; r++) {
            List<Object> row = MiniJson.arr(rows.get(r));
            for (int c = 0; c < W; c++) cells[r * W + c] = MiniJson.asInt(row.get(c));
        }
        for (Object sp : MiniJson.arr(MiniJson.get(setup, "spots"))) spots.add(MiniJson.asInt(MiniJson.get(sp, "pos")));
        for (Object ds : MiniJson.arr(MiniJson.get(setup, "daySteps"))) daySteps.add(MiniJson.asInt(ds));
        int nAgents = MiniJson.arr(MiniJson.get(setup, "agents")).size();

        // 2) Gán loại: xe cuối tiếp tế (1), còn lại tuần tra (0) -> [0,..,1]
        StringBuilder as = new StringBuilder("[");
        for (int i = 0; i < nAgents; i++) { if (i > 0) as.append(","); as.append(i == nAgents - 1 && nAgents > 1 ? 1 : 0); }
        as.append("]");
        while (call("POST", "/assignment", as.toString()).statusCode() != 200) Thread.sleep(250);

        // 3) Vòng ngày: mỗi ngày mới gửi kế hoạch [[..],..]; thoát khi có kết quả
        int lastDay = -1;
        while (true) {
            HttpResponse<String> r = call("GET", "/state", null);
            if (r.statusCode() == 200) {
                Object st = MiniJson.parse(r.body());
                int day = MiniJson.asInt(MiniJson.get(st, "day"));
                if (day != lastDay) {
                    if (call("POST", "/actions", planDay(st)).statusCode() == 200) lastDay = day;
                }
            } else {
                HttpResponse<String> res = call("GET", "/result", null);
                if (res.statusCode() == 200) { System.out.println("Ket qua: " + res.body()); return; }
            }
            Thread.sleep(250);
        }
    }
}