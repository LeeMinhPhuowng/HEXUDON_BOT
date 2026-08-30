# Bot mẫu HEXUDON — Java 11+ (HTTP polling)

Chỉ dùng JDK (java.net.http), không thư viện ngoài.

## Build & chạy
    javac Main.java MiniJson.java
    java Main <BASE_URL> <MATCH_ID> <TOKEN>

Ví dụ:
    java Main http://<judge-host>:8099 m-0001 m-0001-a-xxxxxxxx

## Cấu trúc
- Main.java     — logic bot: lấy setup, gán loại, mỗi ngày BFS + gửi kế hoạch
- MiniJson.java — JSON parser tối giản

Chiến thuật mẫu: xe tuần tra đi tới điểm còn kho gần nhất (BFS);
xe tiếp nhiên liệu bám xe tuần tra ít nhiên liệu nhất.
Hãy thay planDay(...) bằng chiến thuật của đội bạn!
