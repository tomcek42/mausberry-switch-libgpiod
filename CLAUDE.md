# CLAUDE.md

Target system is a Raspberry Pi running Volumio 4 (Debian 12 Bookworm, armhf).

## Language

Everything in this repository is written in English: code, comments, README,
commit messages, tags, release titles and release notes, and this file.
Release notes are structured like v0.9.5: "What's new", "Installation",
"Configuration".

## Packaging (nfpm)

- nfpm copies `depends` from `nfpm.yaml` verbatim into the `control` file.
  Version constraints must use Debian syntax with parentheses:
  `libgpiod2 (>= 1.0)`. Without them, Bookworm's dpkg rejects the package
  with a parse error (the older dpkg on Volumio 3 accepted it) – this was
  the bug in v0.9.1.
- No `preinst`/`postinst` scripts that call `apt-get` to install missing
  dependencies: dpkg holds the lock during installation and apt aborts.
  Users resolve dependencies with `apt install ./<package>.deb` (README).
- The maintainer scripts `data/postinst`, `data/prerm` and `data/postrm`
  enable and start the service. Up to v0.9.2 they were missing, the service
  did not run after installation and the switch had no effect.
  `.gitattributes` enforces LF so they still work from a Windows checkout.
- `maintainer` and `homepage` in `nfpm.yaml` and the URLs in `configure.ac`
  still point to the upstream project (Tom Richards / pauLee). Do not change
  them without being asked.

## Release

- The `.deb` is built only in GitHub Actions (armhf cross build in a
  `debian:bookworm` container), not locally – development happens on
  Windows.
- Steps: bump the version in `nfpm.yaml` and `README.md` (download link,
  file names), commit, push, push an annotated tag `vX.Y.Z`. The `release`
  job in `.github/workflows/app.yml` creates the release if it does not
  exist and attaches the `.deb`. Set title and notes afterwards with
  `gh release edit`.
- Only the user can test daemon changes on the hardware (Raspberry Pi 2
  Model B). Publish such releases as a pre-release first and mark them
  "Latest" only after a successful hardware test.
- `configure` needs `--sysconfdir=/etc`. With `--prefix=/usr` alone,
  `SYSCONFDIR` becomes `/usr/etc`, the daemon does not find
  `/etc/mausberry-switch.conf` and silently runs with the defaults (bug up
  to v0.9.2). The CI step "Check config path" checks this in the binary.
- The version in `AC_INIT` (`configure.ac`) is not bumped; `nfpm.yaml` is
  authoritative for the package.

## Git

- Commit with the noreply address `592889+tomcek42@users.noreply.github.com`
  (set as `user.email` in the repo). GitHub rejects pushes with the private
  address because of email privacy protection (GH007).
- No `Co-Authored-By: Claude …` lines and no Claude mentions in commits,
  tags or releases.
- Committing directly to `master` is the norm here.

## Daemon

- Deliberately timer-free: the process sleeps in `poll()` on the GPIO event
  fd and a `signalfd`. Do not reintroduce timeouts for polling a `running`
  flag.
- The Mausberry board keeps OUT high once turned off, even if the switch is
  turned back on (measured on the Pi). Cancelling during `Delay` is
  therefore impossible; the re-check afterwards only filters glitches.
- The line level is evaluated, not single edges. This is the only way to
  detect a switch that is already off at startup.
- After a successful shutdown command the daemon waits for SIGTERM instead
  of exiting, so the IN line stays high until the end. The Raspberry Pi
  kernel keeps released outputs at their level by default
  (`pinctrl_bcm2835.persist_gpio_outputs`), but the daemon should not rely
  on that.

## CI pitfalls

- The `lint` job checks `src/*.c` with `clang-format --dry-run --Werror`
  (LLVM default style). Format before committing, or CI fails.
