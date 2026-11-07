# CLAUDE.md

Zielsystem ist ein Raspberry Pi mit Volumio 4 (Debian 12 Bookworm, armhf).

## Paketbau (nfpm)

- nfpm übernimmt `depends` in `nfpm.yaml` wörtlich ins `control`-File.
  Versionsangaben müssen in Debian-Syntax mit Klammern stehen:
  `libgpiod2 (>= 1.0)`. Ohne Klammern lehnt das dpkg von Bookworm das
  Paket mit einem Parse-Fehler ab (älteres dpkg unter Volumio 3 hat es
  noch geschluckt) – genau das war der Bug in v0.9.1.
- Keine `preinst`/`postinst`-Skripte, die `apt-get` aufrufen, um
  Abhängigkeiten nachzuinstallieren: dpkg hält während der Installation
  den Lock, apt bricht ab. Abhängigkeiten löst der Nutzer mit
  `apt install ./<paket>.deb` auf (README).
- Die Maintainer-Skripte `data/postinst`, `data/prerm` und `data/postrm`
  aktivieren und starten den Dienst. Bis v0.9.2 fehlten sie, der Dienst lief
  nach der Installation nicht und der Schalter war wirkungslos.
  `.gitattributes` erzwingt LF, damit sie auch aus einem Windows-Checkout
  heraus lauffähig bleiben.
- `maintainer` und `homepage` in `nfpm.yaml` sowie die URLs in
  `configure.ac` verweisen noch auf das Upstream-Projekt
  (Tom Richards / pauLee). Nicht ungefragt ändern.

## Release

- Das `.deb` wird nur in GitHub Actions gebaut (Cross-Build für armhf im
  `debian:bookworm`-Container), nicht lokal – Entwicklung läuft auf Windows.
- Ablauf: Version in `nfpm.yaml` und `README.md` (Download-Link,
  Dateinamen) erhöhen, committen, pushen, annotiertes Tag `vX.Y.Z` pushen.
  Der Job `release` in `.github/workflows/app.yml` legt das Release an,
  falls es fehlt, und hängt das `.deb` an. Titel und Notes danach mit
  `gh release edit` setzen.
- Änderungen am Daemon kann nur der Nutzer auf der Hardware testen
  (Raspberry Pi 2 Model B). Solche Releases zuerst als Pre-Release
  veröffentlichen und erst nach erfolgreichem Hardware-Test auf „Latest“
  setzen.
- `configure` braucht `--sysconfdir=/etc`. Mit `--prefix=/usr` allein wird
  `SYSCONFDIR` zu `/usr/etc`, der Daemon findet `/etc/mausberry-switch.conf`
  nicht und läuft still mit Standardwerten (Fehler bis v0.9.3). Der
  CI-Schritt „Check config path“ prüft das im Binary.
- Die Version in `AC_INIT` (`configure.ac`) wird nicht mitgezogen; maßgeblich
  für das Paket ist `nfpm.yaml`.
- Release-Notes auf Deutsch (Gliederung wie in v0.9.1: „Was ist neu“,
  „Installation“, „Konfiguration“), README und Commit-Messages auf Englisch.

## Git

- Commits mit der noreply-Adresse `592889+tomcek42@users.noreply.github.com`
  (im Repo als `user.email` gesetzt). Pushes mit der privaten Adresse lehnt
  GitHub wegen des E-Mail-Schutzes ab (GH007).
- Keine `Co-Authored-By: Claude …`-Zeilen und keine Claude-Hinweise in
  Commits, Tags oder Releases.
- Direkt auf `master` committen ist hier üblich.

## Daemon

- Absichtlich ohne Timer: Der Prozess schläft in `poll()` auf dem
  GPIO-Event-fd und einem `signalfd`. Keine Timeouts zum Abfragen eines
  `running`-Flags wieder einbauen.
- Ausgewertet wird der Pegel der Leitung, nicht einzelne Flanken. Nur so wird
  ein beim Start bereits ausgeschalteter Schalter erkannt.
- Nach erfolgreichem Shutdown-Befehl wartet der Daemon auf SIGTERM, statt
  sich zu beenden, damit die IN-Leitung bis zuletzt high bleibt. Der
  Raspberry-Pi-Kernel lässt freigegebene Ausgänge zwar standardmäßig auf
  ihrem Pegel (`pinctrl_bcm2835.persist_gpio_outputs`), darauf soll sich der
  Daemon aber nicht verlassen.

## CI-Fallstricke

- Der Job `lint` prüft `src/*.c` mit `clang-format --dry-run --Werror`
  (LLVM-Defaultstil). Vor dem Commit formatieren, sonst schlägt CI fehl.
