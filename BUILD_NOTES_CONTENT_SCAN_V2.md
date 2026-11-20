# AmiTool NFC content scan v2

## Purpose
Restores NFC file parsing for the Saved screen, after the Storage List v1 test was confirmed to work.

## Behavior
- Scans a dedicated folder, `AMI_TOOL_SAVED_FOLDER` (`/ext/nfc/amiibo`), instead of the whole
  `/ext/nfc` tree. Generic dumps (FeliCa, MIFARE Classic, ISO15693, ...) are no longer listed
  and no longer interfere with the Amiibo list.
- Scans the folder and one level of its category subfolders
  (e.g. `/ext/nfc/amiibo/Super Mario Amiibo/mario.nfc`), creates the folder on first use.
- Collects up to `AMI_TOOL_SAVED_MAX_PAGE_ITEMS` (12) `.nfc` files, sorted by path so the
  first 12 entries are stable across visits.
- Loads each candidate with `nfc_device_load()`.
- Checks `nfc_device_get_protocol()` **before** calling `nfc_device_get_data()`: the NFC API
  crashes (hard fault) when the requested protocol is not part of the loaded instance's
  protocol hierarchy.
- Extracts the Amiibo ID from NTAG215 pages 21 and 22 using `ami_tool_extract_amiibo_id()`.
- Resolves all names with a **single pass** over `amiibo.dat`
  (`ami_tool_info_resolve_names()`), instead of one full 121 KB scan per file.
- Displays recognized names in the list; files without an Amiibo ID stay visible with an
  `[NFC]` prefix.
- Selecting an item shows the recognized ID/name or a clear unsupported/unrecognized message.

## Known timing (measured on hardware, F7, 12 files)
| Phase | Time |
|---|---|
| Directory scan | ~364 ms |
| 12 × `nfc_device_load()` | ~1112 ms |
| Name resolution (single pass) | ~234 ms |
| Menu build | ~1 ms |
| **Total blocking time on scene entry** | **~1.7 s** |

The work is synchronous on the GUI thread, so the screen is unresponsive for that period.
Further improvement would require loading dumps lazily (only on selection) or a worker thread.

## Resource notes
- `application.fam` `stack_size` is **8 KiB**. The Saved entry path chains
  `on_enter -> nfc_device_load -> protocol data -> asset lookup`; at 4 KiB this overflowed
  intermittently (observed as an occasional hard fault around the 7th file).
- `saved_files` is a 12-entry static array (~9.5 KiB BSS).

## Still not implemented
- Copying the parsed dump into the app's active tag state.
- Amiibo detail/action screen.
- Emulation, writing, UID changes, App Data editing.
- Any pagination beyond the first 12 files.

## Validation
- Compiles with `ufbt` (Flipper SDK API 87.1, target f7) with `-Werror` clean.
- Deployed and exercised on hardware: entering Saved with 12 Amiibo dumps no longer crashes,
  opens the list, and item selection shows the recognized Amiibo.

---

# Phase 1 - Amiibo library (in progress)

New module: `helpers/ami_tool_library.c`.

- Recursive scan of `/ext/nfc/amiibo` (depth 3) into a **disk index**, not into RAM, so the
  whole collection is reachable (verified: 1034 files, 87 pages).
- Index files live in `/ext/nfc/amiibo/.ami_tool/`:
  - `idx`  - one relative path per line, in scan order
  - `meta` - total file count
  - `ids`  - append-only `id<TAB>name<TAB>series<TAB>path` cache
- A page (12 entries) is loaded by seeking the index; only those 12 entries are in RAM.
- Entering the library parses no dump at all; the name of a file appears once its ID is known.
- "Index this page" indexes the 12 dumps of the current page; **"Index ALL files"** runs the
  same work for the whole library in a background worker thread with a live "n / total"
  progress screen (Back cancels, partial results are kept).
- id cache line format: `<index>\t<id>\t<name>\t<series>\t<relative path>`; entries are written
  in index order, and already-indexed entries are skipped on the next run.
- Measured on hardware: 1034 indexed files, full pass ~2 min, 946 files resolved to
  ID+name+series, 88 files without a usable Amiibo ID (kept, shown as `[NFC] path`).

- **By series** view: the `ids` cache is streamed once to build an alphabetical series list
  with counts (verified: 941 records, 29 series, Animal Crossing 525 / Smash 95 /
  Mario Sports Superstars 90 / Street Fighter 6 63). Selecting a series opens a filtered,
  paginated list of its Amiibo; entries can be opened and operated on as usual.
- **Favorites**: action menu entry "Favorite (toggle)" stores Amiibo IDs in
  `.ami_tool/fav`; the "Favorites" library view filters by that list.
- **Search**: "Search >" opens a TextInput, matching name / series / path of cached entries
  case-insensitively (verified: query `kirby` returns 5 hits).
- **Auto-rescan**: `meta` stores the library folder timestamp; if the folder changes, the index
  is rebuilt on the next entry. Cached ids are validated by path, so a replaced library cannot
  mislabel files.
- Navigation: Back steps Series items -> Series list -> Library -> Main menu.

## Known limits of this iteration
- Every page turn or series/search switch streams the whole `ids` file (~100 KB at 941 records),
  costing roughly 0.2-0.4 s.
- Search matches name, series and path of already-indexed files only.
- Index order is filesystem scan order, not alphabetical.
- Back inside the search TextInput currently exits the app instead of returning to the list.

---

# Phase 2 - data editor (in progress)

- The Amiibo retail key is loaded from `/ext/apps_data/ami_tool/key_retail.bin` (160 bytes).
- New action **"Verify dump"**: derives the data/tag keys from the opened dump, decrypts a copy,
  and validates both HMAC signatures. Result is shown on screen (VALID / INVALID / ERROR).
- Verified on hardware with a real key and a library dump:
  `0282000101D60502 -> VALID`, which proves key loading, key derivation, AES-CTR decryption
  and HMAC validation all work end to end.
- Next: Owner/Mii editing, re-signing, and "save as new file, keep original".

## Nickname edit pipeline (verified end to end)

- Action **"Set nickname (test)"** (hardcoded "AmiTool" until the text input UI lands):
  decrypt a copy -> set the setup flag bit4 at settings+0x0 -> write the name as UTF-16BE at
  settings+0xC (20 bytes) -> re-sign tag/data HMACs -> re-encrypt -> save to
  `/ext/nfc/amiibo/edited_<id>.nfc`. The original file is never modified.
- Offsets follow the 3dbrew Amiibo plaintext layout with the settings block at raw 0xA0.
- Self-check on hardware: the written file was read back, decrypted and validated ->
  `edit:VALID`, i.e. the modified dump keeps a consistent signature.
- Caveat: the settings/nickname offsets come from documentation, not from a dump with a known
  owner name (the library dumps have randomised settings), so on-console acceptance is not
  proven yet.
- **Nickname editing uses a real text input** ("Set nickname" -> TextInput, ASCII, max 10
  chars); after saving, the result screen also reports the re-signed signature status.
- **"Owner Mii (hex)"** shows the 0x60-byte owner Mii block (settings+0x20) for inspection.
- Verified on hardware: deleting the output and repeating the flow recreates
  `edited_0282000101D60502.nfc`, so the TextInput -> edit -> sign -> save path is wired.
- Round trip proven on hardware: the edited dump, loaded back and decoded through the settings
  view, reports `Flags: 0xB7` (original 0xA7 with the setup bit set) and `Nickname: "AmiTool"`.
- **"Show settings"** decodes flags, country, setup/last-write dates, write counter and the
  nickname (UTF-16BE) from any opened dump.
- **"Backup dump"** copies the opened file to `.ami_tool/backup/<tick>_<name>.nfc`
  (verified: `4702811_[AC] AF1 - Stitches.nfc`, 3519 bytes).
- **"Restore latest backup"** copies the newest backup back into the library as
  `restored_<name>.nfc` (verified: `restored_4702811_[AC] AF1 - Stitches.nfc`, 3519 bytes).
  The original file is never overwritten.
- **"App Data (hex)"** (phase 3 groundwork): decrypts and shows the game area
  (0x150-0x207, 184 bytes on the tag) as hex + ASCII, plus the AppID with a name table
  (Smash 0x10110E00, AC:HHD 0x0014F000, Chibi-Robo 0x00152600, Zelda TP HD 0x1019C800)
  and the AppData write counter.
- Per-game parsers (Smash FP first) need a real game-written dump as ground truth; the
  library dumps available so far have uninitialised AppData (random AppID).
- **Smash Bros. FP parser** (AppID 0x10110E00), inside the App Data view:
  version (u32 @0x0), fighter appearance (@0x8), the four specials (@0x9-0xC, shown 1-based),
  bonus effects (@0xD-0x10, FF = none), Atk/Def/Spd (s16 LE @0x11/0x13/0x15) and the raw
  level dword (@0x7C).
- Verified end to end without a retail FP dump: a synthetic dump was written through the real
  crypto path, saved, reloaded and parsed -> `Fighter 5 / Specials 2/3/4/5 / Effects FF FF FF FF /
  Atk/Def/Spd 14/20/30 / Level raw 0000002A`.
- Note: the block above is the **3DS/Wii U** SSB AppData layout. SSBU (Switch) uses a different
  layout, implemented separately:
  - data block at `0xE0` (0xD4 bytes), its own CRC32 stored at `0xDC`
  - struct `<? 9s B B B 93s h 2s h h 1s H 17s 59s B 16s` with an 0x02 prefix offset
  - XP at block+0x6C (level is derived from XP via the 50-entry table), Atk at +0x70,
    Def at +0x72, specials at +0x0C..0x0E, learn at +0x02, gift at +0x75, color at +0xC3
  - "SSBU Figure Player" action shows CRC OK/mismatch, level (XP based), Atk/Def, specials,
    learn flag, color and gift. Editing must also recompute the CRC32 at 0xDC.
  - **SSBU writing**: "SSBU stats (edit)" takes `level attack defense`, converts level -> XP with
    the same 50-entry table, writes XP/Atk/Def, recomputes the data-block CRC32 at 0xDC,
    re-signs the HMAC, re-encrypts and saves as `ssbu_<id>_L<nn>.nfc`.
  - Verified with a synthetic SSBU dump pushed through the real crypto path: after the edit,
    read-back reports `crc=OK xp=3912 atk=150 def=100` (level 50 -> XP 3912).
  - The CRC algorithm is the one used by the reference tool (poly 0xEDB88320, init 0, final xor);
    it has not been confirmed against a console-written SSBU dump yet.







# Action menu layout (grouped, 2026-11-20 night)

    Amiibo Actions
    ├─ Operate >   Emulate / Write to Tag / Save to Storage / Change UID
    ├─ Edit >      Set nickname / SSBU stats / Smash 3DS stats / Favorite
    ├─ Inspect >   Usage Info / Verify dump / Show settings / Owner Mii /
    │              App Data / SSBU Figure Player / (SSBU test) / (Smash test)
    ├─ Backup >    Backup dump / Restore latest
    └─ All actions (flat list, kept as a fallback)

- Verified on hardware: root menu, Operate submenu and Back out of a submenu all keep the app
  alive. Back from a submenu returns to the Amiibo detail page, not to the root menu.
- Not yet verified on hardware: Inspect submenu -> Show settings inside the grouped menu.

## Pending change (compiled, NOT deployed yet)

- Submenu Back now returns to the Amiibo Actions root menu instead of jumping to the
  Amiibo detail page. Implemented with pp->info_menu_depth (1 = root, 2 = group) and a
  depth check in the Back handler of all four scenes.
- Verified with ufbt (clean build). Not flashed on purpose, because the device was in use
  for manual testing; run ufbt launch to deploy it.

## 2026-11-20 night fixes (deployed)

1. Emulation from the library failed with "open a dump first" because the library path only
   copied the dump; it never derived the NTAG215 password/PACK that emulation needs.
   mi_tool_library_load_dump now sets UID, password and the default PACK (same as Generate).
   Verified with a temporary probe: emu:enter -> emu:started.
   Failure texts in the four scenes now explain the real causes.
2. By series produced duplicated/garbled entries: library_series_collect inserted through a
   pointer into the array it was shifting, so the entry got overwritten by its neighbour.
   Now a local value is inserted. Also: series names are compared case-insensitively and with
   trailing spaces trimmed, and AMI_TOOL_SERIES_MAX was raised 40 -> 64.
   Verified on hardware by dumping the aggregated list: 30 unique series, no duplicates
   (Animal Crossing 528, Super Smash Bros. 95, Mario Sports Superstars 90, Street Fighter 6 63,
   Pokemon 2 ...).
3. The deferred "submenu Back returns to the root menu" change is included in this build.

## Emulation write-back (2026-11-20 night)

- Symptom: data written by the reader while emulating (e.g. an owner Mii) was lost after
  stopping emulation and emulating again.
- Cause: the NFC listener keeps its own copy of the tag image; stop_emulation() freed the
  listener without copying that image back into pp->tag_data, so every new session started
  from the stale copy.
- Fix: mi_tool_info_stop_emulation() now pulls the live image with
  
fc_listener_get_data(listener, NfcProtocolMfUltralight) and copies it into
  pp->tag_data before freeing the listener. Verified with a probe: stop:copied pages=135.
- Emulation screen now says that writes stay in memory and that Save to Storage persists them.
- Physical verification (game writes a Mii, Back, emulate again) still to be done by the user,
  since it needs a real reader.

## Save policy change (2026-11-20 night, deployed)

- Save to Storage no longer creates a second file: if the dump came from the library
  (pp->last_dump_path is known) it first copies the current file into
  .ami_tool/backup/<tick>_<name>.nfc, then overwrites the source file in place.
  Verified with a probe: save:src=1, and a new backup appeared next to the original.
- The new file path is remembered, so repeated saves overwrite the same file.
- Emulation auto-save: on stop, if the readers writes changed the image
  (FNV hash of the page area before/after), the source file is backed up and overwritten
  automatically. Hash is taken after stop_emulation() so an old session cannot trigger it.
- Emulation screen now states that changes are written back on stop with a backup.

## Backup policy: one pristine copy per file (2026-11-20 night, deployed)

- .ami_tool/backup/<original name> holds the FIRST version seen; later saves skip the copy
  when the backup already exists, so the original is never replaced.
  Verified on hardware: first save created [AC] AF1 - Stitches.nfc, a second save added
  nothing (directory still has exactly one file).
- Restore latest backup now prefers the backup whose name matches the currently opened dump,
  falling back to the newest entry in the backup folder.
- Old tick-prefixed backups from the previous scheme were removed from the device.
