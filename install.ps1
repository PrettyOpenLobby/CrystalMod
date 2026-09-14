<#
.SYNOPSIS
  Interpose polshim on a PlayOnline component.

.DESCRIPTION
  Two mechanisms, because the Viewer does not load its own components through
  the COM registry:

    -Mode File   (default) Renames the component to <name>.orig.dll and drops
                 polshim.dll in its place. This is the one that works for the
                 Viewer, which builds a file path, LoadLibrary's it, and calls
                 DllGetClassObject via GetProcAddress. polcore.dll exports only
                 the four COM entry points, so the shim covers its whole surface.

    -Mode Clsid  Repoints the CLSID's InprocServer32. Only affects callers that
                 actually go through COM activation (polcfg.exe and friends) --
                 the Viewer itself ignores it.

  Both are fully reversible with -Restore.

.EXAMPLE
  .\install.ps1                          # file-mode on polcore
  .\install.ps1 -Component app           # file-mode on the Viewer core
  .\install.ps1 -Restore                 # undo whatever is installed
#>
[CmdletBinding()]
param(
    [ValidateSet('polcore','app','ffximain','tm','polcontents','polmvf')]
    [string]$Component = 'polcore',
    [ValidateSet('File','Clsid')]
    [string]$Mode = 'File',
    [string]$ShimPath = (Join-Path $PSScriptRoot 'build\polshim.dll'),
    [string]$LogPath  = (Join-Path $PSScriptRoot 'build\polshim.log'),
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'

$CLSIDS = @{
    'polcore'     = '{3501F5DD-7894-42DF-866A-A2B6527D8049}'
    'app'         = '{40555AAE-53AD-4ABC-AE65-8441755E7D69}'
    'ffximain'    = '{1027DC46-750D-4B1F-8834-1D25B8BEBAB8}'
    'tm'          = '{72B2FE03-BA77-4867-84A2-7BAD0F5A8FBB}'
    'polcontents' = '{62021866-976B-49A3-A18B-7A44869008A2}'
    'polmvf'      = '{E697F40A-B701-4F75-A201-635FA69A41B3}'
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this elevated -- it writes under Program Files and HKLM."
}

$running = Get-Process -Name 'pol','polboot','polcfg','ffxi*' -ErrorAction SilentlyContinue
if ($running) {
    throw ("Close these first, they hold the component open: " +
           (($running | ForEach-Object { "$($_.Name)($($_.Id))" }) -join ', '))
}

# The CLSID registration is the authoritative record of where a component lives.
$clsid = $CLSIDS[$Component]
$key = "HKLM:\SOFTWARE\Classes\WOW6432Node\CLSID\$clsid\InprocServer32"
if (-not (Test-Path $key)) { $key = "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32" }
if (-not (Test-Path $key)) { throw "CLSID $clsid is not registered; cannot locate $Component." }

$props    = Get-ItemProperty $key
$regValue = $props.'(default)'
$backup   = $props.PSObject.Properties['PolShimOriginal']

# Wherever the component really lives, ignoring any CLSID hook we installed.
$target = if ($backup) { $backup.Value } else { $regValue }
$dir    = Split-Path $target
$orig   = Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($target) + '.orig.dll')
$ini    = Join-Path $dir 'polshim.ini'

if ($Restore) {
    $did = @()
    if ($backup) {
        Set-ItemProperty $key -Name '(default)' -Value $backup.Value
        Remove-ItemProperty $key -Name 'PolShimOriginal'
        $did += "CLSID registration"
    }
    if (Test-Path $orig) {
        Remove-Item $target -Force -ErrorAction SilentlyContinue   # our shim copy
        Move-Item $orig $target -Force
        $did += "component file"
    }
    if (Test-Path $ini) { Remove-Item $ini -Force; $did += "polshim.ini" }
    if ($did) { Write-Host "[+] restored: $($did -join ', ')" -ForegroundColor Green }
    else      { Write-Host "[=] nothing was installed for $Component" -ForegroundColor Yellow }
    return
}

if (-not (Test-Path $ShimPath)) { throw "Shim not built: $ShimPath (run build.bat first)" }
$ShimPath = (Resolve-Path $ShimPath).Path

if ($Mode -eq 'Clsid') {
    if (-not $backup) {
        New-ItemProperty $key -Name 'PolShimOriginal' -Value $regValue -PropertyType String | Out-Null
    }
    Set-ItemProperty $key -Name '(default)' -Value $ShimPath
    $real = $target
    $iniDir = Split-Path $ShimPath
    $ini = Join-Path $iniDir 'polshim.ini'
    Write-Host "[+] $Component CLSID -> $ShimPath" -ForegroundColor Green
    Write-Host "    NOTE: the Viewer loads its components by path, so this only" -ForegroundColor Yellow
    Write-Host "          catches COM activators such as polcfg.exe." -ForegroundColor Yellow
} else {
    if (Test-Path $orig) {
        Write-Host "[=] already shimmed (original at $orig)" -ForegroundColor Yellow
    } else {
        Move-Item $target $orig
    }
    Copy-Item $ShimPath $target -Force
    $real = $orig
    Write-Host "[+] $Component  file-mode" -ForegroundColor Green
    Write-Host "    shim -> $target"
    Write-Host "    real -> $orig"
}

# The shim reads its config from beside itself, so the ini goes next to whichever
# copy will actually be loaded. Absolute log path: Program Files is not writable
# by the Viewer, and a silent logging failure is worse than none.
#
# EVERY key the shim reads is written here, explicitly, even where the value
# matches the built-in default. A reinstall overwrites this file wholesale, so a
# key omitted here is a key silently reset -- which is exactly how `probes` went
# missing and cost a measurement run. Adding a setting to
# inject.cpp means adding it here in the same change.
@"
[polshim]
real=$real
log=$LogPath
verbose=1
probes=1
max_slots=128

; Wire-level payload capture. This is the highest-yield signal we have -- it
; cracked the pp000 framing on its own -- so it ships on by default.
capture=1
capture_max=8192

; Measurement switches, all off for a fresh install -- each one changes what the
; client does, so they are opt-in per run. Written explicitly (not left to the
; built-in defaults) so a reinstall cannot silently reset one that was armed.
;   startup_state  serve a different usr/all/login_w.bin byte 0x6E in memory
;                  (0x19 = the dormant in-client sign-up wizard)
;   signup_http    route the sign-up wizard's HTTP at our server
;   acct_trace     PAGE_GUARD read-tracer over the lobby 3:0 u/account record
startup_state=0
signup_http=0
acct_trace=0

; Components whose DllGetClassObject export is hooked; required for both
; interposition and [replace].
modules=polcore.dll,app.dll,PolContents.dll

; Substitution: "{CLSID}=our.dll". Left empty by a fresh install so a new
; environment measures the stock client.
[replace]
"@ | Set-Content -Path $ini -Encoding ASCII

Write-Host "    ini  -> $ini"
Write-Host "    log  -> $LogPath"
Write-Host "    load proof -> $env:TEMP\polshim-attach.log"
