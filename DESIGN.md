# ChronoDrive: design notes (client side)

The server side (drives on node disks with the `drive` role, the drive service, grants, backups and mirrors) is
designed and documented in the Miniserver repo: `docs/chronodrive.md` and `API.md` §17. This file is about the
Windows client. Built 2026-10-28 (v0.1.0).

## Pieces

| File | What |
|---|---|
| `msclient.hpp` | Paths, config (`config.json`), secrets (DPAPI), log; `Http()` on WinHTTP: JSON calls, bodies streamed from a file slice (chunks), responses streamed to a file (downloads with `Range`), and **certificate pinning** for home nodes (the SHA-256 of the node's certificate the Miniserver handed out, checked in the `SENDING_REQUEST` callback before any body goes); `Miniserver` (device token, primary then standby on transport failures, pairing); `DriveClient` (the drive service's API). |
| `sync.hpp` | `Index` (what this PC last saw of every path, with the drive's rev as the base rev of the next write), the folder scanner (Windows-safe, ignores), `Engine` (the worker loop: connect, remote pass, local pass; a watcher thread on `ReadDirectoryChangesW`; a poller thread on the change feed's long poll). |
| `main.cpp` | The tray app: one ChronoUI window (account, drive, folder, status, recent changes, log), the tray menu, pairing driven from the UI thread with async calls, autostart (HKCU Run). |
| `cli.cpp` | `cdrive.exe`: the same engine from a console. `cdrive sync --seconds N` is what the end-to-end test drives (`CHRONODRIVE_DATA` gives each "PC" its own data folder). |

## Decisions

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

## Tests

- `tests/drive_service_test.py` in the Miniserver repo exercises the service alone.
- The end-to-end run (two data folders paired as two devices, a drive on the office node, files both ways, a
  conflict, a delete) is a script in this session's notes; it only needs `cdrive.exe`, the superuser of the
  Miniserver and a throwaway user. Worth moving into `tools/` as `e2e.py` once a second office drive is cheap.
