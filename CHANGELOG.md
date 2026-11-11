# Changelog

## 1.0.2 - 2026-11-10
- Import guard: at startup the plugin checks every C runtime stream function the game imports. If the game uses one the plugin doesn't wrap, it logs which one, reports `NOT active` and leaves the game untouched. No change on Skyrim 1.6.1170, where every stream import is covered.
- `fputwc` and `fputs` are wrapped too (writes to a plugin stream fail, as before for `fputc` and `fwrite`).
- Author field and copyright now read nicktheduke.

## 1.0.1 - 2026-10-29
- The log is opened read/write/delete-shareable, so a game process that hasn't fully exited can no longer leave the next session without a log. If `StreamCeilingFix.log` is still held by something else, the plugin logs to `StreamCeilingFix.1.log` (up to `.3`) and says why.
- First log line shows the process ID, which makes a leftover game process easy to spot.
- Added `SKSEPlugin_Query` so the same DLL loads under SKSE for SE 1.5.97.

## 1.0.0 - 2026-10-27
- First release. Serves SkyrimSE.exe's read-only binary opens of `.esp` / `.esm` / `.esl` through Win32 handles instead of C runtime streams.
