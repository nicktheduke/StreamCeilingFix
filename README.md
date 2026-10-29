# StreamCeilingFix

An SKSE plugin for Skyrim Special Edition / Anniversary Edition that stops the **"every save is corrupted"** problem huge load orders hit partway through a session.

## The problem

Skyrim keeps every plugin file (`.esp` / `.esm` / `.esl`) open for the whole session, and it opens each one **twice** (`TESFile::lockedFile` and `TESFile::file`). Those opens go through the C runtime, which allows **8192 open `FILE*` streams for the entire process**. [SSE Engine Fixes](https://www.nexusmods.com/skyrimspecialedition/mods/17230) already raises the limit to that maximum. It cannot go higher.

With thousands of plugins (easy now that light plugins allow 4096 slots), plus every SKSE plugin's log file, most of that pool is gone before you leave the main menu. Exterior cell loads open more. Once the pool is exhausted, *every* `fopen` in the process fails, including the one that opens your save. Every save in the load menu now looks corrupted until you restart the game.

The saves are fine. The game just can't open them.

## What it does

StreamCeilingFix patches the game's import table so its **read-only binary opens of plugin files** are served through plain Win32 file handles, which have no practical limit, instead of C runtime streams. That frees thousands of C runtime slots for saves, INIs and everything else.

- Only `.esp`, `.esm` and `.esl` opened in read-binary mode (`"rb"`) by **SkyrimSE.exe itself** are taken over.
- Saves, INIs, anything opened for writing or in text mode, and every other DLL go through the original C runtime untouched.
- Every `FILE*` function the game imports (`fread`, `fseek`, `_fseeki64`, `ftell`, `fgetc`, `ungetc`, `fgetpos`, ...) is wrapped, so a stream the plugin owns can never reach the real C runtime.
- **No game memory addresses**: it only touches the game's import table. It does not need Address Library and should survive game updates.

On a 3,683-plugin load order the game sits at ~7,900 open files at the main menu and used to break shortly after loading into Tamriel. With StreamCeilingFix it runs with 9,100+ open files and the C runtime still has free slots.

## Who needs it

Load orders of roughly **3000+ plugins**, or anyone whose saves show as corrupted after some play time and load fine again after a restart. Smaller setups won't notice anything; it does no harm there.

## Requirements and compatibility

| | |
|---|---|
| SKSE | Required |
| SSE Engine Fixes | Recommended (raises the C runtime limit to 8192) |
| Address Library | Not needed |
| Skyrim 1.6.1170 (Steam) | Tested in game |
| Other AE builds, GOG | Should work (version-independent, untested) |
| SE 1.5.97 | Should work (`SKSEPlugin_Query` included, untested) |
| VR | Untested |

If a game build doesn't open files the expected way, the plugin logs `nothing to do` and stays out of the way.

## Install

Grab the zip from Releases and install it with Mod Organizer 2 or Vortex, or copy `SKSE/Plugins/StreamCeilingFix.dll` into `Data/SKSE/Plugins`. To uninstall, remove the DLL.

## Log

`Documents/My Games/Skyrim Special Edition/SKSE/StreamCeilingFix.log`

```
StreamCeilingFix 1.0.1 (pid 12345)
active: the game's read-only plugin file opens no longer use C runtime stream slots
taking over: ...\Data/Dawnguard.esm
plugin streams held outside the CRT: 1000 open (peak 1000, 1005 opened in total)
```

If `StreamCeilingFix.log` is still held by a game process that didn't fully exit, the log is written to `StreamCeilingFix.1.log` instead (with a note saying so). If you ever see `plugin opened in mode ... left to the CRT`, please open an issue with your log.

## Building

**MinGW-w64 (Linux, WSL, MSYS2)** - the reference build, used by CI:

```sh
make            # build/StreamCeilingFix.dll
make test       # runs the test harness under Wine
make package    # build/StreamCeilingFix-<version>.zip for MO2 / Vortex
```

**Visual Studio 2019+ / CMake**:

```sh
cmake -B build -A x64
cmake --build build --config Release
```

The CMake build is checked with MinGW; MSVC builds should work but haven't been tested yet.

## Tests

`tests/harness.c` is a self-checking harness linked against the Universal CRT, the same way `SkyrimSE.exe` is, so the plugin patches it exactly as it patches the game. It checks:

- both SKSE entry points and that the plugin reports `active`
- write-mode plugin opens pass through to the C runtime
- 9,000 plugin streams held at once (the C runtime's default limit is 512, its maximum 8192), with an ordinary file still opening while they're held
- 200,000 random `fread` / `fgetc` / `ungetc` / `fseek` / `_fseeki64` / `ftell` / `fgetpos` / `fsetpos` operations checked byte-for-byte against the file
- `fopen_s`, `_wfopen_s`, and `ENOENT` for a missing plugin
- the locked-log fallback (`harness.exe lock`)

Without the plugin, the same harness fails at 509 open files and can't open anything else afterwards: the exact failure the game hits.

## License

MIT, see [LICENSE](LICENSE).
