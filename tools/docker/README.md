# Workstation Debian build

From the repository root:

```sh
python3 tools/docker-build.py
```

This builds committed `HEAD`, runs the release build's Meson tests, and creates
ordinary Debian packages in ignored `build-docker/<timestamp>-<revision>/`.
Commit source changes before building. Uncommitted files are excluded by
`git archive`. Use `--revision <commit>` to select another committed revision.

The image uses pinned Debian trixie and Noctalia's signed APT repository,
with wlroots 0.20.1, Wayland 1.25, libdrm 2.4.134, and libinput 1.28.1 matching
the workstation baseline. Debian packaging and compatibility patches come
from the exact stock source package. The image records all installed build
package versions. Remaining dependencies follow the repository at image build
time, so this is a compatibility baseline rather than a bit reproducible build.

The build runs as the caller's UID with four CPUs and an 8 GiB memory limit.
It has no network, GPU, desktop socket, home directory, or Docker socket access.
Only the Git archive and output directory are mounted. Image preparation needs
network access. The wrapper uses passwordless `sudo -n docker` if direct Docker
access is unavailable, without changing group membership.

Output includes the custom `.deb`, matching stock `.deb` for rollback,
test logs, source/image provenance, dependency versions, and checksums.
The version suffix lets the custom package replace this stock version while
allowing a newer upstream package to supersede it. No packages are installed
and no session is restarted by this command. Live GPU handoff still requires
separate guarded hardware testing.

When the host baseline changes, update the exact dependency pins and key hash
in `Dockerfile`, then use `--package-version <stock-version> --rebuild-image`.
Do not treat an older image as compatible with a new wlroots ABI.
