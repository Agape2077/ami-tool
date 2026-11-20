# AmiTool content-based Saved scanner patch

## Changes
- `Saved` now considers `.nfc` files regardless of whether the filename follows AmiTool's old `AmiiboID-UID.nfc` naming convention.
- Each candidate file is loaded with Flipper's `NfcDevice` parser.
- The scanner extracts the Amiibo ID from NTAG215 pages 21 and 22 using the existing `ami_tool_extract_amiibo_id()` helper.
- The ID must exist in the bundled `files/amiibo.dat` database; this avoids listing unrelated NFC dumps as Amiibo.
- The display name comes from the bundled Amiibo database, and the UID is read from the tag data.
- Existing 12-item pagination, detail view, emulation, and tag-writing actions are retained.

## Validation performed
- ZIP integrity check: passed.
- The 9 supplied `.nfc` samples were checked against `files/amiibo.dat`: 9/9 IDs matched.
- Source-level checks: balanced braces, no trailing whitespace, and no remaining references to the old filename parser.
- A native compile could not be completed in this environment: `ufbt` and SCons are not installed, and the standalone GCC syntax check stops at the missing Flipper SDK header `furi.h`.

## Current limitations
- This patch scans `.nfc` files directly inside `/nfc/`; it does not yet recurse into subdirectories.
- The list is still a flat list. Series/category navigation and search are not part of this patch.
- This patch does not implement Mii, Owner, or per-game save-data editing. It is the first step: reliable library discovery and identification.

## Build on a Flipper development machine
From this project directory, run:

```bash
ufbt
```

To build and launch on a connected Flipper Zero:

```bash
ufbt launch
```
