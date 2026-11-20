# AmiTool — Amiibo manager for Flipper Zero

A Flipper Zero application to browse, inspect, edit and re-emulate Amiibo dumps stored on the
SD card. Built as a Flipper application (`ufbt`, API 87.1, target f7).

## Features

### Library (Saved)

- Scans `/ext/nfc/amiibo` recursively (3 levels) and keeps an on-disk index in
  `/ext/nfc/amiibo/.ami_tool/`, so opening the library does not parse any dump.
- Identifies every file by its Amiibo ID (pages 21/22), not by its file name.
- 12 entries per page; on-demand background indexer (`Index ALL files`) with progress and
  cancel, plus `Index this page`, `Rescan library`.
- **By series** grouping built from `amiibo.dat` metadata (product series, alphabetical,
  counts, case/spacing tolerant).
- **Favorites** stored by Amiibo ID in `.ami_tool/fav`.
- **Search** by name, series or path (case-insensitive, paginated).
- **Unrecognized** view listing files without a valid Amiibo ID.
- Automatic re-index when the library folder timestamp changes; cached ids are validated by
  path so a replaced collection cannot be mislabelled.

### Amiibo actions

`Amiibo detail → OK` opens a grouped menu:

```
Operate   Emulate / Write to Tag / Save to Storage / Change UID
Edit      Set nickname / Owner attrs / Clear owner Mii / SSBU stats /
          Smash 3DS stats / Favorite
Inspect   Usage Info / Verify dump / Show settings / Owner Mii (hex) /
          App Data (hex) / SSBU Figure Player
Backup    Backup dump / Restore from backup... / Restore latest
All       flat list of everything above
```

### Crypto

- Retail key read from `/ext/apps_data/ami_tool/key_retail.bin` (exactly 160 bytes).
- `Verify dump` derives the per-tag data/tag keys, decrypts a copy and validates both HMACs.
- All edits are re-signed and re-encrypted before they are written.

### Editing

| Item | What it does |
|---|---|
| Set nickname | UTF-16BE owner nickname (ASCII input, 10 chars) at settings+0xC, sets the setup flag |
| Owner attrs | country code and setup/last-write date at settings+0x1 / +0x4 / +0x6 |
| Clear owner Mii | zeroes the 0x60-byte owner Mii block at settings+0x20, then re-signs |
| SSBU stats | level (converted to XP with the 50-entry table), attack, defense, with the SSBU data-block CRC32 at 0xDC recomputed |
| Smash 3DS stats | level, attack, defense, speed in the SSB AppData layout |

Edits modify the in-memory tag image; **Save to Storage** writes it out.

### Emulation

- Emulates the loaded dump over NFC; the NTAG215 password/PACK are derived from the UID.
- Writes made by the reader while emulating are pulled back into the in-memory image when
  emulation stops.
- If the image changed and the dump came from a file, the file is backed up and overwritten
  automatically (see backup policy).

### Save / backup policy

- `Save to Storage` never creates a second file: if the dump came from the library, the
  original file is backed up and then overwritten in place.
- `.ami_tool/backup/<original name>` keeps exactly **one** copy — the first version seen.
  Later saves never replace it.
- `Restore latest backup` restores the backup of the currently opened file (or the newest one)
  as `restored_<name>.nfc`; it never overwrites anything.
- `Restore from backup...` lists the files in `.ami_tool/backup` and restores the chosen one.
  Restores always create `restored_<name>.nfc` and never touch the backup or the source.

## Requirements

- Flipper Zero with a recent firmware (API 87.1).
- `key_retail.bin` (160 bytes) placed at `/ext/apps_data/ami_tool/key_retail.bin`.
  Without it, decryption, emulation and editing are unavailable; the library still works.
- Amiibo dumps in `/ext/nfc/amiibo` (sub-folders are fine).

## Build

```sh
ufbt            # build
ufbt launch     # build, install to /ext/apps/NFC and start on a connected Flipper
```

## SD card layout

```
/ext/nfc/amiibo/
    ...your dumps...
    .ami_tool/
        idx      indexed relative paths (one per line)
        meta     file count + library folder timestamp
        ids      id<TAB>name<TAB>series<TAB>path cache
        fav      favorite Amiibo IDs
        backup/  one pristine copy per edited file
```

`.ami_tool/` is a cache + backup area: deleting it triggers a full re-index (about two minutes
for ~1000 files) but loses no dump.

## Limitations

- Owner Mii data is read-only apart from "Clear owner Mii"; there is no Mii editor.
- Game-data parsers exist for Super Smash Bros. Ultimate (read + write) and SSB for
  3DS/Wii U (read + write). Both follow documented/reference layouts and have been validated
  with synthetic dumps through the real crypto path, but not against console-written dumps.
- The flat `All actions` list has no Back to the grouped menu.

## Docs

- `BUILD_NOTES_CONTENT_SCAN_V2.md` — implementation notes and the verification evidence.
- `docs/FEATURES.md` — feature list with the hardware checks performed.
