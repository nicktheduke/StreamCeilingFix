StreamCeilingFix 1.0.1 - SKSE plugin
by NSM

WHAT IT FIXES
Very large load orders start showing every save as "corrupted" partway through a session.
A restart makes them work again. The saves are fine; the game just can't open them.

WHY IT HAPPENS
Skyrim keeps every plugin (.esp/.esm/.esl) open for the whole session, twice, through the
C runtime. The C runtime allows 8192 open files for the entire process (SSE Engine Fixes
already raises it to that maximum; it can't go higher). Thousands of plugins plus SKSE
plugin log files fill most of that pool at the main menu, and exterior cell loads push it
over. After that, opening any file through the C runtime fails, saves included.

WHAT IT DOES
Serves the game's read-only opens of plugin files through plain Windows file handles,
which have no practical limit. That frees thousands of C runtime slots for saves, INIs
and everything else. Saves, INIs, anything opened for writing, and other SKSE plugins go
through the normal path untouched. No game memory addresses are used: it only reroutes
the game's own file-open calls.

WHO NEEDS IT
Load orders of roughly 3000+ plugins, or anyone whose saves show as corrupted after some
play time and load fine again after a restart. Smaller setups won't notice anything.

REQUIREMENTS
- SKSE for your game version
- SSE Engine Fixes (or anything that raises the C runtime file limit to 8192)
- No Address Library needed

VERSIONS
Tested on Skyrim SE 1.6.1170 (Steam). Built version-independent for AE and with the
1.5.97 SKSE entry point; if a build doesn't open files the expected way, it logs
"nothing to do" and stays out of the way.

INSTALL
Install with Mod Organizer 2 or Vortex, or copy SKSE\Plugins\StreamCeilingFix.dll into
Data\SKSE\Plugins. To uninstall, remove the DLL.

LOG
Documents\My Games\Skyrim Special Edition\SKSE\StreamCeilingFix.log
Look for "active" and "taking over: ..." lines. If that file is still held by a game
process that did not fully exit, the log goes to StreamCeilingFix.1.log instead.
