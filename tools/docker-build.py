#!/usr/bin/env python3
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


PACKAGE_VERSION = '0.1.0~1135-g229d64a7-1~bpo13+1'
IMAGE = 'umbriel-workstation-builder:trixie'


def main():
    parser = argparse.ArgumentParser(description='Build a committed Umbriel revision into tested Debian packages.')
    parser.add_argument('--revision', default='HEAD')
    parser.add_argument('--package-version', default=PACKAGE_VERSION)
    parser.add_argument('--rebuild-image', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9][A-Za-z0-9.+~:-]*', args.package_version):
        parser.error('Invalid Debian package version')
    root = Path(__file__).resolve().parents[1]
    revision = subprocess.check_output(['git', 'rev-parse', '--verify', '--end-of-options',
                                        args.revision + '^{commit}'], cwd=root, text=True).strip()
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise RuntimeError('Unexpected Git revision')
    docker = ['docker']
    if subprocess.run(docker + ['info'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        docker = ['sudo', '-n', 'docker']
        subprocess.run(docker + ['info'], stdout=subprocess.DEVNULL, check=True)
    build = docker + ['build', '--platform=linux/amd64', '--tag', IMAGE,
                      '--label', 'umbriel.package-version=' + args.package_version,
                      '--build-arg', 'PACKAGE_VERSION=' + args.package_version]
    if args.rebuild_image:
        build.append('--no-cache')
    subprocess.run(build + [str(root / 'tools/docker')], check=True)
    image_id = subprocess.check_output(docker + ['image', 'inspect', '--format', '{{.Id}}', IMAGE], text=True).strip()
    timestamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = root / 'build-docker' / (timestamp + '-' + revision[:12])
    output.mkdir(parents=True)
    metadata = {'revision': revision, 'base_package_version': args.package_version,
                'version': args.package_version + '+workstation.' + revision[:12], 'image': image_id}
    with tempfile.TemporaryDirectory(prefix='umbriel-source-') as temporary:
        staging = Path(temporary)
        subprocess.run(['git', 'archive', '--format=tar', '--output=' + str(staging / 'source.tar'), revision],
                       cwd=root, check=True)
        (staging / 'source.json').write_text(json.dumps(metadata))
        subprocess.run(docker + ['run', '--rm', '--platform=linux/amd64', '--network=none',
                                '--cap-drop=ALL', '--security-opt=no-new-privileges',
                                '--user', f'{os.getuid()}:{os.getgid()}', '--cpus=4', '--memory=8g',
                                '--mount', f'type=bind,src={staging},dst=/input,readonly',
                                '--mount', f'type=bind,src={output},dst=/output', image_id], check=True)
    print(f'Build complete: {output}')


if __name__ == '__main__':
    main()
