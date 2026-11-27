#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Replace a release download with the verified artifact from the current CI run."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile


def gh(*args):
    return subprocess.check_output(['gh', *args], text=True)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    folder = Path(sys.argv[1])
    repo = os.environ['GITHUB_REPOSITORY']
    revision = os.environ['GITHUB_SHA']
    assert (folder / 'SOURCE_REVISION.txt').read_text(encoding='utf-8-sig').strip() == revision
    archives = list(folder.glob('neoj2k-viewer-*-win-x64.zip'))
    assert len(archives) == 1, 'Expected one standalone ZIP'
    archive = archives[0]
    version = archive.name.removeprefix('neoj2k-viewer-').removesuffix('-win-x64.zip')
    tag = 'v' + version
    with zipfile.ZipFile(archive) as z:
        sums = dict(line.split()[::-1] for line in z.read('SHA256SUMS').decode().splitlines() if line.strip())
        assert sums, 'Empty ZIP checksums'
        for name, digest in sums.items():
            assert sha(z.read(name)) == digest, 'ZIP member checksum mismatch'
        dll_hash = sha(z.read('neoj2k.dll'))
    release = json.loads(gh('release', 'view', tag, '--repo', repo, '--json', 'body,assets'))
    previous = [a for a in release['assets'] if a['name'] == archive.name]
    assert len(previous) == 1, 'Expected one existing versioned asset'
    backup = folder / 'previous'
    backup.mkdir()
    gh('release', 'download', tag, '--repo', repo, '--pattern', archive.name, '--dir', str(backup))
    old_hash = sha((backup / archive.name).read_bytes())
    assert previous[0]['digest'] == 'sha256:' + old_hash
    (backup / 'release.json').write_text(json.dumps(release, indent=2))
    new_hash = sha(archive.read_bytes())
    body = release['body'].replace(old_hash, new_hash)
    with zipfile.ZipFile(backup / archive.name) as old_zip:
        body = body.replace(sha(old_zip.read('neoj2k.dll')), dll_hash)
    body = body.replace(f"{previous[0]['size']:,} bytes", f"{archive.stat().st_size:,} bytes")
    body += f'\n\nCI-built standalone from `{revision}`. Build and verification https://github.com/{repo}/actions/runs/{os.environ["GITHUB_RUN_ID"]}.\n'
    notes = folder / 'release-body.md'
    notes.write_text(body)
    gh('release', 'upload', tag, str(archive), '--repo', repo, '--clobber')
    verified = folder / 'verified'
    verified.mkdir()
    gh('release', 'download', tag, '--repo', repo, '--pattern', archive.name, '--dir', str(verified))
    assert sha((verified / archive.name).read_bytes()) == new_hash
    gh('release', 'edit', tag, '--repo', repo, '--notes-file', str(notes))
    print(f'Published verified CI archive {archive.name}: {new_hash}')


if __name__ == '__main__':
    main()
