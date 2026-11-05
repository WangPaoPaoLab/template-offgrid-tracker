#!/usr/bin/env python3
"""nRF54L15 CMSIS-DAP/OpenOCD flashing and optional RTT server (1 MHz).

Uses PlatformIO's nRF54-capable OpenOCD, not an older system OpenOCD.
Flashing verifies the image before reset. No automatic recover/mass erase.
"""
import argparse
import os
from pathlib import Path
import subprocess


def tcl_path(path):
    value = str(path.resolve())
    if any(c in value for c in '{}\\\n\r'):
        raise ValueError('Unsupported character in OpenOCD image path')
    return '{' + value + '}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('probe', 'flash', 'serve'))
    parser.add_argument('image', nargs='?', type=Path, help='HEX, or BIN with --address')
    parser.add_argument('--address', type=lambda value: int(value, 0), help='Explicit load address for a BIN image')
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
    parser.add_argument('--openocd', type=Path, default=core / 'packages/tool-openocd/bin/openocd')
    parser.add_argument('--serial', help='CMSIS-DAP adapter serial; optional with one probe')
    parser.add_argument('--elf', type=Path, help='Exact flashed ELF; enables RTT on localhost:9090')
    parser.add_argument('--reset', action='store_true', help='Reset when starting a server')
    args = parser.parse_args()
    try:
        scripts = args.openocd.resolve().parent.parent / 'openocd/scripts'
        cmd = [str(args.openocd), '-s', str(scripts), '-f', 'interface/cmsis-dap.cfg',
               '-f', 'target/nordic/nrf54l.cfg', '-c', 'adapter speed 1000; bindto 127.0.0.1']
        if args.serial:
            if not all(c.isalnum() or c in '-_.' for c in args.serial):
                raise ValueError('Invalid adapter serial')
            cmd += ['-c', 'adapter serial ' + args.serial]
        if args.action == 'flash':
            if args.image is None or not args.image.is_file():
                raise ValueError('flash requires an existing image')
            image = tcl_path(args.image)
            if args.image.suffix.lower() == '.bin':
                if args.address is None or args.address < 0:
                    raise ValueError('BIN images require an explicit nonnegative --address')
                image += f' 0x{args.address:x} bin'
            elif args.image.suffix.lower() != '.hex' or args.address is not None:
                raise ValueError('Use HEX without --address, or BIN with --address')
            # RRAMC.CONFIG: enable writes and write buffer, as in Seeed's
            # nRF54 uploader. Restore read mode before verification/reset.
            cmd += ['-c', 'init; set result_code [catch {reset halt; '
                    'mww 0x5004b500 0x101; load_image ' + image + '; '
                    'mww 0x5004b500 0; verify_image ' + image + '} result]; '
                    'if {$result_code} {catch {mww 0x5004b500 0}; echo $result; shutdown error} '
                    'else {echo $result; echo {Image verification passed}; reset run; shutdown}']
        elif args.action == 'probe':
            cmd += ['-c', 'init; targets; shutdown']
        else:
            cmd += ['-c', 'init' + ('; reset run' if args.reset else '')]
            if args.elf:
                from elftools.elf.elffile import ELFFile
                with args.elf.open('rb') as stream:
                    table = ELFFile(stream).get_section_by_name('.symtab')
                    found = table.get_symbol_by_name('_SEGGER_RTT') if table else None
                    if not found or len(found) != 1:
                        raise ValueError('ELF must contain one _SEGGER_RTT symbol')
                    address = found[0]['st_value']
                cmd += ['-c', f'rtt setup 0x{address:x} 24 "SEGGER RTT"; '
                        'rtt start; rtt server start 9090 0']
        return subprocess.call(cmd)
    except (OSError, ValueError, ImportError) as error:
        parser.exit(1, f'OpenOCD setup failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
