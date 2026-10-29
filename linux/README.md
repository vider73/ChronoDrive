# ChronoDrive for Linux

Keeps folders of this PC in two-way sync with your **drives** on the Miniserver: folders of a fixed size that belong
to your account and live on a Miniserver node (the office node, or your own home node). The same drive can be on
Windows PCs (the ChronoDrive tray app) and Linux PCs at the same time.

A console program and a background service. Python 3.8+ standard library only: nothing to `pip install`.

## Install (from git)

```bash
git clone <REPO_URL> ChronoDrive      # placeholder: the repository has no public remote yet
cd ChronoDrive
./linux/install.sh
```

No root needed. The installer copies the program to `~/.local/share/chronodrive`, puts the command in
`~/.local/bin/chronodrive`, and enables a `systemd --user` service (`chronodrive.service`) that syncs in the
background. It also runs `loginctl enable-linger` so the service starts at boot, before you log in (`--no-linger`
skips that; without it the service starts when you log in). If `~/.local/bin` is not in your `PATH`, the installer
says so.

Keep the checkout: `chronodrive update` pulls it and reinstalls.

## Use

```bash
chronodrive pair                     # shows a code and a QR: scan it with your phone (or open the link), Approve
chronodrive drives                   # your drives: size, used, node, reachable from here, where mounted
chronodrive mount Documents          # syncs it in ~/ChronoDrive/Documents
chronodrive mount Fotos ~/Pictures/Fotos   # or in a folder you pick
chronodrive status                   # service, each drive: up to date?, last sync, files, what is not synced
```

That's all: the service keeps every mounted drive in sync from then on. Other commands:

| Command | What |
|---|---|
| `chronodrive` | a menu with all of this |
| `chronodrive unmount <drive>` | stop syncing it here; the files stay in the folder |
| `chronodrive sync [--once] [drive]` | sync in the foreground (`--once`: one pass and exit). For drives the service already syncs it just asks the service to sync now |
| `chronodrive log [n]` | the last lines of the log (the service also logs to the journal: `journalctl --user -u chronodrive`) |
| `chronodrive unlink` | forget the account on this PC (the files stay) |
| `chronodrive update` | `git pull` in the checkout it was installed from, reinstall, restart the service |

`<drive>` is the drive's name, its id, or a unique start of either. New drives are created on the web:
`<server>/chronodrive/`.

Server: `MINISERVER_URL` (environment) > the one saved by `chronodrive pair --url ...` > `https://cs-miniserver.chronoscan.org`.
If the primary does not answer, the client retries on the standby; while the Miniserver is down a drive keeps
syncing with its node on the grant it already has (valid 12 hours).

## How it syncs

Exactly like the Windows client, so both can share a drive:

- The client asks the Miniserver for a 12-hour **grant** and the drive's addresses, and talks to the node directly:
  the office node from anywhere, a home node only on its own network (or your VPN), trusting only that node's own
  certificate (the one the Miniserver hands out). Away from a home drive's network the drive waits ("not reachable").
- `~/.local/state/chronodrive/index-<drive>.json` is what this PC last saw of each file (size, mtime, SHA-256, the
  drive's revision). The drive's change feed (long poll) brings remote changes; inotify on the folder plus a full
  scan every 10 minutes finds local ones. Files up to 8 MiB go in one request; bigger ones in 8 MiB chunks, resumed
  after a cut.
- A remote change over an untouched local file wins; a local change over an untouched remote file wins; both
  changed = the drive's version keeps the name and yours is kept as `name (conflict <PC> <date>).ext` and uploaded.
- A file deleted elsewhere goes to this PC's **trash** (`~/.local/share/Trash`, restorable from the file manager); a
  file deleted here is deleted on the drive (the node keeps it in its trash for 30 days).

**Windows names.** The drive keeps Windows-safe names, so a Linux file is **skipped and reported** (in
`chronodrive status` and the log), never sent, when its name has `\ : * ? " < > |` or control characters, is a
Windows reserved name (`CON`, `NUL`, `COM1`...), ends in a space or a dot, or is not UTF-8. Rename it and it syncs.
Names that differ only by case (`Notes.txt` and `notes.txt`) are one file on the drive: the one already synced (else
the first by name) syncs, the other is reported.

Never synced: symbolic links, sockets and pipes (reported), and temporary files: `*.tmp`, `*.part`, `*.swp`,
`~$*`, `.~lock.*`, `.goutputstream-*`, `desktop.ini`, `Thumbs.db`, `.DS_Store`. A file written less than 2 seconds
ago waits for the next pass.

## Not supported

No files on demand (every file is a real local copy), no selective sync, no file permissions, owners or extended
attributes (only content and modification time), no symbolic links, no sharing a drive between accounts, no
client-side encryption, no version history in the client (the drive's backup on the node is the history), no
bandwidth limits. A drive is a folder, not a block device: "mount" means "keep this folder in sync".

## Files

| Path | What |
|---|---|
| `~/.config/chronodrive/config.json` | server, account, device, the drives mounted here |
| `~/.config/chronodrive/secret` | the device secret (mode 0600) |
| `~/.local/state/chronodrive/` | `index-<drive>.json`, `status-<drive>.json`, `chronodrive.log`, `servers.json` (cached server list) |
| `~/.local/share/chronodrive/` | the installed program (`SOURCE` = the checkout it came from) |
| `~/.config/systemd/user/chronodrive.service` | the background service |

`XDG_CONFIG_HOME`, `XDG_STATE_HOME` and `XDG_DATA_HOME` move them (tests, two instances).
`CHRONODRIVE_NODE_URL=https://host:port` reaches a home node at another address (a VPN); its certificate is still checked.

## Uninstall

```bash
~/.local/share/chronodrive/uninstall.sh            # program and service; keeps the account and the sync state
~/.local/share/chronodrive/uninstall.sh --purge    # also forgets the account and the state
```

Your synced folders are never deleted.

## Development

- Code: `chronodrive` (entry point) and `cdrive/`: `net.py` (HTTP with pinned certificates, the Miniserver,
  the drive service), `sync.py` (index and engine), `local.py` (scan, Windows-name rules, trash), `qr.py`
  (terminal QR), `cli.py` (commands, menu, service).
- Contract: the Miniserver's `pb_public/API.md` sections 2 (pairing), 11 (standby, nearby, home nodes) and 17 (drives).
- End-to-end test with a Windows PC on the same drive (run on Windows, uses WSL): `python linux\tools\e2e_wsl.py [node] [--installed]`.
