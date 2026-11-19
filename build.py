#!/usr/bin/env python3
"""Build this sketch against the sibling client without copying/editing its source."""
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
    parser.add_argument('--tools', type=Path, default=DEFAULT_TOOLS,
                        help='Existing Arduino CLI directory containing data/packages')
    parser.add_argument('--client', type=Path, default=ROOT.parent / 'mioty_at_client_c')
    args = parser.parse_args()
    tools = args.tools.resolve()
    client = args.client.resolve()
    cli = str(tools / 'arduino-cli') if (tools / 'arduino-cli').is_file() else shutil.which('arduino-cli')
    if not cli:
        parser.error('Arduino CLI missing; supply --tools with the installed STM32 3.0.0 tools')
    if not (tools / 'data/packages/STMicroelectronics/hardware/stm32/3.0.0/platform.txt').is_file():
        parser.error('STM32 core 3.0.0 missing in --tools/data; this script does not install packages')
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    sources = sorted((client / 'src').rglob('*'))
    digest = hashlib.sha256()
    for path in sources:
        if path.is_file():
            digest.update(path.relative_to(client).as_posix().encode())
            digest.update(path.read_bytes())
    try:
        commit = subprocess.check_output(['git', '-C', str(client), 'rev-parse', 'HEAD'], text=True).strip()
    except subprocess.CalledProcessError:
        commit = 'unknown'
    config = build / 'arduino-cli.yaml'
    config.write_text('directories:\n  data: ' + json.dumps(str(tools / 'data')) +
                      '\n  downloads: ' + json.dumps(str(build / 'downloads')) +
                      '\n  user: ' + json.dumps(str(build / 'arduino-user')) + '\n' +
                      'build_cache:\n  path: ' + json.dumps(str(build / 'cache')) + '\n')
    fqbn = 'STMicroelectronics:stm32:GenL0:pnum=GENERIC_L072RBTX,xserial=none,upload_method=dfuMethod,usb=CDCgen'
    command = [cli, '--config-file', str(config), 'compile', '--fqbn', fqbn,
               '--library', str(client), '--build-path', str(build / 'firmware'),
               '--build-property', 'build.extra_flags=-DCLIENT_REV=' + commit[:12], str(ROOT)]
    subprocess.run(command, check=True, env={**os.environ, 'TMPDIR': str(build)})
    binary = build / 'firmware/m3b_at_terminal.ino.bin'
    result = build / 'm3b_v3_autotest.bin'
    shutil.copy2(binary, result)
    manifest = {'client_commit': commit, 'client_source_sha256': digest.hexdigest(),
                'firmware_sha256': hashlib.sha256(result.read_bytes()).hexdigest(),
                'fqbn': fqbn, 'command': command}
    (build / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Firmware: {result}\nManifest: {build / "manifest.json"}')


if __name__ == '__main__':
    main()
