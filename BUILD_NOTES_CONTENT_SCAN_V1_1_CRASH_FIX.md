# AmiTool Content Scan v1.1 — Saved crash mitigation

## Symptom addressed
The app crashes immediately after entering Saved, before the user selects an NFC file.

## Source-level finding
The v1 Saved entry path performs a deeper synchronous call chain than the original app:
`Saved on_enter -> refresh -> load_page -> read_identity -> nfc_device_load -> Amiibo ID extraction -> amiibo.dat lookup`.
The app manifest still declared a 2 KiB thread stack. This is a credible stack-exhaustion risk for the added NFC file parser + asset lookup path. It is not possible to prove this was the device's exact crash cause without a crash dump.

## Change
- Increased `application.fam` `stack_size` from 2 KiB to 4 KiB.
- No functional behavior or NFC data parsing was changed in this mitigation release.

## Validation
- Confirmed the manifest has exactly one stack-size setting and it is now `4 * 1024`.
- Confirmed source tree and ZIP archive are structurally valid.
- Native Flipper compilation is still unavailable in this environment because `ufbt`, SCons, and Flipper SDK are not installed.

## Test
Build with `ufbt`, install, then tap Saved directly from the main menu.
If it still crashes, the next isolation build should temporarily disable `nfc_device_load()`/database lookup in the Saved scan to distinguish stack exhaustion from NFC/storage API misuse.
