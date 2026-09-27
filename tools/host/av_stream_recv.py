#!/usr/bin/env python3
"""Raw A/V stream receiver for the freewebcamera UVC dump.

The board's `uvc dump listen <port>` waits for this script to dial in
(192.168.0.249 is the board), or `uvc dump <ip>:<port>` can dial out to a
listening instance here.  Either way the board writes the capture bytes with
no in-band framing at all: what lands in the output file is exactly what the
capture side read out of the video(4) read method.  For MJPG that means JPEG
SOI/EOI markers are the only structure - --split-jpg carves them.

The connection is re-made as needed (the board reconnects, or --connect
retries), and every connection appends to the same output file, so a long
run is one file plus a line per (re)connection.

usage:
  av_stream_recv.py <port> [-c <board-ip>] [-l <listen-ip>] [-o out.mjpg]
                    [--split-jpg <dir>] [--stats] [--wait <seconds>]

examples:
  python3 tools/host/av_stream_recv.py 9100 -c 192.168.0.249 -o video.mjpg
  # board: uvc video on 640x480 MJPG 15 ; uvc dump listen 9100
  python3 tools/host/av_stream_recv.py 9100 -o video.mjpg --split-jpg frames/
"""

import argparse
import os
import socket
import sys
import time

SOI = b"\xff\xd8\xff"
EOI = b"\xff\xd9"


def split_jpg(path, outdir):
    """Carve the raw byte stream into JPEG frames by SOI/EOI."""
    with open(path, "rb") as f:
        data = f.read()
    os.makedirs(outdir, exist_ok=True)
    frames, i, n = [], 0, 0
    while True:
        start = data.find(SOI, i)
        if start < 0:
            break
        end = data.find(EOI, start)
        if end < 0:
            break
        end += len(EOI)
        n += 1
        out = os.path.join(outdir, "frame-%04d.jpg" % n)
        with open(out, "wb") as g:
            g.write(data[start:end])
        frames.append(end - start)
        i = end
    total = sum(frames)
    print("split-jpg: %d frames, %d bytes of %d in-stream bytes"
          % (n, total, len(data)))
    if frames:
        print("split-jpg: first=%d min=%d max=%d mean=%d bytes"
              % (frames[0], min(frames), max(frames), total // n))
    return n


def dial(host, port, wait_s):
    """Connect to the board, retrying while it is not listening yet."""
    deadline = time.time() + wait_s
    while True:
        try:
            return socket.create_connection((host, port), timeout=5)
        except OSError as e:
            if time.time() >= deadline:
                print("connect %s:%d failed: %s" % (host, port, e))
                return None
            time.sleep(1.0)


def serve(conn, label, out, args, state):
    """Drain one connection into the output file."""
    state["conns"] += 1
    print("connection %d from %s" % (state["conns"], label))
    sys.stdout.flush()
    start = time.time()
    last_report = state["total"]
    try:
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            out.write(chunk)
            state["total"] += len(chunk)
            if args.stats and state["total"] - last_report >= (1 << 20):
                last_report = state["total"]
                print("  %d bytes (%.1f KB/s)"
                      % (state["total"], state["total"] / 1024.0 /
                         max(0.001, time.time() - start)))
                sys.stdout.flush()
    finally:
        conn.close()
    print("connection %d closed: %d bytes total (%d conns)"
          % (state["conns"], state["total"], state["conns"]))
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(prog="av_stream_recv.py")
    ap.add_argument("port", type=int, help="TCP port (9100 for video)")
    ap.add_argument("-c", "--connect", metavar="BOARD_IP",
                    help="dial the board (uvc dump listen <port>); "
                         "without this the script listens instead")
    ap.add_argument("-l", "--listen", default="0.0.0.0")
    ap.add_argument("-o", "--out", default="stream.bin",
                    help="raw byte output file (default stream.bin)")
    ap.add_argument("--split-jpg", metavar="DIR",
                    help="afterwards carve the bytes into DIR/frame-NNNN.jpg")
    ap.add_argument("--stats", action="store_true",
                    help="print a progress line every ~1 MB")
    ap.add_argument("--wait", type=int, default=120,
                    help="seconds to keep retrying --connect (default 120)")
    args = ap.parse_args()

    state = {"total": 0, "conns": 0}
    srv = None
    if args.connect:
        print("dialing board %s:%d -> %s (Ctrl+C to stop)"
              % (args.connect, args.port, args.out))
    else:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((args.listen, args.port))
        srv.listen(1)
        print("listening on %s:%d -> %s (Ctrl+C to stop)"
              % (args.listen, args.port, args.out))
    sys.stdout.flush()

    try:
        with open(args.out, "wb") as out:
            while True:
                if args.connect:
                    conn = dial(args.connect, args.port, args.wait)
                    if conn is None:
                        break
                    serve(conn, "%s:%d" % (args.connect, args.port),
                          out, args, state)
                    args.wait = 5  # a broken stream: don't wait long again
                else:
                    conn, addr = srv.accept()
                    serve(conn, "%s:%d" % (addr[0], addr[1]), out, args,
                          state)
    except KeyboardInterrupt:
        print("\nstopping: %d bytes in %s over %d connection(s)"
              % (state["total"], args.out, state["conns"]))
    finally:
        if srv is not None:
            srv.close()

    if args.split_jpg:
        split_jpg(args.out, args.split_jpg)
    return 0


if __name__ == "__main__":
    sys.exit(main())