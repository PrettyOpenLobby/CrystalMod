# CrystalMod

A small in-process add-on for Square Enix's PlayOnline Viewer that lets an
unmodified client talk to a private server (such as OpenLobby) and run its
titles on modern Windows and on Steam Deck / Proton. It does two things:

- **Routing**: resolves the client's `*.pol.com` / `*.playonline.com` /
  `*.sqex.net` names to the server address you configure, in-process, with
  no hosts file or DNS change. Strict mode refuses any unmapped name so
  nothing leaks to the internet.
- **Game fixes**: the compatibility work the 2003-era client and its titles
  need today: windowed and scaled D3D8/D3D9/DirectDraw, DirectInput mouse
  and gamepad seams, focus and sleep/resume recovery, registry repair for
  copied installs, per-title patches applied in memory after unpack, a
  Proton D3D8-on-DXVK opt-in, OS dialog translation, and an in-game
  settings dialog (Home key, or Back+Start on a pad).

Nothing else: no telemetry, no automation, no bundled third-party tools.

## How it works

`pol.exe` statically imports `PolHook.dll`. The installer renames the
game's own `PolHook.dll` to `PolHook_orig.dll` and puts ours in its place;
ours forwards the six original exports to `PolHook_orig.dll` and runs its
hooks from `DllMain`, before the Viewer's entry point. Square Enix's file
is renamed, never modified or redistributed. Uninstall = rename it back.

## Download

Every release on the repository's Releases page carries `PolShimSetup.exe`
(the Windows installer), `PolHook.dll` with its `PolHook.dll.sha256`,
`install.sh` and `Install-PolHookProxy.ps1`, the `polshim.ini` template, a
`SHA256SUMS` file and a zip of the whole set. Every push to `main` also
leaves the same set as a workflow artifact for anyone who wants the newest
build before a release is cut.

## Install

**Windows**: run `PolShimSetup.exe`. It asks which server to use and offers
`play.openlobby.fyi`; press Enter for that, or type another name or IP address. Command line:
`PolShimSetup.exe --server=ADDR`.

**Steam Deck / Proton**: `bash install.sh` from the `dist/` folder; it
finds the PlayOnline install under Steam, asks for the server (same default),
and enables D3D8 through DXVK. `install.sh --revert` undoes everything.

Both installers also keep PlayOnline's own `file.txt` in step. That file is the
manifest the Viewer checks itself against, and replacing `PolHook.dll` without
rewriting its line leaves the install disagreeing with itself: PlayOnline then
either puts SE's DLL back at the next Check Files, or stops during an update and
says a file is missing. The original manifest is kept as `file.txt.polshim-orig`
and `--revert` puts it back. On Linux this step needs `python3`; without it the
installer says so and carries on.

A server name is looked up once, by the installer, and written to `polshim.ini`
as an IP address, because that is the form the shim reads. If the server moves
to a new address, run the installer again.

Settings live in `polshim.ini` next to `pol.exe`; the in-game dialog edits
the same file. `[redirect] server=` is the only required value.

## Updates

The shim checks the project's latest GitHub release hourly and offers newer
builds; it never installs an older one. Both installers fetch from the same
place. Set `[autoupdate] enable=0` to opt out.

Updates do not come from the game server you play on. A server operator who
wants to host a build for their own players unzips a release's set into
`www/shim/dist/` of the OpenLobby checkout and has them set `[autoupdate]
url=server` (installers: `--update-url=server`, or `POLSHIM_UPDATE_URL=server`
for `install.sh`). The `PolHook.dll.sha256` in that folder is what the updater
and `install.sh` compare against.

## Build

`build.bat` with Visual Studio 2022 (x86 tools) produces `build\PolHook.dll`
and `build\PolShimSetup.exe`. No third-party libraries; hooks, image
patching and the PNG writer are in-tree. CI builds every push on a Windows
runner and publishes both artifacts.

## What is not included

- No Square Enix files. The shim is a proxy for a file that stays on your
  disk under its new name.
- No server of its own. The installers offer the OpenLobby public server and
  take any other address you give them.

## License

AGPL-3.0 (see LICENSE).
