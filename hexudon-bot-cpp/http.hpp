// http.hpp — HTTP/HTTPS client tối giản. Hỗ trợ HTTPS trên Windows qua WinHTTP.
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

inline Response request(const std::string& base, const std::string& method,
                        const std::string& path, const std::string& token,
                        const std::string& body) {
    bool isHttps = false;
    std::string host, port, basePath;
    parseBase(base, isHttps, host, port, basePath);

    Response r;
    HINTERNET hSession = WinHttpOpen(L"HexudonBot/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return r;

    INTERNET_PORT nPort = (INTERNET_PORT)std::atoi(port.c_str());
    HINTERNET hConnect = WinHttpConnect(hSession, toWide(host).c_str(), nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return r; }

    std::string fullPath = basePath + path;
    DWORD dwFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, toWide(method).c_str(), toWide(fullPath).c_str(), NULL, NULL, NULL, dwFlags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r; }

    // Bỏ qua lỗi SSL nếu có (tự ký/hết hạn)
    if (isHttps) {
        DWORD dwSecFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE |
                           SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecFlags, sizeof(dwSecFlags));
    }

    std::string headers = "Authorization: Bearer " + token + "\r\n";
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
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return r;
}

#else

inline Response request(const std::string& base, const std::string& method,
                        const std::string& path, const std::string& token,
                        const std::string& body) {
    bool isHttps = false;
    std::string host, port, basePath;
    parseBase(base, isHttps, host, port, basePath);

    Response r;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return r;

    sock_t fd = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        sock_close(fd); fd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) return r;

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
#endif

} // namespace http
