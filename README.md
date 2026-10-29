# ChronoDrive

A Windows tray app that keeps a local folder in sync with a **drive** on the Miniserver: a folder of a fixed size
that belongs to your account and lives on one of the Miniserver nodes (the office node, or your own home node).
Changes go up as you make them, other PCs' changes come down, conflicts are kept as copies, nothing is lost silently.

```
ChronoDrive.exe   the tray app (ChronoUI window for the setup, status, recent changes, log)
cdrive.exe        the console twin: pair, drives, create, use, sync, status, log (scripts and tests)
```

Server side and design: the Miniserver repo (`I:\LLMLab\Azure\miniserver`): `docs/chronodrive.md` (the plan and the
decisions), `pb_public/API.md` §17 (the contract this client follows), `tools/node/drive/drive_service.py` (what
runs on the node). Build: `.\build.ps1` (CMake, VS 2022 x64, header-only ChronoUI from
`M:\ChronoScan\v3\root\ChronoUI`, nlohmann/json vendored in `third_party/`). Installer: `installer/ChronoDrive.iss`
(Inno Setup; the version lives there and CMake reads it): `pwsh -File toolsuild_setup.ps1` builds and signs it
(AutoFirmaCS on `S:\`), `pwsh -File tools\publish_setup.ps1 -Notes "..."` puts it behind the account on the Miniserver
(`/chronodrive/#download`, the page where users also create and manage their drives).

## First run

1. **Pair this PC**: the Miniserver address (default `https://cs-miniserver.chronoscan.org`; the environment
   variable `MINISERVER_URL` wins over the saved one), then "Pair this PC": a code and the QR page open, approve it
   with your account (`/link/`). The device credentials are kept with DPAPI (`%LOCALAPPDATA%\ChronoDrive\secrets.bin`).
2. **Pick a drive** (or create one: name, size in GB, where: the nodes with a `drive` role you may use, with their
   free space; optional daily backup on the node).
3. **Pick the local folder**, then "Start syncing". The tray icon shows the state; right-click: open the folder,
   pause, the recent changes, exit. "Start with Windows" puts it in the tray at logon (`--tray`).

Same thing from a console:

```powershell
cdrive pair
cdrive drives
cdrive create --name Documents --node miniserver-h1 --size 50 --backup
cdrive use <drive id> --folder D:\Drive\Documents
cdrive sync            # until Ctrl+C; status every 2 s, then the recent changes
```

## How it syncs

- The drive is reachable at the node's own address: on the LAN of a home node (`https://<lan ip>:8443/drive/<id>`,
  the node's certificate pinned), from anywhere for the office node. The client asks the Miniserver for a 12-hour
  **grant** and the drive's addresses (`POST /api/ms/drive/drives/{id}/grant`), tries them in turn, and asks again
  every 10 minutes or whenever a call fails. A home drive syncs only on its network (or your own VPN): away from
  home the state is "Waiting: drive not reachable".
- `%LOCALAPPDATA%\ChronoDrive\index.json` is what this PC last saw of every file: size, mtime, SHA-256 and the drive's
  revision (`rev`). A **remote pass** reads the drive's change feed since that revision (long polls keep it near
  real time); a **local pass** compares the folder with the index (a watcher on the folder plus a full scan every
  10 minutes). Files up to 8 MiB go in one PUT; bigger ones in 8 MiB chunks, resumed after a cut.
- Rules: a remote change over an untouched local file wins; a local change over an untouched remote file wins;
  both changed = the drive's version keeps the name and the local one is kept as
  `name (conflict <PC> <date>).ext` and uploaded too. A file deleted elsewhere goes to this PC's Recycle Bin; a file
  deleted here is a tombstone on the drive (the node keeps it in its trash for 30 days).
- Ignored: `~$*`, `*.tmp`, `*.part`, `desktop.ini`, `Thumbs.db`, reparse points (OneDrive placeholders, links),
  hidden system files; a file held open for writing is retried later. Names must be Windows-safe
  (no `\ / : * ? " < > |`), which the drive enforces too.

Not in this version: selective sync, sharing a drive between accounts, a web file browser, client-side
encryption, a version history in the client (the drive's restic backup on the node is the history), file locking,
a relay for home drives from the Internet, bandwidth limits (see `docs/chronodrive.md` section 3.4 in the
Miniserver repo).

## Files on this PC

| Path | What |
|---|---|
| `%LOCALAPPDATA%\ChronoDrive\config.json` | server, account, device, drive, folder, paused |
| `secrets.bin` | the device secret (DPAPI, this user) |
| `index.json` | the sync index (deleted when the drive or the folder changes: a fresh start) |
| `logs\chronodrive.log` | what happened (rotated at 2 MB) |

`CHRONODRIVE_DATA=<folder>` moves all of that (two instances on one PC, tests).
