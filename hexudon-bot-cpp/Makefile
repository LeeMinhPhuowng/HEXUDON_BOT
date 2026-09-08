# Bot mau HEXUDON — C++17, transport HTTP (raw socket, khong thu vien ngoai).
# Linux/macOS: khong can lib them. Windows (MinGW g++): make LDLIBS=-lws2_32 (MSVC tu link).
all:
	g++ -std=c++14 -O3 -Wall -o bot main.cpp -lws2_32 -lwinhttp

clean:
	rm -f bot bot.exe bot_*.exe

