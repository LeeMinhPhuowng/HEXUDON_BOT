# Bot mau HEXUDON — C++17, transport HTTP (raw socket, khong thu vien ngoai)

Bot noi server qua HTTP: GET /setup -> POST /assignment -> poll GET /state / POST /actions -> GET /result.
Gom: main.cpp - http.hpp (client HTTP raw socket) - minijson.hpp (JSON) - Makefile.

## Build
    g++ -std=c++17 -O2 -o bot main.cpp            # Linux/macOS
    g++ -std=c++17 -O2 -o bot main.cpp -lws2_32   # Windows (MinGW)
(hoac: make  /  make LDLIBS=-lws2_32 tren Windows MinGW)

## Chay (noi tran that)
    ./bot <URL> <MATCH_ID> <TOKEN>
    vd: ./bot http://judge:8099 m-0001 <token-doi-cua-ban>

Lay TOKEN + MATCH_ID o trang "Tran cua toi". Chi can sua ham planActions() de cai tien chien thuat.
