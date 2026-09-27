#!/usr/bin/env python3
"""Raw A/V stream receiver for the freewebcamera UVC/UAC dump.

The board's `uvc dump [video|audio] listen <port>` waits for this script to
dial in (192.168.0.249 is the board), or `uvc dump [video|audio]
<ip>:<port>` can dial out to a listening instance here.  Either way the
board writes the capture bytes with no in-band framing at all: what lands
in the output file is exactly what the capture side read out of the
middle layer.  For MJPG that means JPEG SOI/EOI markers are the only
structure - --split-jpg carves them; for the audio channel the bytes are
raw PCM (S16_LE from the UAC microphone) and --wav wraps them in a RIFF
header so players/ffprobe can open the file.

The connection is re-made as needed (the board reconnects, or --connect
retries), and every connection appends to the same output file, so a long
run is one file plus a line per (re)connection.

usage:
  av_stream_recv.py <port> [-c <board-ip>] [-l <listen-ip>] [-o out.bin]
                    [--split-jpg <dir>] [--wav <out.wav> --rate <hz>
                    --channels <n>] [--stats] [--wait <seconds>]

examples:
  python3 tools/host/av_stream_recv.py 9100 -c 192.168.0.249 -o video.mjpg
  # board: uvc video on 640x480 MJPG 15 ; uvc dump listen 9100
  python3 tools/host/av_stream_recv.py 9100 -o video.mjpg --split-jpg frames/
  python3 tools/host/av_stream_recv.py 9101 -c 192.168.0.249 -o audio.pcm \\
        --wav audio.wav --rate 48000 --channels 2
  # board: uvc audio on ; uvc dump audio listen 9101
"""

import argparse
import os
import socket
import struct
import sys
import time
import wave

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


def wrap_wav(pcm_path, wav_path, rate, channels, width=2):
    """Wrap a raw PCM dump in a RIFF/WAVE header.

    The board streams the microphone's samples with no header (that would
    be in-band framing), so the format is whatever `uvc audio info`
    negotiated - pass it in.  Everything here is little-endian S16 unless
    --width says otherwise.
    """
    with open(pcm_path, "rb") as f:
        pcm = f.read()
    if width != 2:
        # wave only writes the standard widths; trim to a whole frame and
        # let the header carry the truth
        pass
    with wave.open(wav_path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(pcm)
    samples = len(pcm) // max(1, width * channels)
    print("wav: %s <- %s (%d bytes, %.2f s, %d Hz, %d ch, %d-bit)"
          % (wav_path, pcm_path, len(pcm),
             samples / float(rate) if rate else 0.0,
             rate, channels, width * 8))
    return wav_path


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
    ap.add_argument("--wav", metavar="OUT.wav",
                    help="afterwards wrap the raw bytes as a RIFF/WAVE file "
                         "(the audio channel; needs --rate and --channels)")
    ap.add_argument("--rate", type=int, default=48000,
                    help="PCM sample rate for --wav (default 48000)")
    ap.add_argument("--channels", type=int, default=2,
                    help="PCM channel count for --wav (default 2)")
    ap.add_argument("--width", type=int, default=2,
                    help="PCM bytes per sample for --wav (default 2 = 16-bit)")
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
    if args.wav:
        wrap_wav(args.out, args.wav, args.rate, args.channels, args.width)
    return 0


if __name__ == "__main__":
    sys.exit(main())