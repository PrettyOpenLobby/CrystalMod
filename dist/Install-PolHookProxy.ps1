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

# --- the sign-up allow-list -------------------------------------------------------
#
# When you finish registering, the server answers with an RDT block that makes the
# Viewer create your login-screen member ITSELF -- ID and password saved, no typing.
# The client refuses that block unless its source URL passes a gate (app.dll
# 0x4a656f8): an https URL on a *.pol.com host, or a prefix listed in the install's
# own usr\all\url\rdthosts.bin. We serve sign-up over PLAIN HTTP (https dies at
# POL-1328 on the certificate), so only the allow-list can admit us -- and the
# shipped one holds SE's two entries, neither of them ours. The block is then
# discarded WHOLE, NEXT-URL included, and the player is bounced back with an error
# code. It happened to three real players before this ran anywhere.
#
# The cipher is a 32-bit XOR keystream (app.dll 0x4a64e81), implemented here rather
# than shelled out to Python so a Windows box needs no Python. The PowerShell and
# Python implementations are pinned byte-for-byte against each other by a parity
# test that builds the same image both ways.
#
# Check Files does not threaten this: every usr\ row in file.txt is a default\ row,
# so the live copy is unmanifested and no digest has to move. A profile RESET does
# drop it (default\ is re-copied over usr\) -- re-run this script after one.
#  WARNING: EVERY CONSTANT HERE CARRIES AN `L` AND THAT IS LOad-BEARING. Windows
#  PowerShell 5.1 types a bare hex literal as Int32, so `0xC8D1B221` is
#  -925781471 and `0xFFFFFFFF` is -1. Casting either to [uint64] then throws,
#  and -- because this ran inside a try//catch that treats a failure as "skip"
#  -- the first version of this code produced an ALL-ZERO keystream and wrote
#  the allow-list out in PLAINTEXT without a word of complaint. The parity test
#  is what caught it. Keep the suffixes.
function Get-RdtKeystream([int]$n) {
  # k = 0xC8D1B221 ; per dword: emit k, then k += (~k >>> 21) | (k << 11), 32-bit.
  $mask = 0xFFFFFFFFL
  $ks = New-Object 'System.UInt32[]' $n
  $k = [uint64]0xC8D1B221L
  for ($i = 0; $i -lt $n; $i++) {
    $ks[$i] = [uint32]$k
    $notk = [uint64](($mask - $k) -band $mask)     # ~k within 32 bits
    $mix  = [uint64]((($notk -shr 21) -bor (($k -shl 11) -band $mask)) -band $mask)
    $k    = [uint64](($k + $mix) -band $mask)
  }
  return ,$ks
}
function Convert-Rdt([byte[]]$blob) {
  # XOR is its own inverse, so this both decrypts and encrypts. Trailing bytes past
  # the last whole dword are dropped, exactly as the reader does (`shr esi, 2`).
  $n = [int][Math]::Floor($blob.Length / 4)
  $ks = Get-RdtKeystream $n
  $out = New-Object 'System.Byte[]' ($n * 4)
  for ($i = 0; $i -lt $n; $i++) {
    $word = [uint32](([uint64][BitConverter]::ToUInt32($blob, $i * 4) -bxor
                      [uint64]$ks[$i]) -band 0xFFFFFFFFL)
    [Array]::Copy([BitConverter]::GetBytes($word), 0, $out, $i * 4, 4)
  }
  # `,` so the array comes back as one object -- a bare `return $out` unrolls it
  # onto the pipeline and the caller gets Object[] of boxed bytes instead.
  return ,$out
}
function Add-RdtHost([string]$path, [string[]]$urls) {
  $plain = [Text.Encoding]::ASCII.GetString((Convert-Rdt ([IO.File]::ReadAllBytes($path))))
  $entries = @($plain.TrimEnd([char]0) -split "`r`n|`n" | ForEach-Object { $_.Trim() } |
               Where-Object { $_ })
  $new = @($urls | Where-Object { $entries -notcontains $_ })
  if ($new.Count -eq 0) { return $false }
  # SE's entries stay -- they cost nothing and dropping them is an invented change.
  $merged = @($entries + $new)
  $text = ($merged | ForEach-Object { "$_`r`n" }) -join ''
  $bytes = [Text.Encoding]::ASCII.GetBytes($text)
  if ($bytes.Length % 4) { $bytes += New-Object 'System.Byte[]' (4 - $bytes.Length % 4) }
  $img = Convert-Rdt $bytes
  # Read it back before committing: an image that does not parse as what was asked
  # for would silently disable the client's whole allow-list.
  $check = [Text.Encoding]::ASCII.GetString((Convert-Rdt $img)).TrimEnd([char]0)
  $back = @($check -split "`r`n|`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ })
  if (@(Compare-Object $back $merged -SyncWindow 0).Count) {
    throw "rdthosts image does not read back as written -- refusing to write it"
  }
  if (-not (Test-Path "$path.orig")) { Copy-Item $path "$path.orig" -Force }
  [IO.File]::WriteAllBytes($path, $img)
  return $true
}

$rdt = Join-Path $InstallDir "usr\all\url\rdthosts.bin"
if (Test-Path $rdt) {
  try {
    # Both forms the live route can take: the shim's signup_http lever rewrites the
    # client's own builder to http on :80, the on-disk lever uses :8080.
    if (Add-RdtHost $rdt @("http://ucs.pol.com/pml-cgi-bin/",
                           "http://ucs.pol.com:8080/pml-cgi-bin/")) {
      Info "Sign-up allow-list updated (the Viewer can auto-add your member)"
    } else {
      Info "Sign-up allow-list already current"
    }
  } catch {
    # Never fatal: registration still works, you just add the member by hand.
    Warn "could not update the sign-up allow-list ($($_.Exception.Message))."
    Warn "  Registration still works -- use Add Member with the ID it shows you."
  }
} else {
  Warn "no usr\all\url\rdthosts.bin here -- skipping the sign-up allow-list."
}

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
