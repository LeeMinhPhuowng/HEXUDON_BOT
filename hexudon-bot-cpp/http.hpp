// http.hpp — Resilient HTTP/HTTPS client với Persistent Connection (Keep-Alive) & Fail-Fast Timeouts.
#pragma once
#include <string>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
  #include <winhttp.h>
  #pragma comment(lib, "winhttp.lib")
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

inline void parseBase(const std::string& url, bool& isHttps, std::string& host, std::string& port, std::string& basePath) {
    std::string s = url;
    isHttps = (s.rfind("https://", 0) == 0);
    auto p = s.find("://");
    if (p != std::string::npos) s = s.substr(p + 3);
    auto slash = s.find('/');
    if (slash != std::string::npos) { basePath = s.substr(slash); s = s.substr(0, slash); }
    else basePath.clear();
    auto colon = s.find(':');
    if (colon != std::string::npos) { host = s.substr(0, colon); port = s.substr(colon + 1); }
    else { host = s; port = isHttps ? "443" : "80"; }
}

#ifdef _WIN32
inline std::wstring toWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

class Client {
private:
    std::string m_base;
    std::string m_token;
    bool        m_isHttps;
    std::string m_host, m_port, m_basePath;
    HINTERNET   m_hSession;
    HINTERNET   m_hConnect;

    void cleanupConnect() {
        if (m_hConnect) {
            WinHttpCloseHandle(m_hConnect);
            m_hConnect = NULL;
        }
    }

    bool ensureConnect() {
        if (m_hConnect) return true;
        if (!m_hSession) {
            m_hSession = WinHttpOpen(L"HexudonBot/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!m_hSession) return false;
            // FAIL-FAST TIMEOUTS: 1200ms connect, 1500ms send, 1500ms receive (thay vì 30-60s mặc định của Windows)
            WinHttpSetTimeouts(m_hSession, 1200, 1200, 1500, 1500);
        }
        INTERNET_PORT nPort = (INTERNET_PORT)std::atoi(m_port.c_str());
        m_hConnect = WinHttpConnect(m_hSession, toWide(m_host).c_str(), nPort, 0);
        return (m_hConnect != NULL);
    }

public:
    Client() : m_isHttps(false), m_hSession(NULL), m_hConnect(NULL) {}

    Client(const std::string& base, const std::string& token = "")
        : m_base(base), m_token(token), m_isHttps(false), m_hSession(NULL), m_hConnect(NULL) {
        init(base, token);
    }

    ~Client() {
        close();
    }

    void close() {
        cleanupConnect();
        if (m_hSession) {
            WinHttpCloseHandle(m_hSession);
            m_hSession = NULL;
        }
    }

    void init(const std::string& base, const std::string& token = "") {
        close();
        m_base = base;
        m_token = token;
        parseBase(m_base, m_isHttps, m_host, m_port, m_basePath);
        ensureConnect();
    }

    void setToken(const std::string& token) {
        m_token = token;
    }

    Response request(const std::string& method, const std::string& path,
                     const std::string& body = "", int timeoutMs = 1500) {
        Response r;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (!ensureConnect()) {
                cleanupConnect();
                continue;
            }

            std::string fullPath = m_basePath + path;
            DWORD dwFlags = m_isHttps ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET hRequest = WinHttpOpenRequest(m_hConnect, toWide(method).c_str(), toWide(fullPath).c_str(), NULL, NULL, NULL, dwFlags);
            if (!hRequest) {
                cleanupConnect();
                continue;
            }

            // Fail-Fast Per-Request Timeouts
            WinHttpSetTimeouts(hRequest, 1200, 1200, timeoutMs, timeoutMs);

            if (m_isHttps) {
                DWORD dwSecFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE |
                                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
                WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecFlags, sizeof(dwSecFlags));
            }

            std::string headers = "Connection: Keep-Alive\r\n";
            if (!m_token.empty()) {
                headers += "Authorization: Bearer " + m_token + "\r\n";
            }
            if (method == "POST") {
                headers += "Content-Type: application/json\r\n";
            }

            std::wstring wHeaders = toWide(headers);
            LPVOID pOptional = (method == "POST" && !body.empty()) ? (LPVOID)body.c_str() : WINHTTP_NO_REQUEST_DATA;
            DWORD dwOptionalLen = (method == "POST" && !body.empty()) ? (DWORD)body.size() : 0;

            BOOL bResults = WinHttpSendRequest(hRequest, wHeaders.c_str(), (DWORD)wHeaders.length(), pOptional, dwOptionalLen, dwOptionalLen, 0);
            if (bResults) bResults = WinHttpReceiveResponse(hRequest, NULL);

            if (bResults) {
                DWORD dwStatusCode = 0;
                DWORD dwSize = sizeof(dwStatusCode);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
                r.status = (int)dwStatusCode;

                DWORD dwDownloaded = 0;
                do {
                    dwSize = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                    if (dwSize == 0) break;
                    std::vector<char> buf(dwSize + 1);
                    if (WinHttpReadData(hRequest, &buf[0], dwSize, &dwDownloaded)) {
                        r.body.append(&buf[0], dwDownloaded);
                    }
                } while (dwSize > 0);

                WinHttpCloseHandle(hRequest);
                return r; // Thành công qua Keep-Alive!
            } else {
                // Thất bại do rớt gói/đứt socket -> refresh socket để thử lại ngay
                WinHttpCloseHandle(hRequest);
                cleanupConnect();
            }
        }
        return r;
    }
};

// Hàm tương thích ngược
inline Response request(const std::string& base, const std::string& method,
                        const std::string& path, const std::string& token,
                        const std::string& body) {
    Client cli(base, token);
    return cli.request(method, path, body);
}

#else

class Client {
private:
    std::string m_base, m_token;
    bool m_isHttps;
    std::string m_host, m_port, m_basePath;
public:
    Client() : m_isHttps(false) {}
    Client(const std::string& base, const std::string& token = "") { init(base, token); }
    void init(const std::string& base, const std::string& token = "") {
        m_base = base; m_token = token;
        parseBase(m_base, m_isHttps, m_host, m_port, m_basePath);
    }
    Response request(const std::string& method, const std::string& path, const std::string& body = "", int timeoutMs = 1500) {
        Response r;
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(m_host.c_str(), m_port.c_str(), &hints, &res) != 0 || !res) return r;

        sock_t fd = INVALID_SOCKET;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd == INVALID_SOCKET) continue;
            if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
            sock_close(fd); fd = INVALID_SOCKET;
        }
        freeaddrinfo(res);
        if (fd == INVALID_SOCKET) return r;

        std::string req = method + " " + m_basePath + path + " HTTP/1.0\r\n";
        req += "Host: " + m_host + "\r\n";
        req += "Authorization: Bearer " + m_token + "\r\n";
        req += "Connection: close\r\n";
        if (method == "POST") {
            req += "Content-Type: application/json\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "\r\n";
        if (method == "POST") req += body;

        size_t sent = 0;
        while (sent < req.size()) {
            int n = (int)send(fd, req.data() + sent, (int)(req.size() - sent), 0);
            if (n <= 0) { sock_close(fd); return r; }
            sent += n;
        }
        std::string raw; char buf[4096]; int n;
        while ((n = (int)recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
        sock_close(fd);

        auto sp = raw.find(' ');
        if (sp != std::string::npos) r.status = std::atoi(raw.c_str() + sp + 1);
        auto hdrEnd = raw.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) r.body = raw.substr(hdrEnd + 4);
        return r;
    }
};

inline Response request(const std::string& base, const std::string& method,
                        const std::string& path, const std::string& token,
                        const std::string& body) {
    Client cli(base, token);
    return cli.request(method, path, body);
}
#endif

} // namespace http
