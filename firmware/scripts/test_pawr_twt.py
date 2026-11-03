#!/usr/bin/env python3
"""Capture the factory PAwR/TWT test (pylink-square and pyserial required).

Flash the HC33 wake-test and nRF sensor-debug images first. J-Link is attached
throughout this functional test; use an external meter with it detached for
current measurements. No claim about RF power state follows from RX alone.
"""
import argparse
import re
import time
from pathlib import Path

import pylink
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hc33', default='/dev/cu.SLAB_USBtoUART')
    parser.add_argument('--jlink-serial', type=int, default=607000398)
    parser.add_argument('--jlink-library', default='/Applications/SEGGER/JLink/libjlinkarm.dylib')
    parser.add_argument('--seconds', type=float, default=50)
    parser.add_argument('--mode', choices=('sparse', 'quiet', 'button'), default='sparse')
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--leave-running', action='store_true', help='Detach without cancelling the negotiated session')
    args = parser.parse_args()
    link = pylink.JLink(lib=pylink.library.Library(dllpath=args.jlink_library))
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.hc33, 115200, 0
    port.dtr = port.rts = False
    active = {}
    expired = False
    inside = outside = 0
    buffers = {'NRF': '', 'HC33': ''}
    trigger_at = None
    triggered = False
    stopped = False
    led_checks = []
    started = time.monotonic()
    try:
        port.open()
        link.open(serial_no=args.jlink_serial)
        link.set_tif(pylink.enums.JLinkInterfaces.SWD)
        link.connect('NRF54L15_M33', speed=4000)
        link.reset(halt=False)
        link.rtt_start()
        with args.log.open('w') as output:
            while time.monotonic() - started < args.seconds:
                for name, data in [('NRF', bytes(link.rtt_read(0, 8192))), ('HC33', port.read(8192))]:
                    buffers[name] += data.decode(errors='replace')
                    lines = buffers[name].split('\n')
                    buffers[name] = lines.pop()
                    for raw in lines:
                        line = re.sub(r'\x1b\[[0-9;]*m', '', raw).strip()
                        record = f'{time.monotonic() - started:.3f} [{name}] {line}\n'
                        output.write(record)
                        if name == 'NRF' and '*** Booting Zephyr' in line:
                            active.clear()
                            expired = False
                            inside = outside = 0
                            trigger_at = None
                            triggered = False
                        if 'PAWR_TWT' in line or 'TWT_TSF' in line or 'TWT_PROBE cmd' in line:
                            print(record, end='', flush=True)
                        if name == 'NRF' and ('PAWR_TWT LED on' in line or 'PAWR_TWT LED off' in line):
                            # Board DTS: active-low LED at P2.05. Nordic MDK:
                            # GPIO2 base=0x50050400, OUT=0, DIR=0x10.
                            out = link.memory_read32(0x50050400, 1)[0]
                            direction = link.memory_read32(0x50050410, 1)[0]
                            on = 'LED on' in line
                            valid = (not bool(out & 32) and bool(direction & 32)) if on else (bool(out & 32) and not bool(direction & 32))
                            led_checks.append(valid)
                            check = f'LED_GPIO expected_on={on} OUT5={(out >> 5) & 1} DIR5={(direction >> 5) & 1} valid={valid}\n'
                            output.write(check)
                            print(check, end='', flush=True)
                        if 'PAWR_TWT ACTIVE' in line:
                            match = re.search(r'session=(\d+)', line)
                            if match:
                                active[name] = int(match.group(1))
                            if name == 'HC33' and trigger_at is None:
                                trigger_at = time.monotonic() + (18 if args.mode == 'button' else 0)
                        if name == 'NRF' and 'PAWR_TWT RX sequence=' in line:
                            if 'planned=inside' in line:
                                inside += 1
                            elif 'planned=outside' in line:
                                outside += 1
                        if name == 'NRF' and 'PAWR_TWT EXPIRED' in line:
                            expired = True
                            match = re.search(r'received_inside=(\d+) received_outside=(\d+)', line)
                            if match:
                                inside, outside = map(int, match.groups())
                if trigger_at is not None and not triggered and time.monotonic() >= trigger_at:
                    port.write(b'q' if args.mode == 'quiet' else b'w' if args.mode == 'button' else b't')
                    triggered = True
                if not args.leave_running and triggered and not stopped and args.mode != 'quiet' and time.monotonic() - trigger_at > (10 if args.mode == 'button' else 35):
                    port.write(b'x')
                    stopped = True
                output.flush()
                if expired:
                    # Capture command removal and full rail shutdown.
                    if trigger_at is not None and time.monotonic() - trigger_at > 38:
                        break
                time.sleep(0.005)
    finally:
        if port.is_open:
            port.close()
        try:
            link.rtt_stop()
        finally:
            link.close()
    negotiated = set(active) == {'NRF', 'HC33'} and active['NRF'] == active['HC33']
    print(f'RESULT negotiated={negotiated} expired={expired} '
          f'inside={inside} outside={outside} LED_GPIO={led_checks}; current measured externally')
    led_ok = args.mode != 'button' or len(led_checks) >= 2 and all(led_checks)
    return 0 if negotiated and (expired or args.leave_running) and led_ok and (args.mode == 'quiet' or inside > 0) else 1


if __name__ == '__main__':
    raise SystemExit(main())
