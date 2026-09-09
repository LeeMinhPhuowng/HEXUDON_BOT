# Tài liệu Kỹ thuật & API — PTIT PROCON 2026

> **Đơn vị tổ chức:** PTIT PROCON 2026 – Thi đấu Procon PTIT.
> **Base URL:** [https://procon.ptit.edu.vn](https://procon.ptit.edu.vn)
> **Xác thực:** Mỗi đội dùng Token (lấy tại trang "Trận của tôi"). Header yêu cầu: `Authorization: Bearer <token>`.

---

## 1. Hai phương thức kết nối

Hệ thống hỗ trợ 2 phương thức kết nối:

| Phương thức | Endpoint / URL | Mô tả đặc điểm |
| :--- | :--- | :--- |
| **HTTP (Polling)** | `https://procon.ptit.edu.vn/api/v1/matches/{id}/...` | Đơn giản nhất: Định kỳ poll state và gửi actions. Header bắt buộc: `Authorization: Bearer <token>`. |
| **WebSocket** | `ws://.../ws/v1/matches/{id}?token=...` | Server tự động đẩy state; bot gửi action trên cùng kết nối. |

### WebSocket — Thứ tự khung dữ liệu

WebSocket đẩy các khung theo thứ tự:

1. **`setup`** — Cấu hình trận đấu.
2. Bot gửi **loại agent** (assignment).
3. Mỗi ngày server đẩy **`day_state`**.
4. Bot gửi **kế hoạch hành động**.
5. Server trả **kết quả**.

Mỗi khung là 1 message JSON **không có trường `type`**, phân biệt dựa theo sự xuất hiện của các trường dữ liệu.

---

## 2. Các lưu ý quan trọng về hệ thống & bản đồ

### 2.1. Hệ tọa độ & Bản đồ lục giác

- Vị trí trên bản đồ được biểu diễn dưới dạng **chỉ số ô phẳng**:

  ```
  pos = row × width + col
  ```

- `map.cells` là mảng 2 chiều chứa các số nguyên đại diện cho địa hình:

  | Giá trị | Địa hình |
  | :---: | :--- |
  | `0` | Đất (Đồng bằng) |
  | `1` | Đường |
  | `2` | Núi |
  | `3` | Ao (không thể đi vào) |

- **Không có sẵn danh sách ô kề:** Bot phải tự tính toán theo hình học lục giác dạng **even-r** (hàng chẵn lệch phải).

### 2.2. Vector dịch chuyển (Δcol, Δrow) theo 6 hướng

| Hàng | Hướng 0 | Hướng 1 | Hướng 2 | Hướng 3 | Hướng 4 | Hướng 5 |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Chẵn** (`DIRS_EVEN`) | (0, −1) | (1, −1) | (1, 0) | (1, 1) | (0, 1) | (−1, 0) |
| **Lẻ** (`DIRS_ODD`) | (−1, −1) | (0, −1) | (1, 0) | (0, 1) | (−1, 1) | (−1, 0) |

### 2.3. Quy ước & lưu ý khác

- **Quy ước ngày:** Ngày đầu tiên bắt đầu từ số **0** (không phải ngày 1).
- **Pha chuẩn bị / Đặt lịch:** Khi bản đồ hoặc trận đấu chưa mở, các endpoint `/setup`, `/state`, `/assignment` trả về mã **HTTP 425** (→ "chưa tới giờ"), bot tiếp tục thử lại định kỳ.
- **Nhiên liệu đối thủ:** Dữ liệu nhiên liệu đối thủ trong trường `others` có thể xuất hiện (tùy thuộc vào cấu hình của BTC). Khi lập kế hoạch, bot chỉ nên dựa vào lượng nhiên liệu của **đội mình**.
- **Giới hạn tần suất gọi (Rate-limit):**
  - Tần suất poll phải **≥ 200ms**. Nếu gửi quá nhanh sẽ nhận mã lỗi **HTTP 429** (`E_RATE_LIMIT`).
  - Kế hoạch hành động gửi lên có thể **gửi lại/gửi đè** trước khi chốt ngày.

---

## 3. Luồng chơi (Game Flow)

### Bước 1 — Nhận setup

Dữ liệu nhận được bao gồm:

| Trường | Mô tả |
| :--- | :--- |
| `daySteps[]` | Mảng chứa số bước tối đa cho mỗi ngày (độ dài mảng = tổng số ngày thi đấu). |
| `map.cells` | Mảng 2 chiều biểu diễn bản đồ địa hình. |
| `spots` | Danh sách các điểm bán udon: `brand` (loại/nhãn hiệu), `pos` (vị trí ô phẳng), `stocks` (số lượng tồn kho). |
| `agents` | Danh sách vị trí xuất phát của các agent đội mình. |
| `fuelLimits` | Giới hạn nhiên liệu. |

### Bước 2 — Gán loại Agent (Gửi assignment)

- Gửi mảng phẳng gồm các số nguyên: `0` (Agent tuần tra – thu thập udon, tiêu tốn nhiên liệu) hoặc `1` (Agent tiếp tế).
- Độ dài mảng bằng đúng số lượng agent.
- **Cố định suốt cả trận đấu** (không được thay đổi sau khi đã gán).

### Bước 3 — Diễn biến từng ngày

1. Nhận dữ liệu **`day_state`** của ngày hôm đó:

   | Trường | Mô tả |
   | :--- | :--- |
   | `day` | Số ngày hiện tại (bắt đầu từ 0). |
   | `agents[]` | Mảng agent chứa: `kind`, `pos`, `fuel`. |
   | `others` | Thông tin đối thủ (có thể có hoặc không). |
   | `traffics[]` | Danh sách trạng thái giao thông: `pos`, `status`. |

2. Tính toán và gửi **kế hoạch di chuyển** cho từng agent trong phạm vi ngân sách bước (`daySteps`) của ngày đó.

---

## 4. Danh sách các Endpoint HTTP

| Thao tác | Phương thức & Endpoint | Mô tả |
| :--- | :--- | :--- |
| Lấy cấu hình | `GET /api/v1/matches/{id}/setup` | Lấy dữ liệu khởi tạo trận đấu (bản đồ, danh sách agent, spots,...). |
| Gán loại tác nhân | `POST /api/v1/matches/{id}/assignment` | Gửi mảng phân loại agent (`0` hoặc `1`). |
| Chờ bắt đầu | `GET /api/v1/matches/{id}/start` | Kiểm tra trận đã bắt đầu chưa (trả về `425` nếu chưa đến giờ). |
| Lấy state ngày | `GET /api/v1/matches/{id}/state` | Nhận trạng thái chi tiết của ngày hiện tại. |
| Gửi hành động | `POST /api/v1/matches/{id}/actions` | Gửi mảng kế hoạch hành động trong ngày (cho phép gửi đè/gửi lại). |
| Kết quả | `GET /api/v1/matches/{id}/result` | Lấy kết quả chung cuộc của trận đấu. |

> **Lưu ý:** Thời gian giữa các lần poll phải **≥ 200ms**; nếu gọi dồn dập sẽ bị chặn với mã HTTP `429`.

---

## 5. Định dạng Kế hoạch & Quy định Di chuyển

### 5.1. Định dạng loại Agent (Assignment)

Mảng 1 chiều có độ dài bằng số agent, nhận giá trị `0` (tuần tra) hoặc `1` (tiếp tế):

```json
[0, 1, 0, 1]
```

### 5.2. Định dạng kế hoạch ngày (Actions)

Là một **mảng-của-mảng**, mỗi phần tử đại diện cho chuỗi hành động của một agent:

- Các số từ `0` đến `5`: **Hướng di chuyển**.
- Số `-1`: **Đứng yên** 1 bước. Khi viết `-N` (ví dụ `-15`) nghĩa là đứng yên N bước.

**Quy định số bước:** Tổng số bước đi của mỗi agent không được vượt quá số bước tối đa của ngày (`daySteps`). Nếu thiếu bước, phần còn lại sẽ tự động được tính là đứng yên (khuyên dùng: nên đệm các bước `-1` cho đủ tổng số bước).

**Ví dụ:**

```json
[
  [-15],
  [0, 1, -10]
]
```

- Agent 0: đứng yên 15 bước.
- Agent 1: đi theo hướng 0, tiếp tục hướng 1, sau đó đứng yên 10 bước.

### 5.3. Quy ước 6 hướng di chuyển

Theo chiều kim đồng hồ, bắt đầu từ trên-trái:

| Hướng | Tên |
| :---: | :--- |
| `0` | Trên – Trái |
| `1` | Trên – Phải |
| `2` | Phải |
| `3` | Dưới – Phải |
| `4` | Dưới – Trái |
| `5` | Trái |

### 5.4. Chi phí bước đi & Tiêu hao nhiên liệu

Chi phí được tính căn cứ theo loại địa hình của **ô xuất phát (ô nguồn)**:

| Địa hình / Trạng thái | Số bước | Nhiên liệu (xe tuần tra) |
| :--- | :---: | :---: |
| 🟩 Đất (Đồng bằng) — `0` | 2 | 1 |
| 🟧 Núi — `2` | 3 | 2 |
| 🟦 Ao — `3` | ❌ Không đi được | — |
| ⬜ Đường (Thông thoáng) — `1` | 1 | 2 |
| ⬜ Đường (Đông đúc) — `1` | 2 | 2 |
| ⬜ Đường (Ùn tắc) — `1` | 4 | 2 |

### 5.5. Cảnh báo tính hợp lệ

- ⚠️ Chỉ cần **một agent** vi phạm quy tắc, **toàn bộ kế hoạch** của lượt đó sẽ bị server từ chối.
- ⚠️ Nếu hết thời gian quy định của ngày mà không có kế hoạch hợp lệ nào được chấp nhận, **tất cả agent** sẽ bị xử phạt đứng yên toàn bộ ngày hôm đó.

---

## 6. Danh mục mã lỗi

Trả về trong `action_result.reason`:

| Mã lỗi | Mô tả |
| :--- | :--- |
| `E_NOT_ADJACENT` | Di chuyển sang ô không kề cạnh ô hiện tại. |
| `E_POND` | Cố gắng di chuyển vào ô Ao. |
| `E_STEP_OVERFLOW` | Tổng số bước vượt quá hạn mức `daySteps` trong ngày. |
| `E_NO_FUEL` | Hết nhiên liệu / không đủ nhiên liệu để thực hiện bước đi. |
| `E_BAD_FORMAT` | Gửi sai định dạng JSON hoặc cấu trúc mảng. |
| `E_RATE_LIMIT` | Gửi yêu cầu quá dày đặc (vi phạm ngưỡng 200ms). |

---

## 7. Lời khuyên & Mẹo chiến thuật

- **Phương thức giao tiếp:** Giai đoạn đầu nên lập trình bằng giao thức **HTTP Polling** để kiểm thử nhanh tính đúng đắn; sau khi bot ổn định có thể nâng cấp sang **WebSocket** để giảm độ trễ.
- **Quản lý tài nguyên:** Cần tính toán cẩn thận để kế hoạch luôn nằm trong ngân sách bước và nhiên liệu cho phép; phối hợp nhịp nhàng giữa xe tuần tra và xe tiếp tế.
- **Chiến lược ăn điểm:** Ưu tiên thu thập **đa dạng các loại udon** (đây là tiêu chí phân định thắng thua số 1) thay vì cố gom số lượng nhiều của cùng một loại udon.
- **Luyện tập:** Tận dụng tính năng đấu tập với bot máy tại mục "Trận của tôi" trên hệ thống web (có thể tùy chọn các mức độ khó) để rà soát toàn bộ lỗi trước khi bước vào trận đấu chính thức.

---

## Tham khảo nhanh — Ví dụ HTTP Polling Flow

```
1. GET  /api/v1/matches/{id}/setup          → Nhận cấu hình trận
2. POST /api/v1/matches/{id}/assignment      → Gửi [0, 1, 0, 1]
3. GET  /api/v1/matches/{id}/start           → Poll cho đến khi không còn 425
4. GET  /api/v1/matches/{id}/state           → Nhận day_state (day=0)
5. POST /api/v1/matches/{id}/actions         → Gửi kế hoạch ngày 0
   ... lặp lại bước 4-5 cho các ngày tiếp theo ...
6. GET  /api/v1/matches/{id}/result          → Nhận kết quả chung cuộc
```

> **Nhớ:** Poll mỗi 200ms trở lên, luôn kèm header `Authorization: Bearer <token>`.
