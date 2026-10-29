# ChronoDrive: design notes (client side)

The server side (drives on node disks with the `drive` role, the drive service, grants, backups and mirrors) is
designed and documented in the Miniserver repo: `docs/chronodrive.md` and `API.md` §17. This file is about the
Windows client. Built 2026-10-28 (v0.1.0: one drive in a folder); 2026-10-29 v0.2.0: every drive of the account, each as a drive
letter or in a folder, after José found the single folder confusing.

## Pieces

| File | What |
|---|---|
| `msclient.hpp` | Paths, config (`config.json`), secrets (DPAPI), log; `Http()` on WinHTTP: JSON calls, bodies streamed from a file slice (chunks), responses streamed to a file (downloads with `Range`), and **certificate pinning** for home nodes (the SHA-256 of the node's certificate the Miniserver handed out, checked in the `SENDING_REQUEST` callback before any body goes); `Miniserver` (device token, primary then standby on transport failures, pairing); `DriveClient` (the drive service's API). |
| `sync.hpp` | `Index` (what this PC last saw of every path, with the drive's rev as the base rev of the next write), the folder scanner (Windows-safe, ignores), `Engine` (the worker loop: connect, remote pass, local pass; a watcher thread on `ReadDirectoryChangesW`; a poller thread on the change feed's long poll). |
| `drives.hpp` | `DriveSet`: one `Engine` per drive shown on this PC; drive letters (`DefineDosDevice`, what `subst` does, in the user's session, with the drive's name and icon under `HKCU\Software\Classes\Applications\Explorer.exe\Drives\<L>`), defined again at start; Remove from PC stops the engine, removes the letter and forgets the index (the files stay). |
| `main.cpp` | The tray app: one ChronoUI window with a card per drive of the account (Show on this PC as a letter or a folder, Open, Pause, Remove from PC), pairing, recent changes; the tray menu lists the drives. One instance per data folder (`CHRONODRIVE_DATA`); `--show <id>` opens a drive's panel. |
| `updater.hpp` | Self-update (the csLink scheme): `windows.json` → newer? → download with the device token (`/api/cdrive/download`) → SHA-256 + Authenticode thumbprint of ChronoScan's certificate → silent setup with `/UPDATEFROM=app`; the setup's `[Run]` starts the app again (`--tray --updated`). The app ends at once on `WM_ENDSESSION` (the setup's Restart Manager) instead of hiding in the tray. |
| `cli.cpp` | `cdrive.exe`: the same engine from a console. `cdrive sync --seconds N` is what the end-to-end test drives (`CHRONODRIVE_DATA` gives each "PC" its own data folder). |

## Decisions

- **Drive letters are `subst`, not a virtual file system.** The letter points at a real folder with real files, so
  every program works with it and nothing is fetched on demand. A placeholder drive like OneDrive's (Cloud Files API)
  or a WinFsp file system would save disk space but is a different, much larger project; it is the natural next step
  if drives get bigger than the PCs' disks.

- **No service.** One process per signed-in user, started at logon from the tray. The drive is the user's; nothing
  needs elevation. (ChronoBackup needs a service because it backs up without a signed-in user; a drive does not.)
- **Index, not a database.** `index.json` holds one entry per path; a few thousand files are fine. If drives grow to
  hundreds of thousands of files, move it to SQLite (the service already uses one).
- **Revisions decide conflicts.** Every write carries the base rev the client last saw; the service answers `412`
  when the drive moved on. The client never guesses which version is newer by time: the drive's version keeps the
  name, the local one becomes a conflict copy and goes up too. Same rule on the way down: a remote change over a
  locally modified file makes a conflict copy first.
- **Our own changes are not echoed.** After an upload the index stores the rev the service returned; the change
  feed later lists that rev and the engine skips it (`rev <= index rev` of the path).
- **Deletes are soft on both sides.** A remote delete sends the local file to the Recycle Bin; a local delete is a
  tombstone the node keeps in its trash for 30 days. A delete that races with a change loses: the file comes back.
- **Moves are not detected** (v1): a move is a delete plus an upload. The node's dedup is per file, so a moved big
  file is uploaded again. A later version can send `/move` when the scanner sees a vanished path and a new one
  with the same size and hash.
- **Reach.** Grants carry the drive's `urls`; the engine takes the first that answers `GET <url>` with the grant.
  A home node's LAN address needs the pinned certificate; the office node's public name uses the normal chain.
  Every failure makes the engine ask the Miniserver again (the node's address may have changed), with a backoff
  from 30 s to 5 min.

- **Sharing (0.3.0).** The owner shares a drive by email (Miniserver API.md section 17 "Sharing"); it shows up in
  the member's list with `owner: false`, `role` and `owner_email`. Read only is the node's job: the member's grant
  has a sixth part `r` and the drive service refuses writes with it. The engine sees the six-part grant and runs
  `KeepReadOnly` instead of `PushLocal`: nothing goes up, files deleted locally come back, files new or changed
  locally stay on the PC (counted in the status) and turn into conflict copies when the drive changes them.

## Tests

- `tests/drive_service_test.py` in the Miniserver repo exercises the service alone.
- `tools/e2e.py [node]`: two data folders paired as two devices, a drive on the node, files both ways, a conflict,
  a delete, then the drive shared with a second throwaway account (read only, then editable, then it leaves). It
  needs `build\Release\cdrive.exe` and the Miniserver's `.secrets.env`.
