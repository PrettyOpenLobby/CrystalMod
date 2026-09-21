#!/usr/bin/env bash
# =============================================================================
# PoL-Shim installer for Linux / Steam Deck (PlayOnline under Proton/Wine).
#
# pol.exe statically imports PolHook.dll, so dropping our proxy in its place makes
# pol.exe load the whole shim BY ITSELF -- no injector, no admin. The proxy forwards
# SE's exports to the renamed original (PolHook_orig.dll), so nothing SE does breaks.
#
#   install:  ./install.sh [path] [--server=<ip>]     (path auto-detected if omitted)
#   revert:   ./install.sh [path] --revert
#   options:  --server=<address>  set the server (else you're prompted); --no-update  skip self-update
#             --no-dxvk-d3d8  leave Proton's d3d8 on wined3d (see ensure_dxvk_d3d8)
#
# THIS SCRIPT IS THE WHOLE DOWNLOAD: if PolHook.dll / polshim.ini are not sitting
# next to it, it fetches them (hash-verified) from the project's latest GitHub
# release. A folder copy that DOES have them still works offline, as before.
#
# It auto-detects the Steam install, PROMPTS for your server (Enter = the default
# below), writes that server into polshim.ini so play uses it too (no Steam launch
# option needed), and self-updates the shim from the latest release before installing.
# Non-interactive? Set POLSHIM_SERVER=<ip> to skip the prompt.
# =============================================================================
set -euo pipefail

# ${BASH_SOURCE[0]:-$0}: BASH_SOURCE is empty when the script arrives on stdin
# (curl | bash), and set -u would trip on it; $0 is "bash" there, so HERE = cwd.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

# Default server offered at the prompt; Enter accepts it. Set
# POLSHIM_DEFAULT_SERVER=... in the environment to offer another, or set it EMPTY
# to offer none (the script then refuses to install without an address).
DEFAULT_SERVER="${POLSHIM_DEFAULT_SERVER-play.openlobby.fyi}"

die(){ echo "[!] $*" >&2; exit 1; }
ok(){  echo "[+] $*"; }
info(){ echo "[*] $*"; }

# The play/update server: --server=, else POLSHIM_SERVER env, else ALWAYS prompt.
# Reads from /dev/tty so the prompt works even when the script is piped (curl | bash).
# Returns empty when nothing was given; the caller refuses to install on that.
resolve_server() {
  if [ -n "${SERVER_ARG:-}" ];    then printf '%s' "$SERVER_ARG";    return; fi
  if [ -n "${POLSHIM_SERVER:-}" ]; then printf '%s' "$POLSHIM_SERVER"; return; fi
  local ans=""
  if [ -e /dev/tty ]; then
    if [ -n "$DEFAULT_SERVER" ]; then
      printf 'PlayOnline server address (IP or hostname) [%s]: ' "$DEFAULT_SERVER" > /dev/tty
    else
      printf 'PlayOnline server address (IP or hostname): ' > /dev/tty
    fi
    IFS= read -r ans < /dev/tty || true
  fi
  printf '%s' "$(printf '%s' "${ans:-$DEFAULT_SERVER}" | tr -d '[:space:]')"
}

# The shim reads [redirect] server= as a DOTTED IPv4 ADDRESS and nothing else
# (it parses it while the DLL is loading, where a DNS lookup is not safe). Both
# installers have always asked for "IP or hostname", so a typed hostname used to
# produce an install that armed nothing and connected nowhere, without a word.
# The name is therefore resolved HERE, once, and the address is what gets written.
# Prints the address, or nothing when the name does not resolve.
to_ipv4() {
  local name="$1" ip=""
  if printf '%s' "$name" | grep -Eq '^([0-9]{1,3}\.){3}[0-9]{1,3}$'; then
    printf '%s' "$name"; return 0
  fi
  if command -v getent >/dev/null 2>&1; then
    ip=$(getent ahostsv4 "$name" 2>/dev/null | awk 'NR==1 {print $1}')
  fi
  if [ -z "$ip" ] && command -v python3 >/dev/null 2>&1; then
    ip=$(python3 -c 'import socket,sys; print(socket.gethostbyname(sys.argv[1]))' "$name" 2>/dev/null || true)
  fi
  if [ -z "$ip" ] && command -v nslookup >/dev/null 2>&1; then
    ip=$(nslookup "$name" 2>/dev/null | awk '/^Address/ && NR>2 {print $2; exit}' | grep -E '^([0-9]{1,3}\.){3}[0-9]{1,3}$' || true)
  fi
  printf '%s' "$ip"
}

# Set `server=<val>` under [redirect] in an ini: replace an existing/commented server=,
# else insert it after the [redirect] header (adds the section if missing).
set_redirect_server() {
  local ini="$1" val="$2"
  [ -f "$ini" ] || printf '[redirect]\n' > "$ini"
  awk -v val="$val" '
    /^[[:space:]]*\[redirect\][[:space:]]*$/ { print; inr=1; next }
    /^[[:space:]]*\[/ { if (inr && !done) { print "server=" val; done=1 } inr=0; print; next }
    inr && /^[[:space:]]*[;#]?[[:space:]]*server[[:space:]]*=/ { if (!done) { print "server=" val; done=1 } next }
    { print }
    END { if (inr && !done) print "server=" val;
          if (!inr && !done) { print "[redirect]"; print "server=" val } }
  ' "$ini" > "$ini.tmp" && mv "$ini.tmp" "$ini"
}

# Add to an EXISTING ini every (section,key) the shipped template has and it lacks,
# at the template's default. A value already set is NEVER touched -- same rule as the
# in-DLL iniheal, so re-running this is safe and idempotent. Without it, updating the
# shim on an install that already had one leaves new options permanently invisible:
# the DLL's compiled default applies and there is nothing in the file to edit.
# Comments are not carried across (the template documents them).
# awk emits the count added as line 1, then the healed file; the shell splits them.
ini_heal() {
  local ini="$1" tpl="$2" out
  [ -f "$ini" ] && [ -f "$tpl" ] || return 1
  awk -v tpl="$tpl" -v ini="$ini" '
    function trim(s){ sub(/^[ 	]+/,"",s); sub(/[ 	]+$/,"",s); return s }
    BEGIN{
      SEP=""
      # --- template: the full set of (section,key)=default we ship ---
      sec=""
      while ((getline l < tpl) > 0) {
        l=trim(l)
        if (l=="" || l ~ /^[;#]/) continue
        if (l ~ /^\[.*\]$/) { sec=l; if(!(sec in tseen)){tseen[sec]=1; tord[++nsec]=sec}; continue }
        if (sec=="" || index(l,"=")==0) continue
        k=trim(substr(l,1,index(l,"=")-1)); v=trim(substr(l,index(l,"=")+1))
        if (match(v,/[ 	]+;/)) v=trim(substr(v,1,RSTART-1))   # strip trailing comment
        if (!((sec SEP k) in tval)) { tval[sec SEP k]=v; tkeys[sec]=tkeys[sec] (tkeys[sec]==""?"":SEP) k }
      }
      close(tpl)
      # --- existing ini: what it already has, and the last line of each section ---
      sec=""; n=0
      while ((getline l < ini) > 0) {
        line[++n]=l; t=trim(l)
        if (t ~ /^\[.*\]$/) { sec=t; hsec[sec]=1 }
        else if (sec!="" && t !~ /^[;#]/ && index(t,"=")>0) { have[sec SEP trim(substr(t,1,index(t,"=")-1))]=1 }
        # anchor inserts to the last NON-BLANK line of the section, so a trailing
        # blank line stays a separator instead of being pushed below the additions.
        # (no apostrophes in here -- the whole awk body is a single-quoted shell string)
        lsec[n]=sec; if (sec!="" && t!="") lend[sec]=n
      }
      close(ini)
      # --- build the healed file in out[], appending each section missing keys ---
      no=0; added=0
      for (i=1;i<=n;i++) {
        out[++no]=line[i]; s=lsec[i]
        if (s!="" && i==lend[s] && (s in tkeys)) {
          m=split(tkeys[s],a,SEP)
          for (j=1;j<=m;j++) if (!((s SEP a[j]) in have)) { out[++no]=a[j] "=" tval[s SEP a[j]]; added++ }
        }
      }
      # --- whole sections the ini does not have at all ---
      for (i=1;i<=nsec;i++) {
        s=tord[i]; if ((s in hsec) || !(s in tkeys)) continue
        out[++no]=""; out[++no]=s
        m=split(tkeys[s],a,SEP)
        for (j=1;j<=m;j++) { out[++no]=a[j] "=" tval[s SEP a[j]]; added++ }
      }
      print added
      for (i=1;i<=no;i++) print out[i]
    }' </dev/null > "$ini.heal" || { rm -f "$ini.heal"; return 1; }
  out=$(head -1 "$ini.heal")
  tail -n +2 "$ini.heal" > "$ini.tmp" && mv "$ini.tmp" "$ini"
  rm -f "$ini.heal"
  echo "${out:-0}"
}

# --- self-update: fetch a newer PolHook.dll (+ this script) from your server -----
# Triggered by the published DLL's hash differing from the local one, so ANY new
# build updates -- not just version bumps. Fails safe (offline -> keep local).
self_update() {
  [ -n "${NO_UPDATE:-}" ] && return 0
  command -v curl      >/dev/null 2>&1 || return 0
  command -v sha256sum >/dev/null 2>&1 || return 0
  [ -f "$HERE/PolHook.dll" ] || return 0
  [ -n "$BASE_URL" ] || return 0

  local remote_hash local_hash
  remote_hash=$(curl -fsSL --max-time 10 "$BASE_URL/PolHook.dll.sha256" 2>/dev/null | awk '{print $1}') || return 0
  [ -n "$remote_hash" ] || return 0
  local_hash=$(sha256sum "$HERE/PolHook.dll" | awk '{print $1}')
  [ "$remote_hash" = "$local_hash" ] && return 0

  info "a newer shim is published -- updating local files..."
  local tmp; tmp=$(mktemp -d); local f fail=0
  for f in PolHook.dll PolHook.dll.sha256 install.sh polshim.ini; do
    curl -fsSL --max-time 120 "$BASE_URL/$f" -o "$tmp/$f" 2>/dev/null || { fail=1; break; }
  done
  # optional: a README is a nicety, and a source without one must still update
  curl -fsSL --max-time 60 "$BASE_URL/README.md" -o "$tmp/README.md" 2>/dev/null || rm -f "$tmp/README.md"
  if [ "$fail" = 0 ] && [ "$(sha256sum "$tmp/PolHook.dll" | awk '{print $1}')" = "$remote_hash" ]; then
    cp -f "$tmp/PolHook.dll" "$tmp/PolHook.dll.sha256" "$tmp/polshim.ini" "$HERE/"
    [ -f "$tmp/README.md" ] && cp -f "$tmp/README.md" "$HERE/"
    cp -f "$tmp/install.sh" "$HERE/install.sh"; chmod +x "$HERE/install.sh"
    rm -rf "$tmp"
    ok "updated -- re-running the new installer"
    NO_UPDATE=1 exec "$HERE/install.sh" "$@"
  fi
  rm -rf "$tmp"
  info "self-update skipped (offline or download/verify failed) -- using local version"
}

# --- bootstrap: no payload next to the script -> pull it from the server ---------
# Makes the whole install a ONE-file download (just this script, or even a straight
# curl | bash): PolHook.dll and polshim.ini come from the same server the shim will
# play against, and the DLL must match its published sha256 before it is installed.
# Sets SRC to the staging dir the install steps below copy from.
fetch_payload() {
  [ -n "$BASE_URL" ] || die "PolHook.dll is not next to this script, and no server is known to fetch it from.
  Re-run with --server=<ip> (or POLSHIM_SERVER=<ip> in the environment)."
  command -v curl      >/dev/null 2>&1 || die "curl is needed to download the shim (or put PolHook.dll + polshim.ini next to this script)."
  command -v sha256sum >/dev/null 2>&1 || die "sha256sum is needed to verify the download (or put PolHook.dll + polshim.ini next to this script)."

  info "shim files not found next to the script -- fetching them from $BASE_URL ..."
  SRC=$(mktemp -d); trap 'rm -rf "$SRC"' EXIT
  local f
  for f in PolHook.dll PolHook.dll.sha256 polshim.ini; do
    curl -fsSL --max-time 120 "$BASE_URL/$f" -o "$SRC/$f" \
      || die "download failed: $BASE_URL/$f
  Is the server reachable and serving the shim? (check: curl -I $BASE_URL/PolHook.dll)"
  done
  local want got
  want=$(awk '{print $1}' "$SRC/PolHook.dll.sha256")
  got=$(sha256sum "$SRC/PolHook.dll" | awk '{print $1}')
  [ -n "$want" ] && [ "$want" = "$got" ] \
    || die "downloaded PolHook.dll does not match its published sha256 -- not installing it."
  ok "downloaded PolHook.dll + polshim.ini (hash verified)"
  # Best-effort, and NOT part of the loop above: a server that predates this file
  # would 404, and the loop dies on a failed download. The shim installs perfectly
  # well without it -- only the Steam-shortcuts extra is skipped -- so a miss here
  # must never turn into a failed install.
  curl -fsSL --max-time 60 "$BASE_URL/polshortcuts.py" -o "$SRC/polshortcuts.py" 2>/dev/null \
    || info "polshortcuts.py not offered by this server -- skipping the Steam-shortcuts extra"
  curl -fsSL --max-time 60 "$BASE_URL/rdthosts.py" -o "$SRC/rdthosts.py" 2>/dev/null \
    || info "rdthosts.py not offered by this server -- skipping the sign-up allow-list"
  NO_UPDATE=1   # what we just fetched IS the published build; self-update would re-download it
}

# --- Steam entries for each title, so Game Mode can launch them directly --------
#
# SE ships a per-title polboot.exe -- Tetra Master's own shortcut, FFXI's, and so on.
# This registers each one as a non-Steam game so they appear in the library instead
# of only being reachable through the Viewer's menu.
#
# IMPORTANT: IT CANNOT RUN WHILE STEAM IS UP, and that is not a caution, it is the whole
# design constraint. Steam holds the shortcut list in MEMORY and writes
# shortcuts.vdf from it on exit, so an edit made while Steam is running is not
# racy -- it is discarded, silently, with no error and no entry. That is also why
# this can never be a button inside the shim's settings dialog: that dialog only
# exists inside a game Steam launched, so Steam is running by definition. Hence a
# desktop launcher, which is used from Desktop mode with Steam closed.
#
# The entries point at the EXISTING prefix via STEAM_COMPAT_DATA_PATH. Without that
# each non-Steam game gets its own empty prefix with no PlayOnline in it.
install_steam_shortcuts() {
  local py="$SRC/polshortcuts.py"
  [ -f "$py" ] || return 0
  command -v python3 >/dev/null 2>&1 || { info "no python3 -- skipping Steam shortcuts"; return 0; }

  # As root this would put pol-config and the desktop entry under /root (where the
  # player never sees them) and edit shortcuts.vdf as root -- a root-owned file in
  # the user's Steam userdata, which Steam then cannot rewrite. Skip instead: the
  # shim is fully installed by this point and this step is pure convenience.
  if [ "$(id -u)" = 0 ]; then
    echo "[~] running as root -- SKIPPING the Steam shortcuts step (it would write"
    echo "    root-owned files into ${DIR_OWNER%%:*}'s Steam folder)."
    echo "    Add them later as your normal user:  ./install.sh \"$DIR\""
    return 0
  fi

  local dest="$HOME/pol-config"
  mkdir -p "$dest"
  cp -f "$py" "$dest/polshortcuts.py"
  chmod +x "$dest/polshortcuts.py"
  ok "Installed $dest/polshortcuts.py"

  # A wrapper script, not an inline Exec=. The desktop-entry spec reserves ';' and
  # quotes inside Exec and does not expand $HOME, so the obvious one-liner is
  # invalid -- desktop-file-validate rejects it outright. A script keeps Exec to a
  # single absolute path, which is what the spec actually wants.
  cat > "$dest/pol-add-shortcuts.sh" <<'WRAP'
#!/usr/bin/env bash
# Add every installed PlayOnline title to Steam as a non-Steam game.
# Steam MUST be closed: it rewrites shortcuts.vdf from memory on exit, so anything
# added while it is running is discarded without an error.
cd "$(dirname "$0")" || exit 1
python3 ./polshortcuts.py --install --apply
status=$?
echo
if [ $status -ne 0 ]; then
    echo "Did not finish (exit $status). If it says Steam is running, quit Steam and re-run."
fi
read -r -p "Press Enter to close..." _
WRAP
  chmod +x "$dest/pol-add-shortcuts.sh"

  local apps="$HOME/.local/share/applications"
  mkdir -p "$apps"
  cat > "$apps/pol-steam-shortcuts.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=POL Add Steam Shortcuts
Comment=Add each PlayOnline title to Steam as a non-Steam game (close Steam first)
Exec=$dest/pol-add-shortcuts.sh
Path=$dest
Terminal=true
Categories=Game;
DESKTOP
  ok "Added the 'POL Add Steam Shortcuts' desktop entry"

  # Offer to do it now, but only when it can actually succeed.
  if pgrep -x steam >/dev/null 2>&1; then
    echo "[~] Steam is running, so the shortcuts were NOT added -- Steam would"
    echo "    overwrite them on exit. Quit Steam, then run 'POL Add Steam Shortcuts'"
    echo "    from your application menu (or: python3 $dest/polshortcuts.py --install --apply)."
  else
    info "Adding the PlayOnline titles to Steam ..."
    python3 "$dest/polshortcuts.py" --install --apply || \
      echo "[~] shortcuts step failed -- the shim itself is installed and fine."
  fi
}

# --- whose home holds the Steam install ------------------------------------------
#
# ROOT IS THE #1 REASON AUTO-DETECT FAILS. Run under sudo or from a root shell,
# $HOME becomes /root, every Steam path below is missing, and the script reports
# "couldn't auto-detect a PlayOnline install" on a machine where the install is
# sitting in /home/deck. The shim needs no admin at all (pol.exe loads the proxy
# itself), so root is never required -- but people land in a root shell for the
# Tailscale/Decky steps and run it from there.
#
# So: probe $HOME, the invoking user's home when we got here via sudo, and every
# /home/* when we are root. Emits one path per line, deduped, existing only.
user_homes() {
  { [ -n "${HOME:-}" ] && printf '%s\n' "$HOME"
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
      getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6
    fi
    if [ "$(id -u)" = 0 ]; then
      local h; for h in /home/*; do [ -d "$h" ] && printf '%s\n' "$h"; done
    fi
  } | awk 'NF && !seen[$0]++'
}

# --- auto-detect the PlayOnlineViewer folder under the Steam install ------------
find_install() {
  local roots=() h m
  while IFS= read -r h; do
    roots+=("$h/.local/share/Steam/steamapps"
            "$h/.steam/steam/steamapps"
            "$h/.steam/root/steamapps"
            "$h/.var/app/com.valvesoftware.Steam/data/Steam/steamapps")
  done < <(user_homes)
  # SD cards and external drives. SteamOS has used BOTH layouts: the older
  # /run/media/<label>/ and the current /run/media/<user>/<label>/, so a single
  # glob at one depth silently misses half the Decks in the wild.
  for m in /run/media/*/steamapps /run/media/*/*/steamapps; do
    [ -d "$m" ] && roots+=("$m")
  done

  # Prefer a path containing PlayOnlineViewer; fall back to any pol.exe.
  local r hit
  for r in "${roots[@]}"; do
    [ -d "$r" ] || continue
    hit=$(find "$r" -iname 'pol.exe' -type f 2>/dev/null | grep -i 'PlayOnlineViewer' | head -1) || true
    [ -z "$hit" ] && hit=$(find "$r" -iname 'pol.exe' -type f 2>/dev/null | head -1) || true
    [ -n "$hit" ] && { dirname "$hit"; return 0; }
  done
  return 1
}

# --- the sign-up allow-list: let the Viewer accept our "add this member" block ----
#
# When you finish registering, the server answers with an RDT block that makes
# the Viewer create your login-screen member ITSELF -- ID and password saved, no
# typing. The client refuses that block unless its source URL passes a gate
# (app.dll 0x4a656f8): either an https URL on a *.pol.com host, or a prefix
# listed in the install's own usr/all/url/rdthosts.bin.
#
# We serve sign-up over PLAIN HTTP (https dies at POL-1328 on the certificate,
# unresolved after six live cycles), so tier 1 is closed to us and the shipped
# allow-list -- SE's two entries, neither of them ours -- closes tier 2. The
# block is then discarded WHOLE, NEXT-URL included, and the player is bounced
# back with an error code.
#
# That is not hypothetical: it happened to three real players (2026-08-22/25/29)
# before this ran anywhere. The edit existed the whole time as a hand-run tool on
# ONE machine, which is exactly why it never reached anybody. It belongs here,
# where every install we touch gets it.
#
# Safe to run every time: rdthosts.py is idempotent, keeps SE's own entries,
# takes a .orig backup, and verifies the image reads back as asked before
# writing. NEVER fatal -- the shim is fine without it and sign-up still works,
# you just have to add the member by hand.
#
# WARNING: A Viewer profile reset re-copies default/ over usr/ and silently drops this.
# Re-running the installer restores it.
allow_signup_rdt() {
  local dir="$1" py="$SRC/rdthosts.py"
  [ -f "$py" ] || return 0
  command -v python3 >/dev/null 2>&1 || { info "no python3 -- skipping the sign-up allow-list"; return 0; }
  [ -f "$dir/usr/all/url/rdthosts.bin" ] || {
    info "no usr/all/url/rdthosts.bin here -- skipping the sign-up allow-list"; return 0; }

  # Both forms the live route can take: the shim's signup_http lever rewrites
  # the client's own builder to http on :80, and the on-disk lever uses :8080.
  # Adding both costs 30 bytes and removes a whole class of
  # "worked on my machine".
  if python3 "$py" "$dir" \
       --add "http://ucs.pol.com/pml-cgi-bin/" \
       --add "http://ucs.pol.com:8080/pml-cgi-bin/" >/dev/null 2>&1; then
    fix_owner "$dir/usr/all/url/rdthosts.bin" "$dir/usr/all/url/rdthosts.bin.orig"
    ok "Sign-up allow-list updated (the Viewer can auto-add your member)"
  else
    echo "[~] could not update the sign-up allow-list -- registration still works,"
    echo "    you will just have to add the member by hand with Add Member."
  fi
}

# --- MFC42: the FMO config app needs it and a Proton prefix ships none ------------
#
# FrontMissionOnlineConfig.exe is the SHIPPED writer for FMO's gamepad map
# (GamePadAssin0) -- the only supported way to rebind those buttons. Without
# MFC42.DLL it exits INSTANTLY: `err:module:import_dll Library MFC42.DLL not found`
# then `loader_init ... status c0000135`. With WINEDEBUG quiet you see NOTHING at
# all, just a shortcut that appears to do nothing, which is how this cost an
# afternoon. It is the only one of the seven config tools that needs it.
#
# Order below is cheapest-legitimate-first. winetricks installs MICROSOFT'S own
# redistributable from Microsoft, so nothing has to be redistributed by this
# project; the local/server copy is only a fallback for a machine without it.
#
# Installed APP-LOCAL, beside the exe, never into the prefix's system32: it affects
# that one tool and leaves the prefix and every game untouched.
find_prefix() {
  local sa="${1%%/steamapps/*}/steamapps"
  [ -d "$sa/compatdata" ] || return 1
  local d
  for d in "$sa"/compatdata/*/pfx; do
    [ -f "$d/system.reg" ] || continue
    grep -qa 'PlayOnline' "$d/system.reg" 2>/dev/null && { echo "$d"; return 0; }
  done
  return 1
}

ensure_mfc42() {
  local pov="$1"                       # the PlayOnlineViewer folder
  local se fmo have pfx tricks
  se="$(dirname "$pov")"
  fmo="$(find "$se" -maxdepth 1 -iname 'FRONT MISSION ONLINE' -type d 2>/dev/null | head -1)"
  [ -n "$fmo" ] || return 0            # FMO not installed -- nothing to do

  have="$(find "$fmo" -maxdepth 1 -iname 'mfc42.dll' 2>/dev/null | head -1)"
  [ -n "$have" ] && { ok "MFC42.DLL present -- FMO config app can run"; return 0; }

  pfx="$(find_prefix "$pov" || true)"
  # A 64-bit prefix puts 32-BIT dlls in syswow64, and POL is 32-bit -- winetricks
  # extracts there, so checking only system32 reports "missing" on a prefix that
  # already has it and reinstalls every run.
  if [ -n "$pfx" ] && { [ -f "$pfx/drive_c/windows/syswow64/mfc42.dll" ]                      || [ -f "$pfx/drive_c/windows/system32/mfc42.dll" ]; }; then
    ok "MFC42.DLL already in the prefix -- FMO config app can run"; return 0
  fi

  # protontricks takes a STEAM APPID, winetricks takes a WINEPREFIX -- and on a Steam
  # Deck protontricks is normally a FLATPAK, so `command -v protontricks` finds
  # nothing even though it is installed. Check all three.
  local appid=""
  case "$pfx" in */compatdata/*/pfx) appid="${pfx%/pfx}"; appid="${appid##*/}" ;; esac

  if [ -n "$appid" ] && command -v protontricks >/dev/null 2>&1; then
    info "Installing MFC42 with protontricks (Microsoft's own redistributable)..."
    protontricks "$appid" -q mfc42 >/dev/null 2>&1 \
      && { ok "MFC42 installed -- the FMO config app will run"; return 0; }
    echo "[~] protontricks could not install mfc42."
  fi

  if [ -n "$appid" ] && command -v flatpak >/dev/null 2>&1 \
     && flatpak info com.github.Matoking.protontricks >/dev/null 2>&1; then
    info "Installing MFC42 with the Protontricks flatpak (Microsoft's own redistributable)..."
    flatpak run com.github.Matoking.protontricks "$appid" -q mfc42 >/dev/null 2>&1 \
      && { ok "MFC42 installed -- the FMO config app will run"; return 0; }
    echo "[~] the Protontricks flatpak could not install mfc42."
  fi

  if [ -n "$pfx" ] && command -v winetricks >/dev/null 2>&1; then
    info "Installing MFC42 with winetricks (Microsoft's own redistributable)..."
    WINEPREFIX="$pfx" winetricks -q mfc42 >/dev/null 2>&1 \
      && { ok "MFC42 installed -- the FMO config app will run"; return 0; }
    echo "[~] winetricks could not install mfc42."
  fi

  # Fallback: a copy sitting next to this script, or one the server chose to host.
  local src=""
  [ -f "$SRC/mfc42.dll" ] && src="$SRC/mfc42.dll"
  if [ -z "$src" ] && [ -n "$SERVER" ] && [ "$SERVER" != "CHANGE-ME" ]; then
    if curl -fsSL --max-time 30 -o "$SRC/mfc42.dll.tmp" "http://$SERVER/shim/dist/mfc42.dll" 2>/dev/null \
       && [ -s "$SRC/mfc42.dll.tmp" ]; then
      mv -f "$SRC/mfc42.dll.tmp" "$SRC/mfc42.dll"; src="$SRC/mfc42.dll"
    else
      rm -f "$SRC/mfc42.dll.tmp"
    fi
  fi

  if [ -n "$src" ]; then
    cp -f "$src" "$fmo/mfc42.dll" && ok "Installed MFC42.DLL beside the FMO config app"
    return 0
  fi

  echo "[~] MFC42.DLL is missing, so Front Mission Online's GAMEPAD CONFIG will not"
  echo "    open (it exits instantly with no error). Everything else works. Fix with:"
  echo "        winetricks -q mfc42        # or: protontricks 230330 -q mfc42"
}

# --- d3d8 -> DXVK: the one Proton setting that decides POL's framerate ------------
#
# Every stock Proton serves Direct3D 8 through wined3d (OpenGL) UNLESS
# PROTON_DXVK_D3D8=1. DXVK's d3d8 front-end (d8vk) ships inside the same Proton --
# it is merely OPT-IN. Every POL title that matters here is d3d8 (Tetra Master,
# Fantasy Earth, FFXI), so all of them take the slow path by default, while FMO --
# the one d3d9 title, and the one that always felt healthiest on a Deck -- has been
# on DXVK the whole time. Measured on a Deck 2026-09-07: Tetra Master issues
# 320-420 draw calls per FRAME and `wined3d_cs` is the hottest thread in the process.
#
# Identify which path a prefix is on by SIZE, in compatdata/<appid>/pfx/drive_c/
# windows/syswow64/ (a 32-bit title loads syswow64, not system32):
#     wined3d builtin d3d8.dll  ~313 KB        <- the slow path
#     Proton 11's DXVK d3d8.dll  ~1,683 KB     <- what we want
#
# IMPORTANT: THE KNOB IS PER-PROTON-INSTALL, AND SWITCHING PROTON SILENTLY LOSES IT.
# It was set by hand on 2026-08-20 in "Proton 10.0/user_settings.py". The title later
# moved to Proton 11.0, whose user_settings.py EXISTS but does not carry the key --
# so the fix quietly stopped running for eleven days and nobody could see it. That is
# exactly why this writes to EVERY Proton install it finds rather than the active one.
#
# Steam Launch Options would also work and are deliberately NOT used: this project
# keeps them empty (a malformed `PROTON_LOG=%command%` left there once cost an
# evening), and user_settings.py is the documented control on these machines.
proton_dirs() {
  local h d
  {
    while IFS= read -r h; do
      ls -d "$h"/.local/share/Steam/steamapps/common/Proton* \
            "$h"/.steam/steam/steamapps/common/Proton* \
            "$h"/.steam/root/steamapps/common/Proton* \
            "$h"/.local/share/Steam/compatibilitytools.d/* \
            "$h"/.steam/root/compatibilitytools.d/* 2>/dev/null || true
    done < <(user_homes)
    ls -d /run/media/*/steamapps/common/Proton* \
          /run/media/*/*/steamapps/common/Proton* 2>/dev/null || true
  } | while IFS= read -r d; do
    # ~/.steam/steam, ~/.steam/root and ~/.local/share/Steam are SYMLINKS to one
    # place on a Deck, so the raw globs list the same Proton two or three times and
    # this would "already"-report its own edit. Resolve, then dedupe.
    readlink -f "$d" 2>/dev/null || echo "$d"
  done | sort -u
}

# Merge the key into ONE Proton's user_settings.py. Echoes what it did; never edits a
# file it does not recognise, and never removes a setting the user put there.
set_dxvk_d3d8_in() {
  # Declared then assigned, NOT `local pd="$1" us="$pd/..."`: bash expands every word
  # of a `local` before the builtin assigns any of them, so the second one reads an
  # unset `pd` and `set -u` aborts the install. Caught on a Deck 2026-09-07.
  local pd us
  pd="$1"; us="$pd/user_settings.py"
  [ -w "$pd" ] || { echo "unwritable"; return 0; }
  if [ -f "$us" ]; then
    grep -q 'PROTON_DXVK_D3D8' "$us" && { echo "already"; return 0; }
    # Anchor on the dict opener. If this file is shaped some other way it is
    # somebody's hand-written Python and we leave it strictly alone.
    grep -qE '^[[:space:]]*user_settings[[:space:]]*=[[:space:]]*\{' "$us" \
      || { echo "unrecognised"; return 0; }
    cp -n "$us" "$us.bak-polshim" 2>/dev/null || true
    sed -i '0,/^[[:space:]]*user_settings[[:space:]]*=[[:space:]]*{/s//&\n    "PROTON_DXVK_D3D8": "1",   # PoL-Shim: d3d8 -> DXVK (Vulkan)/' "$us"
  else
    printf '%s\n' \
      'user_settings = {' \
      '    "PROTON_DXVK_D3D8": "1",   # PoL-Shim: d3d8 -> DXVK (Vulkan)' \
      '}' > "$us"
    fix_owner "$us"
  fi
  # A GATE, not a printed check: the edit only counts if the file still PARSES and
  # the key really resolves to "1". Anything else puts the original back -- a Proton
  # that cannot read its own user_settings.py refuses to launch the game at all.
  if command -v python3 >/dev/null 2>&1; then
    if ! python3 -c 'import sys; ns={}; exec(open(sys.argv[1]).read(), ns); sys.exit(0 if ns.get("user_settings",{}).get("PROTON_DXVK_D3D8")=="1" else 1)' "$us" 2>/dev/null; then
      if [ -f "$us.bak-polshim" ]; then mv -f "$us.bak-polshim" "$us"; else rm -f "$us"; fi
      echo "failed"; return 0
    fi
  else
    grep -q 'PROTON_DXVK_D3D8' "$us" || { echo "failed"; return 0; }
  fi
  echo "set"
}

ensure_dxvk_d3d8() {
  [ "${NO_DXVK_D3D8:-0}" = 1 ] && { info "leaving d3d8/DXVK alone (--no-dxvk-d3d8)"; return 0; }
  local d done_n=0 warn_n=0 r
  while IFS= read -r d; do
    [ -f "$d/proton" ] || continue                                     # not a Proton runtime
    [ -f "$d/files/lib/wine/dxvk/i386-windows/d3d8.dll" ] || continue  # this Proton has no d8vk
    r="$(set_dxvk_d3d8_in "$d")"
    case "$r" in
      set)     ok "d3d8 -> DXVK enabled for $(basename "$d")"; done_n=$((done_n+1)) ;;
      already) done_n=$((done_n+1)) ;;
      *)       echo "[~] could not enable d3d8->DXVK in $(basename "$d") ($r)"; warn_n=$((warn_n+1)) ;;
    esac
  done < <(proton_dirs)

  if [ "$done_n" -gt 0 ]; then
    ok "d3d8 -> DXVK active on $done_n Proton install(s) -- takes effect next launch"
  elif [ "$warn_n" -eq 0 ]; then
    echo "[~] no Proton with DXVK d3d8 found -- Tetra Master / FFXI / Fantasy Earth will"
    echo "    run on wined3d (slower). Proton 9 or newer ships it; update Proton and re-run."
  fi
}

revert_dxvk_d3d8() {
  local d us
  while IFS= read -r d; do
    us="$d/user_settings.py"
    [ -f "$us" ] || continue
    grep -q 'PoL-Shim: d3d8 -> DXVK' "$us" || continue    # not a line we wrote
    sed -i '/PoL-Shim: d3d8 -> DXVK/d' "$us"
    # Delete only a file that is now an EMPTY dict -- i.e. one WE created. Anything
    # with keys left is the user's. This first tested for a double-quoted key line,
    # which would have DELETED a hand-written file that quotes with ' (all four on
    # this Deck do). Accept either quote style.
    if ! grep -qE "^[[:space:]]*['\"]" "$us"; then rm -f "$us"; fi
    ok "d3d8 -> DXVK removed from $(basename "$d")"
  done < <(proton_dirs)
}

# --- args -----------------------------------------------------------------------
DIR=""; MODE="install"; SERVER_ARG=""
for a in "$@"; do
  case "$a" in
    --revert|revert)  MODE="revert" ;;
    --no-update)      NO_UPDATE=1 ;;
    --no-dxvk-d3d8)   NO_DXVK_D3D8=1 ;;
    --server=*)       SERVER_ARG="${a#--server=}" ;;
    -*)               die "unknown option: $a (use --server=<ip> / --revert / --no-update / --no-dxvk-d3d8)" ;;
    *)                DIR="$a" ;;
  esac
done

# Resolve the server (install mode only): --server=, env, else prompt. No server,
# no install: the shim is a router, and a copy pointed at nobody connects nowhere.
SERVER=""
if [ "$MODE" = "install" ]; then
  SERVER="$(resolve_server)"
  [ -n "$SERVER" ] || die "A server address is required. Re-run with --server=<address>, set POLSHIM_SERVER=<address> in the environment, or type one at the prompt."
fi

# Self-update and download source. Default: the project's latest GitHub release,
# NOT the game server -- an operator's /shim/dist may hold a private or modified
# build, and their players should not be handed it for pointing the shim there.
# POLSHIM_UPDATE_URL=<url> overrides; POLSHIM_UPDATE_URL=server restores the old
# http://<server>/shim/dist for an operator who hosts their own build.
GITHUB_BASE="https://github.com/PrettyOpenLobby/CrystalMod/releases/latest/download"
BASE_URL="${POLSHIM_UPDATE_URL:-$GITHUB_BASE}"
if [ "$BASE_URL" = "server" ]; then
  BASE_URL=""
  [ -n "$SERVER" ] && [ "$SERVER" != "CHANGE-ME" ] && BASE_URL="http://${SERVER}/shim/dist"
fi

# Where the payload (PolHook.dll / polshim.ini) is copied FROM: next to the script
# if it's there, else a staging dir fetch_payload fills from the server.
SRC="$HERE"
if [ "$MODE" = "install" ] && { [ ! -f "$HERE/PolHook.dll" ] || [ ! -f "$HERE/polshim.ini" ]; }; then
  fetch_payload
fi

# Self-update first (install path only -- a revert is a purely local operation).
[ "$MODE" = "install" ] && self_update "$@"

# Resolve the install directory.
if [ -z "$DIR" ]; then
  info "no folder given -- searching your Steam install for pol.exe..."
  if ! DIR=$(find_install); then
    # Name the root case explicitly. Searched-as-root is the common failure and
    # the message used to send people hunting for a path that was never missing.
    [ "$(id -u)" = 0 ] && echo "[!] you are running as ROOT -- the shim needs no admin," >&2 &&
      echo "    and as root \$HOME is /root, so a normal user's Steam folder is invisible." >&2 &&
      echo "    Try again as your normal user (on a Steam Deck that is 'deck')." >&2
    echo "[i] homes searched: $(user_homes | tr '\n' ' ')" >&2
    die "couldn't auto-detect a PlayOnline install.
  Pass the folder explicitly:   ./install.sh \"/path/to/PlayOnlineViewer\"
  (find it with:  find /home /run/media -iname pol.exe 2>/dev/null )"
  fi
  ok "detected install: $DIR"
fi
[ -d "$DIR" ] || die "not a directory: $DIR"

# --- running as root: hand every file we write back to the folder's owner --------
#
# The shim needs no admin, but if someone DOES run this as root over a normal
# user's install, every file we drop lands root-owned inside their Steam folder.
# The game still reads them, so this looks fine -- until the next run as that
# user, which cannot overwrite its own PolHook.dll or polshim.ini and fails in a
# way that has nothing to do with the shim. Cheap to prevent, expensive to debug.
DIR_OWNER=""
if [ "$(id -u)" = 0 ]; then
  DIR_OWNER="$(stat -c '%U:%G' "$DIR" 2>/dev/null)" || DIR_OWNER=""
  case "$DIR_OWNER" in
    root:*|"") DIR_OWNER="" ;;
    *) echo "[~] running as root on a folder owned by ${DIR_OWNER%%:*} --"
       echo "    files written here will be handed back to ${DIR_OWNER%%:*}." ;;
  esac
fi
fix_owner(){ [ -n "$DIR_OWNER" ] && chown "$DIR_OWNER" "$@" 2>/dev/null || true; }

# Wine's filesystem is case-sensitive on Linux; find the real casing of each file.
find_ci(){ find "$DIR" -maxdepth 1 -iname "$1" 2>/dev/null | head -1; }

# Is this file OUR proxy rather than SE's DLL? Our build carries the "PoL-Shim" title
# tag; SE's does not (verified against the US PolHook.dll, PolHook_orig.dll and the
# EU/JP polhook.dll -- 0 hits in each, 2 in ours). Used to refuse the two arrangements
# that would create a proxy-forwards-to-proxy loop.
is_our_proxy(){ [ -n "$1" ] && [ -f "$1" ] && grep -qa "PoL-Shim" "$1" 2>/dev/null; }

POL="$(find_ci pol.exe)"
[ -n "$POL" ] || die "pol.exe not found in '$DIR' -- is this the PlayOnlineViewer folder?"
HOOK="$(find_ci PolHook.dll)"
ORIG="$(find_ci PolHook_orig.dll)"

# --- revert ---------------------------------------------------------------------
if [ "$MODE" = "revert" ]; then
  [ -n "$ORIG" ] || die "No PolHook_orig.dll in '$DIR' -- proxy not installed here."
  [ -n "$HOOK" ] && rm -f "$HOOK"
  mv "$ORIG" "$DIR/PolHook.dll"
  ok "Reverted: SE's PolHook.dll restored."
  revert_dxvk_d3d8 || true      # never fatal: the shim is already out
  exit 0
fi

# --- install --------------------------------------------------------------------
[ -f "$SRC/PolHook.dll" ] || die "PolHook.dll missing next to this script."

if [ -n "$ORIG" ]; then
  # Guard against the one arrangement that bricks the install: PolHook_orig.dll being
  # a copy of OUR proxy. The proxy forwards SE's six exports to PolHook_orig.dll, so
  # proxy-forwarding-to-proxy is a loop with no real implementation at the end.
  if is_our_proxy "$ORIG"; then
    die "PolHook_orig.dll is OUR proxy, not SE's DLL -- that is a forwarding loop.
    Restore SE's file first:  ./install.sh --revert   then re-run this.
    If revert cannot help, SE's original may be at ${ORIG%_orig.dll}.dll.preproxy.bak"
  fi
  echo "[~] PolHook_orig.dll already present -- updating the proxy only (backup kept)."
  # The Viewer's own Check Files / repair restores SE's PolHook.dll over ours, which
  # silently removes the whole shim -- no title-bar tag, no settings chord. Name it,
  # because from the outside it just looks like the shim broke.
  if [ -n "$HOOK" ] && ! is_our_proxy "$HOOK"; then
    echo "[~] the installed PolHook.dll is SE's, not ours -- the Viewer's updater or"
    echo "    'Check Files' has overwritten the shim since last time. Reinstalling it."
  fi
else
  [ -n "$HOOK" ] || die "No PolHook.dll in '$DIR' -- not a PlayOnline Viewer install?"
  # Never promote our own proxy to PolHook_orig.dll. This happens if PolHook_orig.dll
  # is deleted while the proxy is installed, and it produces the loop described above.
  if is_our_proxy "$HOOK"; then
    die "PolHook.dll here is already OUR proxy but PolHook_orig.dll is missing, so SE's
    original cannot be recovered from this folder. Restore it from
    PolHook.dll.preproxy.bak, or verify the game files in Steam, then re-run."
  fi
  cp -f "$HOOK" "$HOOK.preproxy.bak"          # extra safety copy
  mv "$HOOK" "$DIR/PolHook_orig.dll"
  fix_owner "$HOOK.preproxy.bak" "$DIR/PolHook_orig.dll"
  ok "Backed up SE PolHook.dll -> PolHook_orig.dll"
fi

cp -f "$SRC/PolHook.dll" "$DIR/PolHook.dll"
fix_owner "$DIR/PolHook.dll"
ok "Installed proxy -> $DIR/PolHook.dll"

allow_signup_rdt "$DIR"

ensure_mfc42 "$DIR"

# Never fatal, same rule as MFC42: the shim is installed and working by this point,
# and a Proton we cannot write to must not fail the install -- it only costs speed.
ensure_dxvk_d3d8 || true

INI="$(find_ci polshim.ini)"; [ -z "$INI" ] && INI="$DIR/polshim.ini"
if [ ! -f "$INI" ]; then
  cp -f "$SRC/polshim.ini" "$INI"
  fix_owner "$INI"
  ok "Wrote polshim.ini (gamepad on, diagnostics off)"
else
  # An ini already here is the USER'S -- keep every value they set, but add any option
  # this shim ships that their file predates. Without this an update is DLL-only and a
  # new default (e.g. [regfix] enable) can never reach an install that already had one.
  ADDED="$(ini_heal "$INI" "$SRC/polshim.ini" 2>/dev/null)" || ADDED=""
  case "$ADDED" in
    ''|0) ok "polshim.ini kept as-is (already has every shipped option)" ;;
    *)    ok "polshim.ini kept; added $ADDED new option(s) at their defaults" ;;
  esac
fi

# Bake the chosen server into the ini so PLAY uses it too -- no Steam launch option
# needed. netredir arms the redirect on any server=; a Steam POLSHIM_SERVER still wins.
if [ -n "$SERVER" ] && [ "$SERVER" != "CHANGE-ME" ]; then
  SERVER_IP="$(to_ipv4 "$SERVER")"
  [ -n "$SERVER_IP" ] || die "Could not look up the address of '$SERVER'. Check the name and your connection, or give the server's IP address with --server=<ip>."
  set_redirect_server "$INI" "$SERVER_IP"
  if [ "$SERVER_IP" = "$SERVER" ]; then
    ok "Set [redirect] server=$SERVER_IP in polshim.ini"
  else
    ok "Set [redirect] server=$SERVER_IP in polshim.ini ($SERVER)"
    info "If that server ever moves to a new address, run this installer again."
  fi
else
  echo "[~] no server chosen -- set [redirect] server= in polshim.ini (or POLSHIM_SERVER at play)."
fi

# Last, and never fatal: the shim is already installed and working by this point,
# so a problem adding library entries must not fail the install.
install_steam_shortcuts || true

cat <<EOF

[+] DONE  (install: $DIR${SERVER:+ , server: $SERVER})
Just launch the game normally -- the title bar should end with '[PoL-Shim v0.1.0]'.
  - Server:      baked into polshim.ini (change it by re-running, or edit [redirect] server=).
  - Controller:  on by default; [inputmode] mode=off for kb/mouse, swap_confirm=1 to swap A/B.
  - Speed:       d3d8 -> DXVK enabled in Proton (Tetra Master / FFXI / Fantasy Earth).
                 Re-run this after switching Proton version -- the setting lives in the
                 Proton folder, so a switch loses it. Opt out with --no-dxvk-d3d8.
  - Revert:      ./install.sh --revert
EOF
