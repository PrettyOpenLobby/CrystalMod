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

## Install

**Windows**: run `PolShimSetup.exe`, enter your server address when asked
(there is no default), done. Command line: `PolShimSetup.exe --server=ADDR`.

**Steam Deck / Proton**: `bash install.sh` from the `dist/` folder; it
finds the PlayOnline install under Steam, asks for the server address, and
enables D3D8 through DXVK. `install.sh --revert` undoes everything.

Settings live in `polshim.ini` next to `pol.exe`; the in-game dialog edits
the same file. `[redirect] server=` is the only required value.

## Updates

If the server hosts a shim build (OpenLobby serves `/shim/dist/` from its
portal tree), the shim checks it hourly and offers newer builds; it never
installs an older one. Set `[autoupdate] enable=0` to opt out.

## Build

`build.bat` with Visual Studio 2022 (x86 tools) produces `build\PolHook.dll`
and `build\PolShimSetup.exe`. No third-party libraries; hooks, image
patching and the PNG writer are in-tree. CI builds every push on a Windows
runner and publishes both artifacts.

## What is not included

- No Square Enix files. The shim is a proxy for a file that stays on your
  disk under its new name.
- No server address. You supply your own.

## License

AGPL-3.0 (see LICENSE).
