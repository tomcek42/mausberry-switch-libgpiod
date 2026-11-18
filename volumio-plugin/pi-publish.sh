#!/bin/bash
#
# Test and submit the mausberry_switch Volumio plugin from a Raspberry Pi
# running Volumio 4. Run as user "volumio" (not root):
#
#   bash pi-publish.sh
#
# The script pauses whenever a manual step (web UI, GitHub) is needed.
# It can be re-run; existing clones are updated instead of re-created.

set -euo pipefail

GH_USER="tomcek42"
GH_EMAIL="592889+tomcek42@users.noreply.github.com"
PLUGIN="mausberry_switch"
SRC_REPO="https://github.com/${GH_USER}/mausberry-switch-libgpiod.git"
FORK_REPO="https://github.com/${GH_USER}/volumio-plugins-sources-bookworm.git"
UPSTREAM_REPO="https://github.com/volumio/volumio-plugins-sources-bookworm.git"
WORK="${HOME}/plugin-work"
SRC_DIR="${WORK}/mausberry-switch-libgpiod"
FORK_DIR="${WORK}/volumio-plugins-sources-bookworm"
PLUGIN_DIR="${FORK_DIR}/${PLUGIN}"
CONF="/etc/mausberry-switch.conf"

step()  { echo; echo "=== $* ==="; }
ok()    { echo "  [OK]   $*"; }
fail()  { echo "  [FAIL] $*"; FAILED=1; }
pause() { echo; read -r -p ">>> $* -- press Enter to continue (Ctrl+C aborts) " _; }
ask()   { local a; read -r -p ">>> $* [y/N] " a; [[ "$a" =~ ^[yYjJ]$ ]]; }

FAILED=0

[ "$(id -un)" = "root" ] && { echo "Run as user volumio, not root."; exit 1; }
command -v volumio >/dev/null || { echo "'volumio' command not found - is this Volumio?"; exit 1; }

# --- 1. Prerequisites ------------------------------------------------------
step "1. Prerequisites"
if ! command -v git >/dev/null; then
  sudo apt-get update && sudo apt-get install -y git
fi
ok "git $(git --version | awk '{print $3}')"

if dpkg -s mausberry-switch >/dev/null 2>&1; then
  echo "  mausberry-switch is already installed (manual install?)."
  if ask "Purge it to test a clean first installation?"; then
    sudo apt-get purge -y mausberry-switch
  fi
fi

if [ -d /data/plugins/system_controller/$PLUGIN ]; then
  echo "  Plugin 1.0.0 is installed under the old category system_controller."
  pause "UNINSTALL it in the web UI first"
fi

# --- 2. Clone / update repositories ----------------------------------------
step "2. Repositories"
mkdir -p "$WORK"

if [ -d "$SRC_DIR/.git" ]; then
  git -C "$SRC_DIR" pull --ff-only
else
  git clone --depth=1 "$SRC_REPO" "$SRC_DIR"
fi
ok "plugin source: $(git -C "$SRC_DIR" log -1 --format='%h %s')"

if [ -d "$FORK_DIR/.git" ]; then
  ok "fork already cloned"
else
  if ! git ls-remote "$FORK_REPO" >/dev/null 2>&1; then
    echo "  Fork ${FORK_REPO} does not exist."
    pause "Fork ${UPSTREAM_REPO} on GitHub now"
  fi
  git clone --depth=1 "$FORK_REPO" "$FORK_DIR"
fi
cd "$FORK_DIR"
git config user.name "$GH_USER"
git config user.email "$GH_EMAIL"
git checkout "$PLUGIN" 2>/dev/null || git checkout -b "$PLUGIN"

rm -rf "$PLUGIN_DIR"
cp -r "$SRC_DIR/volumio-plugin/$PLUGIN" "$PLUGIN_DIR"
chmod +x "$PLUGIN_DIR/install.sh" "$PLUGIN_DIR/uninstall.sh"
if grep -q $'\r' "$PLUGIN_DIR"/*.sh; then fail "CRLF in shell scripts"; exit 1; fi
ok "plugin copied to $PLUGIN_DIR"

VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$PLUGIN_DIR/package.json" | head -1)
ok "plugin version $VERSION"

# --- 3. Install ------------------------------------------------------------
step "3. volumio plugin install"
cd "$PLUGIN_DIR"
volumio plugin install | tee "$WORK/install.log"
# Volumio swallows the "plugininstallend" line (end marker), so check its own success message.
grep -q "Successfully Installed" "$WORK/install.log" && ok "Volumio reports successful install" || fail "Volumio did not report a successful install"

dpkg -s mausberry-switch 2>/dev/null | grep -q '^Status: install ok installed' \
  && ok "package installed: $(dpkg-query -W -f='${Version}' mausberry-switch)" \
  || fail "package mausberry-switch not installed"
dpkg -s libgpiod2 >/dev/null 2>&1 && ok "libgpiod2 installed" || fail "libgpiod2 missing"
# The daemon must not run before the plugin is enabled (Volumio checklist).
systemctl is-active --quiet mausberry-switch && fail "service active before the plugin is enabled" || ok "service not active yet"
systemctl is-enabled --quiet mausberry-switch && fail "service enabled before the plugin is enabled" || ok "service not enabled yet"
[ -d /data/plugins/system_hardware/$PLUGIN ] && ok "plugin dir in /data/plugins" || fail "plugin dir missing"

# --- 4. sudo check (used by index.js) --------------------------------------
step "4. Passwordless sudo"
sudo -n true 2>/dev/null && ok "sudo without password works" || fail "sudo needs a password - index.js cannot save"

# --- 5. Manual UI tests ----------------------------------------------------
step "5. Web UI tests (manual)"
cp "$CONF" "$WORK/conf.before"
cat <<EOF
  In the Volumio web UI (Settings -> Plugins -> Installed plugins):
    a) Enable "Mausberry Switch" if it is off.
    b) Open its settings: values must be Out 23 / In 24 / Delay 0
       (or whatever is in $CONF).
    c) Try invalid values, each must show an error toast:
       pin 1, pin 30, Out = In, Delay -1, Delay 5000.
    d) Set Delay to 5 and save.
EOF
pause "Do a) to d) now"

grep -q '^Delay=5$' "$CONF" && ok "Delay=5 written" || fail "Delay=5 not found in $CONF"
diff <(grep -v '^Delay=' "$WORK/conf.before") <(grep -v '^Delay=' "$CONF") >/dev/null \
  && ok "rest of the file (comments, ShutdownCommand) unchanged" \
  || fail "other lines changed: see diff $WORK/conf.before $CONF"
echo "  Service started: $(systemctl show -p ActiveEnterTimestamp --value mausberry-switch)"

pause "Set Delay back to the old value and save, then DISABLE the plugin"
systemctl is-active --quiet mausberry-switch && fail "service still active after disable" || ok "service stopped"
systemctl is-enabled --quiet mausberry-switch && fail "service still enabled" || ok "service disabled"

pause "ENABLE the plugin again"
systemctl is-active --quiet mausberry-switch && ok "service active" || fail "service not active"
systemctl is-enabled --quiet mausberry-switch && ok "service enabled" || fail "service not enabled"

if [ "$FAILED" -ne 0 ]; then
  echo
  echo "Some checks failed. Logs for the fix:"
  echo "  journalctl -u mausberry-switch -n 50 --no-pager"
  echo "  journalctl -n 300 --no-pager | grep -i -A3 mausberry   (or /var/log/volumio.log)"
  ask "Continue anyway?" || exit 1
  FAILED=0
fi

# --- 6. Uninstall ----------------------------------------------------------
step "6. Uninstall"
pause "UNINSTALL the plugin in the web UI"
dpkg -s mausberry-switch >/dev/null 2>&1 && fail "package still installed" || ok "package removed"
[ -e "$CONF" ] && fail "$CONF still exists" || ok "$CONF purged"
rm -rf "$PLUGIN_DIR/node_modules"
ok "node_modules removed"

[ "$FAILED" -ne 0 ] && { ask "Uninstall checks failed. Continue anyway?" || exit 1; }

# --- 7. Commit and push ----------------------------------------------------
step "7. Commit and push"
cd "$FORK_DIR"
git add "$PLUGIN"
echo "  Changes to be committed:"
git status --short
if git diff --cached --quiet; then
  ok "nothing to commit (already committed)"
else
  ask "Commit as '$PLUGIN - release $VERSION'?" || exit 1
  git commit -m "$PLUGIN - release $VERSION"
fi
echo "  GitHub asks for username ($GH_USER) and a Personal Access Token as password."
git push -u origin "$PLUGIN"

# --- 8. Pull request -------------------------------------------------------
step "8. Pull request (manual, on the PC)"
echo "  Open: https://github.com/${GH_USER}/volumio-plugins-sources-bookworm/pull/new/${PLUGIN}"
echo "  Base: volumio/volumio-plugins-sources-bookworm, default branch"
echo "  Title: $PLUGIN - release $VERSION"
pause "Create the pull request"

# --- 9. Submit -------------------------------------------------------------
step "9. volumio plugin submit"
cd "$PLUGIN_DIR"
ask "Submit $PLUGIN $VERSION to the Volumio plugin store now?" || exit 0
volumio plugin submit

echo
echo "Done. Enable 'Plugin Test Mode' at http://volumio.local/dev to see the beta in the store."
