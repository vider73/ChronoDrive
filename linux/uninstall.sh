#!/usr/bin/env bash
# ChronoDrive for Linux: removes the program, the command and the service. The synced folders are never touched.
#   --purge   also forget the account and the sync state (~/.config/chronodrive, ~/.local/state/chronodrive)
set -euo pipefail

DEST="${XDG_DATA_HOME:-$HOME/.local/share}/chronodrive"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/chronodrive.service"
PURGE=0
[ "${1:-}" = "--purge" ] && PURGE=1

if systemctl --user show-environment >/dev/null 2>&1; then
  systemctl --user disable --now chronodrive.service >/dev/null 2>&1 || true
fi
rm -f "$UNIT"
systemctl --user daemon-reload >/dev/null 2>&1 || true
if [ -L "$HOME/.local/bin/chronodrive" ]; then rm -f "$HOME/.local/bin/chronodrive"; fi
rm -rf "$DEST"
echo "ChronoDrive removed (the program and the service). Your synced folders stay as they are."

if [ "$PURGE" = 1 ]; then
  rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/chronodrive" "${XDG_STATE_HOME:-$HOME/.local/state}/chronodrive"
  echo "Account and sync state forgotten. (This PC still shows in the account's paired devices: revoke it on the web.)"
else
  echo "The account and sync state are kept in ~/.config/chronodrive and ~/.local/state/chronodrive (--purge removes them)."
fi
