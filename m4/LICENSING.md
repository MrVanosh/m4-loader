# Licensing of the M4 loader's own files

This repository is GravityLinux's bootloader (a fork of m1n1) plus the M4 Pro (J616s) loader revisions on top of it.

- Upstream m1n1 code keeps its MIT licence (`LICENSE.MIT`). The fork's own contributions since `df7656cf` are
  GPL-2.0-only (`LICENSE.GPL2`, the fork's README), so the combined work is GPL-2.0-only.
- Our files carry their licence in their SPDX header (MIT or GPL-2.0-only).
- Our files without an SPDX header are **GPL-2.0-only**: everything under `m4/`, and `src/standalone/dart-capture.c`, `gates.h`,
  `level1.ld`, `loader-stdio.h`, `mcc.c`, `payload.S`, `upstream-spmi.inc` (92 % m1n1 `spmi.c`, MIT: that part stays MIT),
  `watchdog.c` and `sysinc/stdio.h`. The headers are not added in the files themselves because that would change the
  reference build bytes.
- `src/standalone/mcc.c` reads MCC TrustZone registers at the addresses of Yureka Lilian's M4 tree (reference `a4388f6c`,
  https://cyberchaos.dev/yuka/linux, GPL-2.0); the code itself is ours.

Never commit per-unit or per-install data: the `private/` inputs of `m4/BUILD.md` (ADT dumps, `gates-unit.h`,
`level1-install.h`, `private-install.conf`, the reference hashes of the harness output trees) stay off git, and so do built
images, which embed them.
