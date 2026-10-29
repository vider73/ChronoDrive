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

1. **Pair this PC**: press "Pair this PC"; a code opens in the browser, approve it with your account. (The server
   address is the default; `MINISERVER_URL` wins over it.) The device key is kept with DPAPI.
2. **Show a drive on this PC**: every drive of your account is a card. "Show on this PC" puts it here
   **as its own drive letter** (`Z:`, with its name in Explorer; the files are kept in
   `%USERPROFILE%\ChronoDrive\<name>`) **or in a folder** you pick. Each drive on its own: show both, or one.
3. That's it: the card says where it is and whether it is up to date; Open / Pause / Remove from PC. New drives are
   created on the web ("New drive…" opens `/chronodrive/` on the Miniserver). "Start with Windows" puts it in the tray
   at logon, and it defines the drive letters again then (a letter lasts until you sign out).

Same thing from a console:

```powershell
cdrive pair
cdrive drives
cdrive use <drive id> --letter Z          # or: --folder D:\Drive\Documents
cdrive sync            # every drive shown here, until Ctrl+C
cdrive remove <drive id>                  # off this PC; the files stay
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

## Updates

ChronoDrive updates itself, like ChronoRemote: a minute after it starts and every 6 hours it reads
`<server>/chronodrive/windows.json`; when a newer version is published it downloads the setup with this PC's device
token, checks the SHA-256 the server announced and ChronoScan's code signature, waits until every drive is up to date,
and runs the setup silently (`/UPDATEFROM=app`): the setup closes the app, installs over it and starts it again in the
tray ("Updated to 0.2.x" at the bottom of the window). No administrator rights. The tray menu has "Check for updates"
(and "Install version x now" once one is downloaded); `cdrive update [--install]` does the same from a console.
Publishing a version: `toolsuild_setup.ps1` then `tools\publish_setup.ps1 -Notes "..."`.

## Files on this PC

| Path | What |
|---|---|
| `%LOCALAPPDATA%\ChronoDrive\config.json` | server, account, device, and the drives shown here (letter or folder, paused) |
| `secrets.bin` | the device secret (DPAPI, this user) |
| `index-<drive id>.json` | the sync index of each drive (forgotten when the drive is removed from this PC) |
| `logs\chronodrive.log` | what happened (rotated at 2 MB) |
| `updates\` | the last setup downloaded by the updater (older ones are deleted at start) |

`CHRONODRIVE_DATA=<folder>` moves all of that (two instances on one PC, tests).
