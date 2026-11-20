#!/usr/bin/env python3
"""Build the USB-to-RF serial terminal."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
ROOT = Path(__file__).resolve().parent
DEFAULT_TOOLS = ROOT.parent.parent / 'mioty-m3board' / '.tools' / 'linux'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, default=DEFAULT_TOOLS)
    args = parser.parse_args()
    tools = args.tools.resolve()
    cli = tools / 'arduino-cli'
    if not cli.is_file() or not (tools / 'data/packages/STMicroelectronics/hardware/stm32/3.0.0/platform.txt').is_file():
        parser.error('Arduino CLI and STM32 core 3.0.0 required; supply --tools')
    sketch = ROOT
    build = ROOT / 'build/terminal'
    build.mkdir(parents=True, exist_ok=True)
    config = build / 'arduino-cli.yaml'
    config.write_text('directories:\n  data: ' + json.dumps(str(tools / 'data')) +
                      '\n  downloads: ' + json.dumps(str(build / 'downloads')) +
                      '\n  user: ' + json.dumps(str(build / 'arduino-user')) + '\n' +
                      'build_cache:\n  path: ' + json.dumps(str(build / 'cache')) + '\n')
    fqbn = 'STMicroelectronics:stm32:GenL0:pnum=GENERIC_L072RBTX,xserial=none,upload_method=dfuMethod,usb=CDCgen'
    command = [str(cli), '--config-file', str(config), 'compile', '--fqbn', fqbn,
               '--build-path', str(build / 'firmware'), str(sketch)]
    subprocess.run(command, check=True, env={**os.environ, 'TMPDIR': str(build)})
    result = ROOT / 'build/m3b_at_terminal.bin'
    shutil.copy2(build / 'firmware/m3b_at_terminal.ino.bin', result)
    manifest = {
        'original_source_commit': '92eff93e5d4fb3d868ed974ed2744a2cfab6d3e5',
        'source_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(sketch.iterdir()) if p.is_file() and p.suffix in {'.ino', '.cpp', '.h'}},
        'firmware_sha256': hashlib.sha256(result.read_bytes()).hexdigest(),
        'fqbn': fqbn, 'command': command,
    }
    manifest_path = ROOT / 'build/terminal_manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Firmware: {result}\nManifest: {manifest_path}')


if __name__ == '__main__':
    main()
