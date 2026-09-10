// ws.hpp — Lightweight, High-Performance WebSocket client (RFC 6455).
// Hỗ trợ cả ws:// và wss:// (TLS / HTTPS):
//   - Windows: Dùng WinHttp API native của hệ điều hành (hỗ trợ đầy đủ wss:// TLS & ws://).
//   - Linux/POSIX: Dùng raw socket RFC 6455 (hỗ trợ ws://, chống trễ TCP_NODELAY, ghép frame phân mảnh).
// Không phụ thuộc bất kỳ thư viện ngoài nào ngoài OS API.
#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <winhttp.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "winhttp.lib")
    #pragma comment(lib, "ws2_32.lib")
  #endif
#else
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/tcp.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <arpa/inet.h>
#endif

namespace ws {

// ── Minimal Base64 ───────────────────────────────────────────────────
static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64enc(const unsigned char* d, size_t n) {
    std::string r;
    r.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned t = (unsigned)d[i] << 16;
        if (i + 1 < n) t |= (unsigned)d[i + 1] << 8;
        if (i + 2 < n) t |= d[i + 2];
        r += b64[(t >> 18) & 63];
        r += b64[(t >> 12) & 63];
        r += (i + 1 < n) ? b64[(t >> 6) & 63] : '=';
        r += (i + 2 < n) ? b64[t & 63] : '=';
    }
    return r;
}

#ifdef _WIN32

inline std::wstring toWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

// Định nghĩa kiểu dữ liệu WinHttp WebSocket (dành cho MinGW cũ chưa khai báo sẵn)
#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
  #define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

typedef enum _WINHTTP_WEB_SOCKET_BUFFER_TYPE {
    WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE = 0,
    WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE = 1,
    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE = 2,
    WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE = 3,
    WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE = 4
} WINHTTP_WEB_SOCKET_BUFFER_TYPE;

typedef HINTERNET (WINAPI *pfnWinHttpWebSocketCompleteUpgrade)(HINTERNET, DWORD_PTR);
typedef DWORD (WINAPI *pfnWinHttpWebSocketSend)(HINTERNET, WINHTTP_WEB_SOCKET_BUFFER_TYPE, PVOID, DWORD);
typedef DWORD (WINAPI *pfnWinHttpWebSocketReceive)(HINTERNET, PVOID, DWORD, DWORD*, WINHTTP_WEB_SOCKET_BUFFER_TYPE*);
typedef DWORD (WINAPI *pfnWinHttpWebSocketClose)(HINTERNET, USHORT, PVOID, DWORD);

// ── Client WebSocket chuẩn Windows (WinHttp Native: ws:// & wss:// TLS)
class Client {
    HMODULE   m_hWinHttpDll = NULL;
    pfnWinHttpWebSocketCompleteUpgrade m_fnUpgrade = NULL;
    pfnWinHttpWebSocketSend            m_fnSend    = NULL;
    pfnWinHttpWebSocketReceive         m_fnReceive = NULL;
    pfnWinHttpWebSocketClose           m_fnClose   = NULL;

    HINTERNET m_hSession   = NULL;
    HINTERNET m_hConnect   = NULL;
    HINTERNET m_hWebSocket = NULL;
    bool      m_ok         = false;

    bool loadApis() {
        if (!m_hWinHttpDll) {
            m_hWinHttpDll = LoadLibraryA("winhttp.dll");
            if (!m_hWinHttpDll) return false;
            m_fnUpgrade = (pfnWinHttpWebSocketCompleteUpgrade)GetProcAddress(m_hWinHttpDll, "WinHttpWebSocketCompleteUpgrade");
            m_fnSend    = (pfnWinHttpWebSocketSend)GetProcAddress(m_hWinHttpDll, "WinHttpWebSocketSend");
            m_fnReceive = (pfnWinHttpWebSocketReceive)GetProcAddress(m_hWinHttpDll, "WinHttpWebSocketReceive");
            m_fnClose   = (pfnWinHttpWebSocketClose)GetProcAddress(m_hWinHttpDll, "WinHttpWebSocketClose");
        }
        return (m_fnUpgrade && m_fnSend && m_fnReceive && m_fnClose);
    }

public:
    Client() { loadApis(); }
    ~Client() { close(); }

    bool connect(const std::string& url, const std::vector<std::string>& extraHeaders = {}) {
        close();
        if (!loadApis()) {
            fprintf(stderr, "[WS] winhttp.dll khong ho tro WebSocket API tren may nay!\n");
            return false;
        }

        bool isTls = (url.find("wss://") == 0);
        std::string s = url;
        auto p = s.find("://");
        if (p != std::string::npos) s = s.substr(p + 3);

        std::string host, portStr, path = "/";
        auto sl = s.find('/');
        if (sl != std::string::npos) {
            path = s.substr(sl);
            s = s.substr(0, sl);
        }
        auto co = s.find(':');
        if (co != std::string::npos) {
            host = s.substr(0, co);
            portStr = s.substr(co + 1);
        } else {
            host = s;
            portStr = isTls ? "443" : "80";
        }
        INTERNET_PORT port = (INTERNET_PORT)std::atoi(portStr.c_str());

        m_hSession = WinHttpOpen(L"HexudonBot/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_hSession) {
            fprintf(stderr, "[WS] WinHttpOpen failed (err=%lu)\n", GetLastError());
            return false;
        }

        m_hConnect = WinHttpConnect(m_hSession, toWide(host).c_str(), port, 0);
        if (!m_hConnect) {
            fprintf(stderr, "[WS] WinHttpConnect toi %s:%d that bai (err=%lu)\n",
                    host.c_str(), (int)port, GetLastError());
            close();
            return false;
        }

        DWORD dwFlags = isTls ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(m_hConnect, L"GET", toWide(path).c_str(),
                                               NULL, NULL, NULL, dwFlags);
        if (!hRequest) {
            fprintf(stderr, "[WS] WinHttpOpenRequest that bai (err=%lu)\n", GetLastError());
            close();
            return false;
        }

        // Bật option nâng cấp lên WebSocket
        if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
            fprintf(stderr, "[WS] WinHttpSetOption UPGRADE_TO_WEB_SOCKET that bai (err=%lu)\n", GetLastError());
            WinHttpCloseHandle(hRequest);
            close();
            return false;
        }

        // Bỏ qua chứng chỉ SSL không hợp lệ / tự ký khi chạy TLS
        if (isTls) {
            DWORD dwSecFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                               SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE |
                               SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                               SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecFlags, sizeof(dwSecFlags));
        }

        std::wstring wHeaders;
        for (const auto& h : extraHeaders) {
            wHeaders += toWide(h) + L"\r\n";
        }

        BOOL bSend = WinHttpSendRequest(
            hRequest,
            wHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wHeaders.c_str(),
            (DWORD)wHeaders.length(),
            NULL, 0, 0, 0
        );
        if (!bSend) {
            fprintf(stderr, "[WS] WinHttpSendRequest that bai (err=%lu)\n", GetLastError());
            WinHttpCloseHandle(hRequest);
            close();
            return false;
        }

        if (!WinHttpReceiveResponse(hRequest, NULL)) {
            fprintf(stderr, "[WS] WinHttpReceiveResponse that bai (err=%lu)\n", GetLastError());
            WinHttpCloseHandle(hRequest);
            close();
            return false;
        }

        DWORD dwStatusCode = 0, dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &dwStatusCode, &dwSize, NULL);
        if (dwStatusCode != 101) {
            fprintf(stderr, "[WS] Handshake upgrade that bai — Server tra ve HTTP %lu (can 101)\n", dwStatusCode);
            WinHttpCloseHandle(hRequest);
            close();
            return false;
        }

        m_hWebSocket = m_fnUpgrade(hRequest, 0);
        WinHttpCloseHandle(hRequest); // Handle request đã được upgrade thành công, đóng an toàn
        if (!m_hWebSocket) {
            fprintf(stderr, "[WS] WinHttpWebSocketCompleteUpgrade that bai (err=%lu)\n", GetLastError());
            close();
            return false;
        }

        m_ok = true;
        fprintf(stderr, "[WS] Connected thanh cong (%s) toi %s:%d%s\n",
                isTls ? "WSS / TLS" : "WS", host.c_str(), (int)port, path.c_str());
        return true;
    }

    bool send(const std::string& text) {
        if (!m_ok || !m_hWebSocket) return false;
        DWORD err = m_fnSend(m_hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)text.data(), (DWORD)text.size());
        if (err != ERROR_SUCCESS) {
            m_ok = false;
            return false;
        }
        return true;
    }

    std::string recv(int timeoutMs = -1) {
        if (!m_ok || !m_hWebSocket) return "";

        // Thiết lập timeout nhận nếu có
        DWORD to = (timeoutMs > 0) ? (DWORD)timeoutMs : INFINITE;
        WinHttpSetOption(m_hWebSocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

        std::string message;
        char buf[65536];

        while (m_ok && m_hWebSocket) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;
            DWORD err = m_fnReceive(m_hWebSocket, buf, sizeof(buf), &bytesRead, &bufType);

            if (err == 12002) { // ERROR_WINHTTP_TIMEOUT
                return ""; // Hết thời gian chờ mà chưa có gói tin
            }
            if (err != ERROR_SUCCESS) {
                m_ok = false;
                return "";
            }

            if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                m_ok = false;
                return "";
            }

            if (bytesRead > 0) {
                message.append(buf, bytesRead);
            }

            // Gói tin UTF-8 hoàn chỉnh (hoặc fragment cuối cùng)
            if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                return message;
            }
        }
        return "";
    }

    bool ok() const { return m_ok && (m_hWebSocket != NULL); }

    void close() {
        if (m_hWebSocket) {
            if (m_ok && m_fnClose) {
                m_fnClose(m_hWebSocket, 1000, NULL, 0); // 1000 Normal Close
            }
            WinHttpCloseHandle(m_hWebSocket);
            m_hWebSocket = NULL;
        }
        if (m_hConnect) {
            WinHttpCloseHandle(m_hConnect);
            m_hConnect = NULL;
        }
        if (m_hSession) {
            WinHttpCloseHandle(m_hSession);
            m_hSession = NULL;
        }
        m_ok = false;
    }
};

#else

// ── Client WebSocket Linux / POSIX (Raw socket RFC 6455 cho ws://) ──
using ws_sock_t = int;
#define WS_INVALID_SOCKET (-1)

class Client {
    ws_sock_t m_fd = WS_INVALID_SOCKET;
    bool      m_ok = false;
    std::mt19937 m_rng{std::random_device{}()};

    bool txAll(const void* p, size_t n) {
        if (m_fd == WS_INVALID_SOCKET) return false;
        const char* d = (const char*)p;
        while (n > 0) {
            int s = ::send(m_fd, d, (int)n, 0);
            if (s <= 0) { m_ok = false; return false; }
            d += s; n -= (size_t)s;
        }
        return true;
    }

    bool rxAll(void* p, size_t n) {
        if (m_fd == WS_INVALID_SOCKET) return false;
        char* d = (char*)p;
        while (n > 0) {
            int r = ::recv(m_fd, d, (int)n, 0);
            if (r <= 0) { m_ok = false; return false; }
            d += r; n -= (size_t)r;
        }
        return true;
    }

    bool waitForData(int timeoutMs) {
        if (m_fd == WS_INVALID_SOCKET) return false;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_fd, &fds);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int ret = ::select(m_fd + 1, &fds, NULL, NULL, &tv);
        return (ret > 0);
    }

    bool txFrame(int op, const std::string& pay) {
        if (!m_ok || m_fd == WS_INVALID_SOCKET) return false;
        std::vector<unsigned char> f;
        f.reserve(14 + pay.size());
        f.push_back((unsigned char)(0x80 | (op & 0x0F)));

        size_t L = pay.size();
        if (L < 126) {
            f.push_back((unsigned char)(0x80 | L));
        } else if (L < 65536) {
            f.push_back(0x80 | 126);
            f.push_back((unsigned char)((L >> 8) & 0xFF));
            f.push_back((unsigned char)(L & 0xFF));
        } else {
            f.push_back(0x80 | 127);
            for (int i = 7; i >= 0; i--)
                f.push_back((unsigned char)((L >> (i * 8)) & 0xFF));
        }

        unsigned char mk[4];
        uint32_t mv = m_rng();
        memcpy(mk, &mv, 4);
        for (int i = 0; i < 4; i++) f.push_back(mk[i]);

        size_t cur = f.size();
        f.resize(cur + L);
        for (size_t i = 0; i < L; i++)
            f[cur + i] = (unsigned char)pay[i] ^ mk[i % 4];

        if (!txAll(f.data(), f.size())) { m_ok = false; return false; }
        return true;
    }

    bool rxFrame(bool& fin, int& op, std::string& pay) {
        unsigned char h[2];
        if (!rxAll(h, 2)) return false;

        fin = (h[0] & 0x80) != 0;
        op  = h[0] & 0x0F;
        bool masked = (h[1] & 0x80) != 0;
        uint64_t L = h[1] & 0x7F;

        if (L == 126) {
            unsigned char e[2];
            if (!rxAll(e, 2)) return false;
            L = ((uint64_t)e[0] << 8) | e[1];
        } else if (L == 127) {
            unsigned char e[8];
            if (!rxAll(e, 8)) return false;
            L = 0;
            for (int i = 0; i < 8; i++) L = (L << 8) | e[i];
        }

        if (L > 64 * 1024 * 1024) { m_ok = false; return false; }

        unsigned char mk[4] = {};
        if (masked && !rxAll(mk, 4)) return false;

        pay.resize((size_t)L);
        if (L > 0) {
            if (!rxAll(&pay[0], (size_t)L)) return false;
            if (masked)
                for (size_t i = 0; i < L; i++) pay[i] ^= mk[i % 4];
        }
        return true;
    }

public:
    Client() = default;
    ~Client() { close(); }

    bool connect(const std::string& url, const std::vector<std::string>& extraHeaders = {}) {
        close();
        std::string s = url;
        bool tls = (s.find("wss://") == 0);
        auto p = s.find("://");
        if (p != std::string::npos) s = s.substr(p + 3);
        if (tls) {
            fprintf(stderr, "[WS] Tren Linux wss:// can OpenSSL; vui long dung ws://\n");
            return false;
        }

        std::string host, port = "80", path = "/";
        auto sl = s.find('/');
        if (sl != std::string::npos) { path = s.substr(sl); s = s.substr(0, sl); }
        auto co = s.find(':');
        if (co != std::string::npos) { host = s.substr(0, co); port = s.substr(co + 1); }
        else host = s;

        addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;

        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            m_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (m_fd == WS_INVALID_SOCKET) continue;
            if (::connect(m_fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
            ::close(m_fd);
            m_fd = WS_INVALID_SOCKET;
        }
        freeaddrinfo(res);
        if (m_fd == WS_INVALID_SOCKET) return false;

        int nodelay = 1;
        setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        unsigned char kb[16];
        for (int i = 0; i < 16; i++) kb[i] = (unsigned char)(m_rng() & 0xFF);
        std::string key = base64enc(kb, 16);

        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + port + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n";
        for (const auto& h : extraHeaders) req += h + "\r\n";
        req += "\r\n";

        if (!txAll(req.data(), req.size())) { close(); return false; }

        std::string resp;
        char c;
        while (resp.size() < 8192) {
            if (!rxAll(&c, 1)) { close(); return false; }
            resp += c;
            if (resp.size() >= 4 && resp.substr(resp.size() - 4) == "\r\n\r\n") break;
        }
        if (resp.find("101") == std::string::npos) { close(); return false; }

        m_ok = true;
        return true;
    }

    bool send(const std::string& text) {
        if (!m_ok) return false;
        return txFrame(1, text);
    }

    std::string recv(int timeoutMs = -1) {
        std::string message;
        while (m_ok) {
            if (timeoutMs > 0 && message.empty()) {
                if (!waitForData(timeoutMs)) return "";
            }
            bool fin = false;
            int op = 0;
            std::string pay;
            if (!rxFrame(fin, op, pay)) { m_ok = false; return ""; }
            if (op == 0x8) { m_ok = false; return ""; }
            if (op == 0x9) { txFrame(0xA, pay); continue; }
            if (op == 0xA) continue;

            if (op != 0) message = std::move(pay);
            else message.append(pay);

            if (fin) return message;
        }
        return "";
    }

    bool ok() const { return m_ok; }

    void close() {
        if (m_fd != WS_INVALID_SOCKET) {
            if (m_ok) {
                unsigned char code[2] = { 0x03, (unsigned char)0xE8 };
                txFrame(8, std::string((char*)code, 2));
            }
            m_ok = false;
            shutdown(m_fd, SHUT_RDWR);
            ::close(m_fd);
            m_fd = WS_INVALID_SOCKET;
        }
        m_ok = false;
    }
};

#endif

} // namespace ws
