#!/usr/bin/env python3
"""Capture every USB log byte to a new file. Requires pyserial on the attached PC."""
import argparse
from datetime import datetime
from pathlib import Path
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='COM5, /dev/ttyACM0, etc.')
    parser.add_argument('--output', type=Path, help='New log file; existing files are never overwritten')
    parser.add_argument('--send', choices=('RUN', 'PD', 'SBTL'), help='Send one local firmware action after opening')
    parser.add_argument('--stop-after-suite', action='store_true')
    args = parser.parse_args()
    try:
        import serial
    except ImportError:
        parser.error('pyserial missing: python -m pip install pyserial')
    output = args.output or Path('logs') / ('mioty-v3-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '.log')
    output.parent.mkdir(parents=True, exist_ok=True)
    print(f'Logging to {output.resolve()} (Ctrl-C to stop)', file=sys.stderr)
    with output.open('xb') as log_file, serial.Serial(args.port, 115200, timeout=0.2) as port:
        if args.send:
            port.write((args.send + '\r').encode('ascii'))
            port.flush()
        tail = b''
        try:
            while True:
                data = port.read(4096)
                if not data:
                    time.sleep(0.01)
                    continue
                log_file.write(data)
                log_file.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                tail = (tail + data)[-1024:]
                if args.stop_after_suite and b'Ctrl-C stops a running suite.\r\n' in tail:
                    break
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
