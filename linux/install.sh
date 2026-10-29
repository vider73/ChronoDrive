#!/usr/bin/env bash
# ChronoDrive for Linux: installs the client from this git checkout for the current user (no root needed).
#   ~/.local/share/chronodrive/     the program (a copy of linux/; SOURCE = the checkout, for `chronodrive update`)
#   ~/.local/bin/chronodrive        the command
#   ~/.config/systemd/user/chronodrive.service   the background sync, enabled and started
# Options: --no-linger (do not start the service at boot without a login), --update (used by `chronodrive update`).
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SRC/.." && pwd)"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/chronodrive"
BIN="$HOME/.local/bin"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
LINGER=1
UPDATE=0
for a in "$@"; do
  case "$a" in
    --no-linger) LINGER=0 ;;
    --update) UPDATE=1; LINGER=0 ;;
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

if ! command -v python3 >/dev/null || ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))'; then
  echo "ChronoDrive needs Python 3.8 or newer (sudo apt install python3)." >&2
  exit 1
fi

mkdir -p "$DEST" "$BIN" "$UNIT_DIR"
rm -rf "$DEST/cdrive.new"
cp -r "$SRC/cdrive" "$DEST/cdrive.new"
find "$DEST/cdrive.new" -name __pycache__ -prune -exec rm -rf {} +
rm -rf "$DEST/cdrive"
mv "$DEST/cdrive.new" "$DEST/cdrive"
install -m 0755 "$SRC/chronodrive" "$DEST/chronodrive"
install -m 0644 "$SRC/README.md" "$DEST/README.md"
install -m 0755 "$SRC/uninstall.sh" "$DEST/uninstall.sh"
printf '%s\n' "$REPO" > "$DEST/SOURCE"
ln -sfn "$DEST/chronodrive" "$BIN/chronodrive"
sed "s|@DEST@|$DEST|g" "$SRC/chronodrive.service" > "$UNIT_DIR/chronodrive.service"
VERSION="$(python3 "$DEST/chronodrive" --version)"
echo "ChronoDrive $VERSION installed in $DEST"

if systemctl --user show-environment >/dev/null 2>&1; then
  systemctl --user daemon-reload
  systemctl --user enable chronodrive.service >/dev/null 2>&1
  systemctl --user restart chronodrive.service
  echo "Background sync: chronodrive.service (systemd --user) enabled and running."
  if [ "$LINGER" = 1 ] && command -v loginctl >/dev/null; then
    if [ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" = "yes" ]; then
      :
    elif loginctl enable-linger "$USER" 2>/dev/null; then
      echo "It also starts at boot, before you log in (loginctl enable-linger)."
    else
      echo "It starts when you log in. To start it at boot too: sudo loginctl enable-linger $USER"
    fi
  fi
else
  echo "No systemd user session here: run 'chronodrive sync' yourself to keep the drives in sync."
fi

case ":$PATH:" in
  *":$BIN:"*) ;;
  *) echo "Note: $BIN is not in your PATH. Add it (e.g. in ~/.profile): export PATH=\"\$HOME/.local/bin:\$PATH\"" ;;
esac
if [ "$UPDATE" = 0 ]; then
  echo
  echo "Next: chronodrive pair    (then: chronodrive drives, chronodrive mount <drive>)"
fi
