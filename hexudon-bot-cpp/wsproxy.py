#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wsproxy.py — Ultra-Low-Latency WebSocket TLS Proxy với TCP_NODELAY = 1.
Chạy hoàn toàn ở quyền User thông thường (KHÔNG CẦN QUYỀN ADMINISTRATOR).
"""

import sys
import socket
import ssl
import select
import threading
import re

LOCAL_HOST = "127.0.0.1"
LOCAL_PORT = 8099
REMOTE_HOST = "procon.ptit.edu.vn"
REMOTE_PORT = 443

def handle_client(client_sock):
    client_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    remote_raw = socket.create_connection((REMOTE_HOST, REMOTE_PORT), timeout=10)
    remote_raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    remote_sock = ctx.wrap_socket(remote_raw, server_hostname=REMOTE_HOST)

    sockets = [client_sock, remote_sock]
    first_client_packet = True

    try:
        while True:
            r, _, x = select.select(sockets, [], sockets, 60.0)
            if x:
                break
            if not r:
                continue

            for s in r:
                data = s.recv(65536)
                if not data:
                    return

                if s is client_sock:
                    if first_client_packet:
                        # Thay thế Host header thành tên miền thật của server BTC để Nginx nhận diện
                        data = re.sub(
                            rb"Host:\s*[^\r\n]+",
                            f"Host: {REMOTE_HOST}".encode("utf-8"),
                            data,
                            count=1
                        )
                        first_client_packet = False
                    remote_sock.sendall(data)
                else:
                    client_sock.sendall(data)
    except Exception:
        pass
    finally:
        try: client_sock.close()
        except: pass
        try: remote_sock.close()
        except: pass

def main():
    port = LOCAL_PORT
    if len(sys.argv) > 1:
        try: port = int(sys.argv[1])
        except: pass

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((LOCAL_HOST, port))
    server.listen(5)
    print(f"============================================================")
    print(f"  WSPROXY ULTRA-LOW-LATENCY RUNNING ON ws://{LOCAL_HOST}:{port}")
    print(f"  Target: wss://{REMOTE_HOST}:{REMOTE_PORT}")
    print(f"  TCP_NODELAY: ACTIVE (Khong bi delay 40ms)")
    print(f"============================================================")

    while True:
        try:
            client_sock, addr = server.accept()
            t = threading.Thread(target=handle_client, args=(client_sock,), daemon=True)
            t.start()
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"[ERR] accept: {e}")

if __name__ == "__main__":
    main()
