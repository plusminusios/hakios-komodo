# Working on ios-tegu

- Read `NOTES.md` before starting; it contains the confirmed tegu hardware
  facts (register map, sources) and the open questions. `docs/reference-sweet-notes.md`
  is the ios-sweet bring-up history — the reference for how each subsystem
  was solved on the SM7150.
- Keep the active handoff in `NOTES.md` after each milestone and before
  stopping: commit hashes, CI run IDs, exact build/boot results, unfinished
  work. Distinguish confirmed facts from guesses; mark guesses *(verify)*.
- Commit completed changes. Use `gh` for GitHub Actions kernel builds and
  logs. `tools/xnu-cc.py` replays single compile commands locally before
  spending a CI run.
- Device bring-up uses temporary `fastboot boot` images only. Preserve the
  Android installation and all partition backups. Nothing is ever flashed to
  boot/recovery/vbmeta/modem/persist.
- **The owner has NOT approved erasing Android yet.** The iOS root image step
  (which writes the userdata partition) requires his explicit confirmation
  before it is ever run or scheduled. Do not assume prior approvals from the
  reference project (ios-sweet) carry over.
- Keep device dumps, serial numbers, IMEI/EID, and downloaded Google/Apple
  artifacts (OTA zips, IPSWs, DTBs) out of git. .gitignore already excludes them.
- tegu hardware facts must come from the stock firmware dumps or the live
  device (`/proc/device-tree`, dmesg, /proc/iomem). Never invent an address;
  if a value is unknown, read it from the FDT at runtime or mark it *(verify)*.
