# Bot mau HEXUDON — C++14/C++17, HTTP & WebSocket transports (raw socket, khong thu vien ngoai).
# Windows (MinGW g++): -lws2_32 -lwinhttp
all: bot botws

bot: main.cpp http.hpp minijson.hpp
	g++ -std=c++14 -O3 -Wall -o bot main.cpp -lws2_32 -lwinhttp

botws: mainws.cpp ws.hpp minijson.hpp
	g++ -std=c++14 -O3 -Wall -o botws mainws.cpp -lws2_32 -lwinhttp

bot_brand: mainws.cpp ws.hpp minijson.hpp
	g++ -std=c++14 -O3 -Wall -o bot_brand mainws.cpp -lws2_32 -lwinhttp

clean:
	rm -f bot bot.exe botws botws.exe bot_*.exe
