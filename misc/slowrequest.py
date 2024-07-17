#!/usr/bin/env python3

import socket
import sys
import time

timebase = 0.1
s = None
for res in socket.getaddrinfo("localhost", 8080, socket.AF_INET, socket.SOCK_STREAM):
    af, socktype, proto, canonname, sa = res
    try:
        s = socket.socket(af, socktype, proto)
    except:
        s = None
        continue

    s.connect(sa)

MSG = """POST /asdqweasd HTTP/1.1\r
a: b\r
c: d\r
Content-Length: 6\r
\r
Hello!"""

with s:
    for i in range(len(MSG)):
        #s.sendall(MSG[i].encode('utf-8'))
        print(ord(MSG[i]))
        try:
            s.sendall(bytes([ord(MSG[i])]))
        except:
            break
        time.sleep(timebase)
    while True:
        try:
            data = s.recv(1)
        except:
            break
        if(len(data) == 0):
            break
        time.sleep(timebase)
        print(data.decode('utf-8'), end='')

print()
