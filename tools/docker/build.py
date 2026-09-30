import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


def main():
    output = Path('/output')
    metadata = json.loads(Path('/input/source.json').read_text())
    os.environ['HOME'] = '/tmp/build-home'
    Path(os.environ['HOME']).mkdir()
    os.environ['DEB_BUILD_OPTIONS'] = 'parallel=4'
    with tempfile.TemporaryDirectory(prefix='umbriel-build-') as temporary:
        work = Path(temporary)
        source = work / 'umbriel'
        source.mkdir()
        with tarfile.open('/input/source.tar') as archive:
            archive.extractall(source, filter='data')
        shutil.copytree('/opt/umbriel-debian', source / 'debian')
        revision_header = source / 'src/core/git_revision.h.in'
        revision_header.write_text(revision_header.read_text().replace('@VCS_TAG@', metadata['revision']))
        changelog = source / 'debian/changelog'
        lines = changelog.read_text().splitlines(keepends=True)
        lines[0] = f"umbriel ({metadata['version']}) unstable; urgency=medium\n"
        changelog.write_text(''.join(lines))
        rules = source / 'debian/rules'
        with rules.open('a') as handle:
            handle.write('\noverride_dh_auto_configure:\n'
                         '\tdh_auto_configure -- -Dtests=enabled -Dtest_ipc=disabled\n')
        try:
            subprocess.run(['dpkg-buildpackage', '-b', '-us', '-uc', '-j4'], cwd=source, check=True)
        finally:
            for log in source.glob('obj-*/meson-logs/testlog*'):
                shutil.copy2(log, output / log.name)
        artifacts = list(work.glob('*.deb')) + list(work.glob('*.buildinfo')) + list(work.glob('*.changes'))
        if not artifacts:
            raise RuntimeError('Build produced no package artifacts')
        for artifact in artifacts:
            shutil.copy2(artifact, output / artifact.name)
        for probe in source.glob('obj-*/copy-renderer-gpu-probe'):
            shutil.copy2(probe, output / probe.name)
        for stock in Path('/opt/stock').glob('*.deb'):
            shutil.copy2(stock, output / stock.name)
        shutil.copy2('/opt/build-packages.txt', output / 'build-packages.txt')
        (output / 'source.json').write_text(json.dumps(metadata, indent=2) + '\n')
        checksums = []
        for artifact in sorted(output.iterdir()):
            if artifact.is_file() and artifact.name != 'SHA256SUMS':
                checksums.append(f'{hashlib.sha256(artifact.read_bytes()).hexdigest()}  {artifact.name}\n')
        (output / 'SHA256SUMS').write_text(''.join(checksums))
    print(f'Packages and verification records saved to {output}')


if __name__ == '__main__':
    main()
