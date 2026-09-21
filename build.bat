@echo off
rem Build the PolHook.dll proxy (32-bit -- the POL Viewer process is x86) and the
rem one-file Windows installer PolShimSetup.exe that embeds it.
setlocal

rem Find Visual Studio instead of assuming an edition. This used to hardcode
rem ...\2022\Community\..., which is right on a developer PC and wrong on a
rem GitHub runner (Enterprise), so the CI build failed before compiling a line.
rem Order: a VCVARS you set yourself, then vswhere (ships with every VS 2017+
rem installer and with the Build Tools), then the old Community path.
if defined VCVARS if exist "%VCVARS%" goto :have_vcvars
set "VCVARS="
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :try_default
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if defined VSROOT set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat"
if defined VCVARS if exist "%VCVARS%" goto :have_vcvars
:try_default
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARS%" goto :have_vcvars
echo [!] No Visual Studio with the C++ x86 tools was found.
echo     Install "Desktop development with C++" (or the Build Tools), or run
echo     set VCVARS=^<path to vcvarsall.bat^>   before build.bat.
exit /b 1
:have_vcvars
echo [+] using "%VCVARS%"

call "%VCVARS%" x86 >nul
if errorlevel 1 exit /b 1

if not exist "%~dp0build" mkdir "%~dp0build"
pushd "%~dp0build"

rem PolHook.dll is a PROXY of the Viewer's own PolHook.dll, which pol.exe STATICALLY
rem imports -- so pol.exe loads the whole shim itself, no launcher and no admin.
rem PolHook.def forwards the original's 6 exports to the renamed PolHook_orig.dll.
rem dxhook.cpp adds the DirectDraw/DirectSound layer; dxguid.lib supplies
rem IID_IDirectDraw7 / IID_IDirectSound8 (referenced, never called).
set "SHIMSRC=..\src\inject.cpp ..\src\proxy.cpp ..\src\log.cpp ..\src\patches.cpp ..\src\fepatch.cpp ..\src\tmpathfix.cpp ..\src\authkey.cpp ..\src\dxhook.cpp ..\src\mailstore.cpp ..\src\comtrace.cpp ..\src\d3d8hook.cpp ..\src\d3d9hook.cpp ..\src\vidfit.cpp ..\src\fmvskip.cpp ..\src\wndguard.cpp ..\src\vmrfix.cpp ..\src\dinputhook.cpp ..\src\hookspy.cpp ..\src\inputgate.cpp ..\src\keystate.cpp ..\src\exitprompt.cpp ..\src\netredir.cpp ..\src\profiles.cpp ..\src\polfetch.cpp ..\src\fmokey.cpp ..\src\fmoime.cpp ..\src\msgxlate.cpp ..\src\uitrace.cpp ..\src\regredir.cpp ..\src\regfix.cpp ..\src\fmoiid.cpp ..\src\polfiletxt.cpp ..\src\iniheal.cpp ..\src\polsettings.cpp ..\src\gamecfg.cpp ..\src\logprune.cpp ..\src\patchver.cpp ..\src\inputmode.cpp ..\src\shortcut.cpp ..\src\padmap.cpp ..\src\padoverlay.cpp ..\src\titletag.cpp ..\src\maskguard.cpp ..\src\protondxvk.cpp ..\src\sessionwatch.cpp ..\src\autoupdate.cpp ..\src\poltoken.cpp ..\src\ffxiplug.cpp ..\src\ffxicfg.cpp ..\src\crashlog.cpp ..\src\reslock.cpp ..\src\flwindow.cpp ..\src\wakerecover.cpp ..\src\fecfg.cpp"
set "SHIMLIBS=kernel32.lib user32.lib advapi32.lib ole32.lib dxguid.lib strmiids.lib ws2_32.lib shlwapi.lib gdi32.lib shell32.lib crypt32.lib wininet.lib bcrypt.lib comctl32.lib comdlg32.lib"

rem PolHook.rc replicates the original PolHook.dll VS_VERSION_INFO: pol.exe
rem version-checks its DLLs, and a proxy with no version resource fails that check.
rem The forwarders name PolHook_orig.dll but do NOT need it present to LINK (a
rem forwarder is a string resolved at load time), so this builds standalone.
rc /nologo /fo PolHook.res ..\PolHook.rc
if errorlevel 1 ( popd & echo [!] PolHook.rc compile failed & exit /b 1 )
cl /nologo /LD /MT /O2 /W3 /EHsc /DWIN32 /D_CRT_SECURE_NO_WARNINGS ^
   %SHIMSRC% PolHook.res /Fe:PolHook.dll ^
   /link /DEF:..\PolHook.def %SHIMLIBS%

set RC=%ERRORLEVEL%
if %RC% NEQ 0 ( popd & echo [!] PolHook proxy build failed & exit /b %RC% )
echo [+] built %~dp0build\PolHook.dll

rem One-file Windows installer. setup.rc embeds PolHook.dll (from THIS build dir --
rem hence built last, after the proxy exists) plus the shipping ini template
rem (dist\polshim.ini), so the download is a single file that cannot go stale
rem against its payload. The manifest is requireAdministrator: the swap writes into
rem Program Files. /I ..\src lets rc find setup.manifest and buildnum.h.
rc /nologo /I ..\src /fo setup.res ..\setup.rc
if errorlevel 1 ( popd & echo [!] setup.rc compile failed & exit /b 1 )
cl /nologo /MT /O2 /W3 /EHsc /DWIN32 /D_CRT_SECURE_NO_WARNINGS ^
   ..\src\setup.cpp ..\src\polfiletxt.cpp setup.res /Fe:PolShimSetup.exe ^
   /link kernel32.lib user32.lib advapi32.lib wininet.lib bcrypt.lib /MANIFEST:NO

set RC=%ERRORLEVEL%
popd
if %RC% NEQ 0 ( echo [!] PolShimSetup build failed & exit /b %RC% )
echo [+] built %~dp0build\PolShimSetup.exe
exit /b 0
