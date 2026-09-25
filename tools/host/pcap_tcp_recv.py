#!/usr/bin/env python3
"""pcap-over-TCP receiver for the freewebcamera tcpdump.

The board's `tcpdump -i wl0 -w <ip>:<port>` connects OUT to this script
(the board is the TCP client; no inbound firewall rules needed), writes the
libpcap stream it sends, and the resulting file opens directly in
Wireshark. Stop the capture with `tcpdump -p` on the board (or Ctrl+C
here) - Wireshark can open the file even while the capture is running.

usage:
  pcap_tcp_recv.py [-l <listen-ip>] <port> [-o out.pcap]

example:
  python3 tools/host/pcap_tcp_recv.py 9000 -o run1.pcap
  # board: tcpdump -i wl0 -w 192.168.0.18:9000
"""

import argparse
import signal
import socket
import sys

PCAP_GLOBAL_HEADER = (
    0xA1B2C3D4.to_bytes(4, "little")	# magic
    + (2).to_bytes(2, "little")		# version major
    + (4).to_bytes(2, "little")		# version minor
    + (0).to_bytes(4, "little")		# thiszone
    + (0).to_bytes(4, "little")		# sigfigs
    + (0xFFFF).to_bytes(4, "little")	# snaplen
    + (1).to_bytes(4, "little")		# linktype: Ethernet
)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", type=int)
    ap.add_argument("-l", "--listen", default="0.0.0.0")
    ap.add_argument("-o", "--out", default="capture.pcap")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.listen, args.port))
    srv.listen(1)
    print(f"listening on {args.listen}:{args.port}, writing {args.out}")
    print("waiting for the board to connect (tcpdump -i wl0 -w <ip>:%d)" % args.port)

    conn, peer = srv.accept()
    print(f"board connected from {peer[0]} - capturing; Ctrl+C to stop")
    with open(args.out, "wb") as f:
        f.write(PCAP_GLOBAL_HEADER)
        f.flush()
        total = 0
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    print("board closed the stream")
                    break
                f.write(chunk)
                total += len(chunk)
                if total % (1024 * 1024) < 65536:
                    print(f"  {total} bytes captured...", flush=True)
        except KeyboardInterrupt:
            print("\nstopped by user")
        finally:
            conn.close()
            print(f"{total} bytes -> {args.out} (open in Wireshark)")
    srv.close()
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)
    sys.exit(main())
