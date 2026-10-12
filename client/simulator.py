"""Send explicitly simulated device readings; no third-party packages required."""
import argparse
import math
import re
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=9000)
    parser.add_argument('--device', default='simulated-sensor')
    parser.add_argument('--count', type=int, default=30)
    parser.add_argument('--interval', type=float, default=0.1)
    parser.add_argument('--fragment', action='store_true', help='Split each message across two sends')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', args.device):
        parser.error('device must contain 1..32 letters, digits, underscores or hyphens')
    if not 1 <= args.port <= 65535 or not 1 <= args.count <= 100000 or not math.isfinite(args.interval) or args.interval < 0:
        parser.error('invalid port/count/interval')
    with socket.create_connection((args.host, args.port), timeout=5) as client:
        for sequence in range(args.count):
            value = 20 + 5 * math.sin(sequence / 5)
            message = f'{args.device},{sequence},{value:.3f}\n'.encode('ascii')
            if args.fragment:
                middle = len(message) // 2
                client.sendall(message[:middle])
                time.sleep(0.005)
                client.sendall(message[middle:])
            else:
                client.sendall(message)
            time.sleep(args.interval)
    print(f'Sent {args.count} simulated readings from {args.device}. Check the server log for accepted records.')


if __name__ == '__main__':
    main()
