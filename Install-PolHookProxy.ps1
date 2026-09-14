<#
.SYNOPSIS
  Install (or revert) the self-loading PoL-Shim as a PolHook.dll PROXY.

.DESCRIPTION
  pol.exe STATICALLY imports PolHook.dll (SE's small legacy mouse-hook DLL), so
  dropping our proxy in its place makes pol.exe load the whole shim BY ITSELF --
  no injector, no admin. The proxy forwards SE's 6 exports to the renamed
  original (PolHook_orig.dll), so nothing SE does breaks.

    Install:  PolHook.dll -> PolHook_orig.dll (backup+forward), our proxy -> PolHook.dll,
              plus a polshim.ini beside it.
    Revert:   -Revert  (deletes our proxy, restores PolHook_orig.dll -> PolHook.dll)

  The Viewer MUST be closed first (an open pol.exe locks PolHook.dll).

  After installing, LAUNCH THE NORMAL WAY (pol.exe / the standard PlayOnline
  shortcut) -- NOT the "PlayOnline (shim)" injector shortcut. The proxy self-loads;
  using both would load the shim twice.

.PARAMETER Server
  Optional server IP written to polshim.ini as [redirect] server=<ip> (persistent).
  You can instead override at launch (no file edit) with the POLSHIM_SERVER env var
  or a --polserver=<ip> command-line arg -- which is the Steam Deck path.

.EXAMPLE
  .\Install-PolHookProxy.ps1
  .\Install-PolHookProxy.ps1 -Server 198.51.100.10 -Gamepad
  .\Install-PolHookProxy.ps1 -Revert
#>
[CmdletBinding()]
param(
  [string]$InstallDir = "C:\Program Files (x86)\PlayOnline\SquareEnix\PlayOnlineViewer",
  [string]$ProxyDll   = (Join-Path $PSScriptRoot "build\PolHook.dll"),
  [string]$IniSource  = (Join-Path $PSScriptRoot "polshim.ini.production"),
  [string]$Server     = "",
  [switch]$Gamepad,
  [switch]$Revert
)

$ErrorActionPreference = 'Stop'
function Info($m){ Write-Host "[+] $m" -ForegroundColor Green }
function Warn($m){ Write-Host "[~] $m" -ForegroundColor Yellow }
function Fail($m){ Write-Host "[!] $m" -ForegroundColor Red; exit 1 }

$pol  = Join-Path $InstallDir "pol.exe"
$hook = Join-Path $InstallDir "PolHook.dll"
$orig = Join-Path $InstallDir "PolHook_orig.dll"
$ini  = Join-Path $InstallDir "polshim.ini"

if (-not (Test-Path $pol)) { Fail "pol.exe not found in `"$InstallDir`" -- wrong -InstallDir?" }

# An open pol.exe holds PolHook.dll (static import), so the swap can't happen.
if (Get-Process -Name pol -ErrorAction SilentlyContinue) {
  Fail "PlayOnline (pol.exe) is RUNNING. Close it fully, then re-run -- it locks PolHook.dll."
}

# ---- revert -----------------------------------------------------------------
if ($Revert) {
  if (-not (Test-Path $orig)) { Fail "No PolHook_orig.dll here -- proxy not installed in `"$InstallDir`"." }
  Remove-Item $hook -Force -ErrorAction SilentlyContinue
  Rename-Item $orig $hook
  Info "Reverted: SE's PolHook.dll restored. (polshim.ini left in place; delete it if you like.)"
  exit 0
}

# ---- install ----------------------------------------------------------------
if (-not (Test-Path $ProxyDll)) { Fail "Proxy DLL not found: `"$ProxyDll`". Build it first (build.bat -> build\PolHook.dll)." }

if (Test-Path $orig) {
  Warn "PolHook_orig.dll already present -- updating the proxy only (backup preserved)."
} else {
  if (-not (Test-Path $hook)) { Fail "No PolHook.dll in `"$InstallDir`" -- is this a PlayOnline Viewer install?" }
  Copy-Item $hook "$hook.preproxy.bak" -Force        # extra belt-and-suspenders copy
  Rename-Item $hook $orig
  Info "Backed up SE PolHook.dll -> PolHook_orig.dll"
}

Copy-Item $ProxyDll $hook -Force
Info "Installed proxy -> $hook"

# polshim.ini beside the DLL. Created only if absent, so re-runs keep your edits.
if (-not (Test-Path $ini)) {
  if (Test-Path $IniSource) { Copy-Item $IniSource $ini -Force; Info "Wrote polshim.ini (from $([IO.Path]::GetFileName($IniSource)))" }
  else {
    Set-Content $ini "[polshim]`r`nlog=polshim.log`r`ntitletag=1`r`nmodules=polcore.dll,app.dll,PolContents.dll`r`n`r`n[redirect]`r`nenable=0`r`nstrict=1`r`n" -Encoding ASCII
    Info "Wrote a minimal polshim.ini"
  }
} else { Warn "polshim.ini already exists -- left as-is." }

# Small INI editor (section-aware). Used for -Server / -Gamepad.
function Set-IniKey([string]$path,[string]$section,[string]$key,[string]$value) {
  $lines = @(Get-Content $path)
  $out = New-Object System.Collections.Generic.List[string]
  $inSec = $false; $done = $false
  foreach ($l in $lines) {
    if ($l -match '^\s*\[(.+?)\]') {
      if ($inSec -and -not $done) { $out.Add("$key=$value"); $done = $true }
      $inSec = ($matches[1] -ieq $section)
      $out.Add($l); continue
    }
    if ($inSec -and ($l -match "^\s*;?\s*$([regex]::Escape($key))\s*=")) {
      if (-not $done) { $out.Add("$key=$value"); $done = $true }
      continue
    }
    $out.Add($l)
  }
  if ($inSec -and -not $done) { $out.Add("$key=$value"); $done = $true }
  if (-not $done) { $out.Add("[$section]"); $out.Add("$key=$value") }
  Set-Content $path $out -Encoding ASCII
}

if ($Server)  { Set-IniKey $ini 'redirect'  'server' $Server;          Info "Set [redirect] server=$Server (persistent; arms in-process redirect)" }
if ($Gamepad) { Set-IniKey $ini 'inputmode' 'mode'   'force_gamepad';  Info "Set [inputmode] mode=force_gamepad (Steam Deck controller)" }

Write-Host ""
Info "DONE."
Write-Host "Next:" -ForegroundColor Cyan
Write-Host "  1) Launch PlayOnline the NORMAL way (pol.exe / standard shortcut)."
Write-Host "     NOT the 'PlayOnline (shim)' injector shortcut -- the proxy self-loads;"
Write-Host "     using both loads the shim twice."
Write-Host "  2) The window title should end with '[PoL-Shim v0.1.0]' -- that confirms it loaded."
Write-Host "  3) Change server at launch WITHOUT editing this file:"
Write-Host "       env :  POLSHIM_SERVER=<ip>"
Write-Host "              Steam Deck -> game Properties -> Launch Options:  POLSHIM_SERVER=<ip> %command%"
Write-Host "       arg :  add  --polserver=<ip>  to the launch command"
Write-Host "     (precedence: --polserver > POLSHIM_SERVER > [redirect] server= in the ini)"
Write-Host "  4) Log to confirm: $InstallDir\polshim.<pid>.log"
Write-Host "     look for the '[polshim] PoL-Shim v...' banner and a '[dns] REDIRECT ACTIVE' line."
Write-Host "  Revert anytime:  .\Install-PolHookProxy.ps1 -Revert" -ForegroundColor Cyan
