// http.hpp — HTTP client tối giản (raw socket, KHÔNG thư viện ngoài). Đủ cho bot HEXUDON.
// Cross-platform: POSIX (Linux/macOS) + Winsock (Windows). Mỗi request 1 kết nối (Connection: close).
#pragma once
#include <string>
#include <cstdlib>
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  static void sock_close(sock_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  using sock_t = int;
  static const sock_t INVALID_SOCKET = -1;
  static void sock_close(sock_t s) { close(s); }
#endif
namespace http {
struct Response { int status = 0; std::string body; };

// Tách "http://host:port/duong-dan" -> host, port (mặc định 80), basePath (phần đường dẫn, có thể rỗng).
inline void parseBase(const std::string& url, std::string& host, std::string& port, std::string& basePath) {
    std::string s = url;
    auto p = s.find("://"); if (p != std::string::npos) s = s.substr(p + 3);
    auto slash = s.find('/');
    if (slash != std::string::npos) { basePath = s.substr(slash); s = s.substr(0, slash); }
    else basePath.clear();
    auto colon = s.find(':');
    if (colon != std::string::npos) { host = s.substr(0, colon); port = s.substr(colon + 1); }
    else { host = s; port = "80"; }
}

// method GET/POST · path (đầy đủ) · token Bearer · body (POST). Trả Response (status=0 nếu lỗi mạng).
inline Response request(const std::string& base, const std::string& method,
                        const std::string& path, const std::string& token,
                        const std::string& body) {
    std::string host, port, basePath; parseBase(base, host, port, basePath);
#ifdef _WIN32
    static bool inited = false;
    if (!inited) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); inited = true; }
#endif
    Response r;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return r;
    // Thử TỪNG địa chỉ: getaddrinfo có thể trả IPv6 (::1) trước IPv4 (127.0.0.1); server có thể chỉ
    // nghe một họ -> phải duyệt tới khi connect được, không chỉ lấy ô đầu.
    sock_t fd = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break; // nối được
        sock_close(fd); fd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) return r; // không địa chỉ nào nối được

    // HTTP/1.0 + Connection: close: server trả body rồi ĐÓNG (không chunked của HTTP/1.1) -> recv
    // tới khi đóng là đọc trọn body sạch. Tránh phải tự giải mã transfer-encoding: chunked.
    std::string req = method + " " + basePath + path + " HTTP/1.0\r\n";
    req += "Host: " + host + "\r\n";
    req += "Authorization: Bearer " + token + "\r\n";
    req += "Connection: close\r\n";
    if (method == "POST") {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    if (method == "POST") req += body;

    // Gửi hết (loop phòng send từng phần).
    size_t sent = 0;
    while (sent < req.size()) {
        int n = (int)send(fd, req.data() + sent, (int)(req.size() - sent), 0);
        if (n <= 0) { sock_close(fd); return r; }
        sent += n;
    }
    // Đọc tới khi server đóng (Connection: close).
    std::string raw; char buf[4096]; int n;
    while ((n = (int)recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
    sock_close(fd);

    // Status: "HTTP/1.1 <code> ...".
    auto sp = raw.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(raw.c_str() + sp + 1);
    // Body sau header (bỏ qua chunked — server trả Content-Length cho các phản hồi nhỏ này).
    auto hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd != std::string::npos) r.body = raw.substr(hdrEnd + 4);
    return r;
}
} // namespace http
