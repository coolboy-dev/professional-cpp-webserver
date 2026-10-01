#!/usr/bin/env python3
"""Raw-socket hardening matrix.  tests/raw.py --port 8080 --idle 3
Expects a server started with the given --idle. For the connection-cap
test start one with --max-conns 50 and run:  tests/raw.py --port P --max-conns 50
"""
import argparse, json, selectors, socket, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8080)
ap.add_argument("--idle", type=int, default=3, help="server --idle value")
ap.add_argument("--max-conns", type=int, default=0, help="run only the connection-cap test")
args = ap.parse_args()
HOST = "127.0.0.1"
fails = 0

def conn():
    return socket.create_connection((HOST, args.port))

def read_all(s, timeout=1.0):
    """Returns (data, eof_seen)."""
    s.settimeout(timeout)
    data, eof = b"", False
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                eof = True
                break
            data += chunk
    except socket.timeout:
        pass
    except ConnectionResetError:
        eof = True  # RST: counts as closed, but any unread reply is lost
    return data, eof

def collect(socks, timeout):
    """One recv per socket, all at once; returns the set that answered 200 within timeout."""
    sel = selectors.DefaultSelector()
    for s in socks:
        sel.register(s, selectors.EVENT_READ)
    got, deadline = set(), time.time() + timeout
    while sel.get_map() and time.time() < deadline:
        for key, _ in sel.select(max(0.0, deadline - time.time())):
            sel.unregister(key.fileobj)
            try:
                data = key.fileobj.recv(65536)
            except OSError:
                data = b""
            if statuses(data)[:1] == [200]:
                got.add(key.fileobj)
    sel.close()
    return got

def statuses(data):
    return [int(p[:3]) for p in data.split(b"HTTP/1.1 ")[1:]]

def check(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + detail))
    if not ok:
        fails += 1

def expect(name, payload, status, close=None):
    s = conn()
    s.sendall(payload)
    data, eof = read_all(s)
    s.close()
    got = statuses(data)
    ok = got[:1] == [status] and (close is None or eof == close)
    check(name, ok, f"got {got} eof={eof}")

def matrix():
    expect("bare CRLFCRLF -> 400", b"\r\n\r\n", 400, close=True)
    expect("method only -> 400", b"GET\r\n\r\n", 400, close=True)
    expect("HTTP/2.0 -> 505", b"GET / HTTP/2.0\r\n\r\n", 505, close=True)
    expect("unknown method -> 405", b"BREW /pot HTTP/1.1\r\n\r\n", 405)
    expect("encoded traversal -> 403", b"GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\n\r\n", 403)
    expect("plain traversal -> 403", b"GET /../../etc/passwd HTTP/1.1\r\n\r\n", 403)
    expect("a..b.txt is a name, not traversal -> 404", b"GET /a..b.txt HTTP/1.1\r\n\r\n", 404)
    expect("%00 in path -> 400", b"GET /%00 HTTP/1.1\r\n\r\n", 400, close=True)
    expect("control byte in path -> 400", b"GET /\x01 HTTP/1.1\r\n\r\n", 400, close=True)
    expect("header without colon -> 400", b"GET / HTTP/1.1\r\nnocolon\r\n\r\n", 400, close=True)
    expect("Content-Length -1 -> 400", b"GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 400, close=True)
    expect("Content-Length abc -> 400", b"GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 400, close=True)
    expect("Content-Length 20 digits -> 400", b"GET / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n", 400, close=True)
    expect("conflicting Content-Length -> 400", b"GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400, close=True)
    expect("Content-Length 70000 -> 413 + close", b"GET / HTTP/1.1\r\nContent-Length: 70000\r\n\r\n", 413, close=True)
    expect("Transfer-Encoding -> 501 + close", b"GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 501, close=True)
    expect("70 KB headers, no terminator -> 431 + close", b"GET / HTTP/1.1\r\n" + b"X: " + b"p" * 70000, 431, close=True)
    expect("9 KB request line -> 414 + close", b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\n\r\n", 414, close=True)
    expect("TLS ClientHello -> 400 + close", bytes.fromhex("160301020001000001fc0303") + b"\x00" * 32, 400, close=True)
    expect("missing Host accepted", b"GET / HTTP/1.1\r\n\r\n", 200)
    expect("10 KB garbage -> 400 + close", b"\x00\xff" * 5000, 400, close=True)

    # POST with a body is 405 but framed correctly, so a pipelined GET after it still works.
    s = conn()
    s.sendall(b"POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\nabGET / HTTP/1.1\r\nConnection: close\r\n\r\n")
    data, eof = read_all(s)
    check("POST body framed, pipelined GET served", statuses(data) == [405, 200] and eof, f"{statuses(data)} eof={eof}")
    s.close()

    # Content-Length announced but body never sent: must not hang forever, closed at idle.
    s = conn()
    s.sendall(b"POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nab")
    data, eof = read_all(s, timeout=args.idle + 1.5)
    check("incomplete body closed at idle timeout", eof and not data, f"eof={eof} data={data[:20]}")
    s.close()

    # Slowloris: many sockets trickle partial headers and hold; real clients unaffected; all reaped.
    held = []
    for _ in range(300):
        c = conn()
        c.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")
        held.append(c)
    probe = conn()
    probe.sendall(b"GET / HTTP/1.1\r\n\r\n")
    data, _ = read_all(probe, 1.0)
    check("served while 300 slowloris sockets are held", statuses(data) == [200])
    probe.close()
    time.sleep(args.idle + 1.5)
    reaped = 0
    for c in held:
        d, eof = read_all(c, 0.2)
        reaped += eof
        c.close()
    check("all 300 slowloris sockets reaped after idle", reaped == 300, f"reaped={reaped}")

    # Abandoned keep-alive connections are reaped and the connection count drops back.
    kept = []
    for _ in range(1000):
        c = conn()
        c.sendall(b"GET /test.html HTTP/1.1\r\n\r\n")
        kept.append(c)
    answered = len(collect(kept, 3.0))
    check("1000 keep-alive requests answered", answered == 1000, f"answered={answered}")
    time.sleep(args.idle + 1.5)
    probe = conn()
    probe.sendall(b"GET /api/info HTTP/1.1\r\nConnection: close\r\n\r\n")
    data, _ = read_all(probe)
    body = json.loads(data.split(b"\r\n\r\n", 1)[1])
    check("1000 abandoned keep-alive conns reaped", body["active_connections"] == 1, f"active={body['active_connections']}")
    for c in kept:
        c.close()

def max_conns_test():
    cap = args.max_conns
    socks = [conn() for _ in range(cap * 2)]  # the kernel completes all handshakes into the backlog
    for s in socks:
        s.sendall(b"GET /test.html HTTP/1.1\r\n\r\n")
    first = collect(socks, 1.5)
    check(f"exactly {cap} served while the rest wait in the backlog", len(first) == cap, f"served={len(first)}")
    for s in first:
        s.close()
    rest = [s for s in socks if s not in first]
    second = collect(rest, 2.0)
    check(f"remaining {len(rest)} served once slots free up", len(second) == len(rest), f"served={len(second)}")
    for s in rest:
        s.close()

if args.max_conns:
    max_conns_test()
else:
    matrix()
print()
print("ALL PASS" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
