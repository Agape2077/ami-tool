# AmiTool — implemented features and how to use them

Status as of 2026-11-20 04:45. Everything below was compiled with `ufbt` (Flipper SDK API 87.1,
target f7) and exercised on hardware unless marked otherwise.

## Library (Saved)

- Scans `/ext/nfc/amiibo` recursively (3 levels), index cached on disk in
  `/ext/nfc/amiibo/.ami_tool/` (`idx`, `meta`, `ids`, `fav`, `backup/`).
- 1035 files indexed in the test collection, 941 resolved to Amiibo ID + name + series.
- 12 files per page, `Library n/m - N files` header.
- Menu entries: *Index this page*, *Index ALL files* (background thread with progress and
  cancel), *Rescan library*, *By series*, *Favorites*, *Search*, *Unrecognized*.
- `By series` uses the cached series names: 29 groups in the test collection.
- `Search` matches name / series / path, case-insensitive (verified: `kirby` -> 5 hits).
- Favorites are stored by Amiibo ID in `.ami_tool/fav`.
- `Unrecognized` lists entries without a valid Amiibo ID.
- The index is rebuilt automatically when the library folder timestamp changes; cached ids are
  validated by path so a replaced collection cannot mislabel files.

## Amiibo actions (grouped menu)

Amiibo detail -> OK -> `Amiibo Actions`:

- **Operate**: Emulate, Write to Tag, Save to Storage, Change UID.
- **Edit**: Set nickname, SSBU stats, Smash 3DS stats, Favorite toggle.
- **Inspect**: Usage Info, Verify dump, Show settings, Owner Mii (hex), App Data (hex),
  SSBU Figure Player.
- **Backup**: Backup dump, Restore latest backup.
- **All actions**: the previous flat list, kept as a fallback.

Back inside a group returns to the root menu (change compiled, not yet flashed).

## Crypto

- Retail key is read from `/ext/apps_data/ami_tool/key_retail.bin` (must be exactly 160 bytes).
- `Verify dump` derives data/tag keys, decrypts a copy and validates both HMACs.
  Verified on hardware: `0282000101D60502 -> VALID`.
- Edits always write a **new file** and leave the source untouched; the result screen also
  reports the re-signed signature status.

## Editing

- **Nickname**: text input, ASCII, up to 10 chars. Writes UTF-16BE at settings+0xC, sets the
  setup flag bit, re-signs and re-encrypts.
  Round-trip verified on hardware: reading the saved dump back reports `Flags 0xB7` and
  `Nickname: "AmiTool"`.
- **SSBU (Switch) stats**: level/attack/defense. Level is converted to XP with the 50-entry
  table, the data-block CRC32 at 0xDC is recomputed, then the payload is re-signed and
  re-encrypted. Verified with a synthetic SSBU dump: read-back `crc=OK xp=3912 atk=150 def=100`.
- **Smash 3DS/Wii U stats**: level/attack/defense/strength in the SSB AppData layout
  (AppID 0x10110E00). Verified with a synthetic dump.
- Caveat: both game parsers use documented/reference layouts. They have **not** been validated
  against a console-written game dump, because the collection contains none.

## Backup / restore

- `Backup dump` copies the opened file to `.ami_tool/backup/<tick>_<name>.nfc`.
- `Restore latest backup` copies the newest backup into the library as `restored_<name>.nfc`.
- Both verified on hardware; neither overwrites the original.

## Known limits

- No Owner attribute editing (country / setup date) yet, Mii block is read-only.
- `Restore` only takes the newest backup; there is no chooser yet.
- Action-menu Back behaviour change is compiled but not deployed at the time of writing.
- Library name resolution depends on the local index; after replacing the collection you should
  run *Index ALL files* once.
