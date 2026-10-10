#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Wireless Power -- headless package + deploy helper.

Two modes:
  package : run Alpakit's PackagePlugin via RunUAT (no editor UI needed)
  deploy  : take the produced Windows zip, extract to BOTH game Mods dirs,
            inject mod.json, and normalise the .modules BuildId to "SML".

Usage:
  python deploy_wirelesspower.py package
  python deploy_wirelesspower.py deploy
  python deploy_wirelesspower.py all        # package then deploy
"""
import os
import sys
import shutil
import zipfile
import subprocess
import json
import hashlib

# ---- paths (adjust ENGINE_PATH if your "Unreal Engine - CSS" lives elsewhere) ----
PROJECT       = r"F:/Workbuddy/SMLDev/SMLStarterProject/FactoryGame.uproject"
ENGINE_PATH   = r"E:/Unreal Engine - CSS/Engine/Build/BatchFiles/RunUAT.bat"
MOD_SRC       = r"F:/Workbuddy/SMLDev/SMLStarterProject/Mods/WirelessPower"
ARCHIVED      = r"F:/Workbuddy/SMLDev/SMLStarterProject/Saved/ArchivedPlugins/WirelessPower"
LOCALAPP_MODS = os.path.join(os.environ.get("LOCALAPPDATA", ""), "FactoryGame", "Mods")
STEAM_MODS    = r"C:/Program Files (x86)/Steam/steamapps/common/Satisfactory/FactoryGame/Mods"
# Fallback common Steam library locations:
STEAM_CANDIDATES = [
    STEAM_MODS,
    r"D:/Steam/steamapps/common/Satisfactory/FactoryGame/Mods",
    r"E:/SteamLibrary/steamapps/common/Satisfactory/FactoryGame/Mods",
    r"E:/Steam/steamapps/common/Satisfactory/FactoryGame/Mods",
    r"F:/Steam/steamapps/common/Satisfactory/FactoryGame/Mods",
]
# ---------------------------------------------------------------------------------------------
# BuildId written into every deployed .modules manifest.
#
# The engine gates module discovery on this value: Core/Private/Modules/ModuleManager.cpp,
# FModuleManager::FindModulePathsInDirectory() skips a manifest whose BuildId differs from the
# game's own -- "Skipping out-of-date modules in manifest ..." -- after which the plugin dies
# with "module could not be found". Two values are blessed:
#
#   * the game's own BuildId      -> 502094 for Satisfactory 1.1 (CL 502094). Build-specific:
#                                    a game patch changes it and every mod tagged with the old
#                                    number stops loading.
#   * the literal "SML"           -> Coffee Stain's build-independent allowance. This is why
#                                    Alpakit writes it for every mod it packages.
#
# The "SML" allowance is NOT in the public UE-CSS SDK source (that only shows the
# /Engine/Plugins/Bridge/ clause next to the check), but it IS in the shipping binary: the string
# "SML" occurs exactly ONCE in FactoryGameSteam-Core-Win64-Shipping.dll (8.6 MB), sitting right
# after "/Engine/Plugins/Bridge/" in the same string-table block as the "Skipping out-of-date
# modules" message. Ecosystem cross-check: 13/13 published ficsit.app mods that ship a .modules
# use "SML" -- including SML itself -- and none ship a build-specific number.
#
# => Ship "SML". The earlier "must be 502094" rule came from a real but DIFFERENT failure (our own
#    game-target build wrote a random GUID 43139311, which is genuinely rejected); the fix was
#    never re-examined once Alpakit took over packaging.
# ---------------------------------------------------------------------------------------------
BUILD_ID = "SML"

MOD_NAME = "WirelessPower"

# Where stale in-place backups get moved. NEVER leave WirelessPower.bak_* inside the
# game's Mods folder: the plugin scan discovers every .uplugin there and SML prioritizes
# the first one found, so an old backup SHADOWS the freshly deployed mod (this shipped
# v1.0.0 to the player while v1.0.4 sat next to it, 2026-09-30).
QUARANTINE = r"F:/Workbuddy/SMLDev/_bak_20260930/mods_quarantine"


def quarantine_stale_backups(mods_root):
    """Move any WirelessPower.bak_* out of a game Mods folder before deploying."""
    if not os.path.isdir(mods_root):
        return
    for entry in sorted(os.listdir(mods_root)):
        if entry.startswith(MOD_NAME + ".bak_"):
            src = os.path.join(mods_root, entry)
            os.makedirs(QUARANTINE, exist_ok=True)
            dst = os.path.join(QUARANTINE, os.path.basename(os.path.normpath(mods_root)) + "_" + entry)
            print(f"[deploy] quarantining stale backup: {src} -> {dst}")
            try:
                shutil.move(src, dst)
            except Exception as e:
                print(f"[deploy] WARN: could not move {src}: {e}")


def find_windows_zip():
    if not os.path.isdir(ARCHIVED):
        return None
    for f in os.listdir(ARCHIVED):
        if f.lower().endswith(".zip") and "windows" in f.lower():
            return os.path.join(ARCHIVED, f)
    # some Alpakit versions name it just <Mod>.zip
    for f in os.listdir(ARCHIVED):
        if f.lower().endswith(".zip"):
            return os.path.join(ARCHIVED, f)
    return None


def package():
    if not os.path.exists(ENGINE_PATH):
        print(f"[deploy] ERROR: RunUAT not found at {ENGINE_PATH}")
        print("        Set ENGINE_PATH in this script to your 'Unreal Engine - CSS' RunUAT.bat.")
        sys.exit(1)
    cmd = [
        ENGINE_PATH,
        f'-ScriptsForProject="{PROJECT}"',
        "PackagePlugin",
        f'-project="{PROJECT}"',
        "-clientconfig=Shipping",
        "-serverconfig=Shipping",
        "-utf8output",
        f"-DLCName={MOD_NAME}",
        "-build",
        "-platform=Win64",
        "-server",
        "-serverplatform=Win64",
        "-nocompileeditor",
        "-installed",
    ]
    print("[deploy] Running Alpakit PackagePlugin (headless, ~7 min if C++ changed)...")
    print(" ".join(cmd))
    # Quote the engine path: it contains spaces ("Unreal Engine - CSS"), and a raw join would
    # make cmd.exe split it into "E:/Unreal" + "Engine - CSS/..." and fail with
    # "'E:/Unreal' is not recognized". Only the executable (cmd[0]) needs quoting.
    cmd_quoted = ['"{}"'.format(cmd[0])] + cmd[1:]
    rc = subprocess.call(" ".join(cmd_quoted), shell=True)
    if rc != 0:
        print(f"[deploy] PackagePlugin exited with code {rc}")
        sys.exit(rc)
    z = find_windows_zip()
    print(f"[deploy] PackagePlugin done. Windows zip: {z}")
    return z


def patch_modules(target_dir):
    """Find any *.modules in the deployed mod and force BuildId to BUILD_ID ("SML").

    Alpakit already writes "SML", so the usual case is a no-op -- and we deliberately leave the
    file UNTOUCHED then, so the deployed copy stays byte-identical to the published zip (what we
    publish is exactly what we test). Only a genuine mismatch is rewritten, which also normalises
    a leftover game-target build that baked in a random GUID.
    """
    for root, _dirs, files in os.walk(target_dir):
        for fn in files:
            if fn.lower().endswith(".modules"):
                p = os.path.join(root, fn)
                try:
                    with open(p, "r", encoding="utf-8") as fh:
                        data = json.load(fh)
                    if data.get("BuildId") == BUILD_ID:
                        print(f"[deploy] .modules BuildId already {BUILD_ID} (left as-is): {p}")
                        continue
                    data["BuildId"] = BUILD_ID
                    with open(p, "w", encoding="utf-8") as fh:
                        json.dump(data, fh, indent=2)
                    print(f"[deploy] patched .modules BuildId -> {BUILD_ID}: {p}")
                except Exception as e:
                    print(f"[deploy] WARN: could not patch {p}: {e}")


def game_running():
    """True if a Shipping game process is alive. Its DLLs/PAKs are memory-mapped, so Windows
    refuses to overwrite or delete them -- deploying then leaves a MIXED mod folder."""
    try:
        r = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True)
        s = r.stdout.decode("gbk", errors="replace").lower()
    except Exception:
        return False
    return any(n in s for n in (
        "factorygamesteam-win64-shipping.exe",
        "factorygameegs-win64-shipping.exe",
        "factorygame-win64-shipping.exe",
    ))


def deploy_one(zip_path, mods_root):
    if not os.path.isdir(mods_root):
        print(f"[deploy] skip (dir missing): {mods_root}")
        return True
    quarantine_stale_backups(mods_root)
    dest = os.path.join(mods_root, MOD_NAME)
    if os.path.isdir(dest) and game_running():
        # NEVER clear the folder while the game runs: rmtree would succeed for every unlocked
        # file and leave a HALF-DELETED mod folder behind (exactly what happened on 2026-10-01:
        # the Steam install-dir copy ended up holding nothing but the locked v1.0.16 DLL).
        # Overwriting file-by-file below leaves a complete -- if partly stale -- folder instead.
        print(f"[deploy] NOTE: the game is RUNNING -- overwriting in place, not clearing {dest} "
              f"(locked DLL/PAK files will be skipped; re-run after the game closes)")
    elif os.path.isdir(dest):
        try:
            shutil.rmtree(dest)
        except Exception as e:
            # Almost always the safe-delete shim refusing to trash the tree. Fall through to the
            # per-file overwrite below, which reports exactly what stayed locked.
            print(f"[deploy] WARN: could not clear {dest} ({e}); overwriting in place")
    os.makedirs(dest, exist_ok=True)

    # Extract file by file (instead of extractall) so a locked target is reported by name
    # rather than aborting the whole deploy with a bare traceback.
    locked = []
    with zipfile.ZipFile(zip_path, "r") as zf:
        for info in zf.infolist():
            out = os.path.join(dest, info.filename)
            if info.is_dir():
                os.makedirs(out, exist_ok=True)
                continue
            try:
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with zf.open(info) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
            except Exception as e:
                locked.append(info.filename)
                print(f"[deploy] LOCKED {info.filename}: {e}")

    # Alpakit zips sometimes wrap contents in a single "WirelessPower" folder. Flatten it
    # so the mod lands directly under Mods/WirelessPower.
    entries = [e for e in os.listdir(dest) if os.path.isdir(os.path.join(dest, e))]
    if len(entries) == 1 and os.path.isdir(os.path.join(dest, entries[0], "Binaries")):
        wrapper = os.path.join(dest, entries[0])
        for item in os.listdir(wrapper):
            shutil.move(os.path.join(wrapper, item), os.path.join(dest, item))
        os.rmdir(wrapper)
    # Inject mod.json (Alpakit zip does not include it).
    src_json = os.path.join(MOD_SRC, "mod.json")
    if os.path.exists(src_json):
        try:
            shutil.copy(src_json, os.path.join(dest, "mod.json"))
            print(f"[deploy] copied mod.json -> {dest}")
        except Exception as e:
            print(f"[deploy] LOCKED mod.json: {e}")
            locked.append("mod.json")
    else:
        print(f"[deploy] WARN: mod.json not found at {src_json}")
    patch_modules(dest)

    if locked:
        print(f"[deploy] INCOMPLETE ({len(locked)} locked): {locked}")
        print(f"[deploy] -> close the game and re-run 'deploy' to finish {dest}")
        return False
    print(f"[deploy] deployed -> {dest}")
    return True


def deploy():
    z = find_windows_zip()
    if not z or not os.path.exists(z):
        print(f"[deploy] ERROR: Windows zip not found under {ARCHIVED}. Run 'package' first.")
        sys.exit(1)
    print(f"[deploy] using zip: {z}")
    ok = deploy_one(z, LOCALAPP_MODS)
    for c in STEAM_CANDIDATES:
        if os.path.isdir(c):
            ok = deploy_one(z, c) and ok
            break
    else:
        print("[deploy] no Steam Satisfactory install found; deployed only to %LOCALAPPDATA%.")
    if not ok:
        print("[deploy] RESULT: at least one target is INCOMPLETE -- re-run after closing the game.")
        return 2
    print("[deploy] RESULT: all targets up to date.")
    return 0


def _md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def _md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _dest_path(dest, name):
    """Map a zip entry name to its deployed path, tolerating deploy_one's wrapper flattening."""
    p = os.path.join(dest, *name.split("/"))
    if os.path.exists(p):
        return p
    parts = name.split("/", 1)
    if len(parts) == 2:
        p2 = os.path.join(dest, *parts[1].split("/"))
        if os.path.exists(p2):
            return p2
    return p


def verify():
    """Assert the deployed folders match the packaged zip AND will actually load.

    Three independent checks per target:
      1. every file in the zip is present on disk with an identical MD5 -- catches a half-finished
         deploy (e.g. a DLL that stayed locked because the game was still running), and proves the
         local install is literally the artifact that would be published;
      2. every .modules carries BuildId == BUILD_ID -- otherwise the engine skips the DLL and the
         plugin dies with "module could not be found";
      3. the .uplugin declares SemVersion + GameVersion + a Plugins[].SemVersion for SML -- these
         are the fields SML actually reads (it never looks at mod.json), and both are ENFORCED:
         a mismatch is a UE_LOG(Fatal) "mismatched dependencies", i.e. the game refuses to start.
    """
    z = find_windows_zip()
    if not z or not os.path.exists(z):
        print("[verify] ERROR: no Windows zip found; run 'package' first")
        return 1
    print(f"[verify] zip: {z}")
    with zipfile.ZipFile(z) as zf:
        wanted = {}
        for info in zf.infolist():
            if info.is_dir():
                continue
            raw = zf.read(info)
            parsed = None
            if info.filename.lower().endswith(".modules"):
                try:
                    parsed = json.loads(raw.decode("utf-8-sig"))
                except Exception:  # noqa: BLE001
                    parsed = None
            wanted[info.filename] = (_md5_bytes(raw), parsed)

    targets = [LOCALAPP_MODS]
    steam = [c for c in STEAM_CANDIDATES if os.path.isdir(c)]
    if steam:
        targets.append(steam[0])
    else:
        print("[verify] WARN: no Steam install found; checking %LOCALAPPDATA% only")

    ok = True
    for root in dict.fromkeys(targets):
        dest = os.path.join(root, MOD_NAME)
        print(f"\n[verify] ---- {dest}")
        if not os.path.isdir(dest):
            print("[verify]   MISSING folder")
            ok = False
            continue

        missing, differ = [], []
        for name, (want_md5, want_json) in wanted.items():
            p = _dest_path(dest, name)
            if not os.path.exists(p):
                missing.append(name)
                continue
            if name.lower().endswith(".modules") and want_json is not None:
                # BuildId may legitimately be normalised on deploy, so compare the parsed
                # document rather than the bytes.
                try:
                    got = json.load(open(p, encoding="utf-8"))
                except Exception as exc:  # noqa: BLE001
                    differ.append(f"{name} (unreadable: {exc})")
                    continue
                if got != want_json:
                    differ.append(f"{name} (manifest mismatch)")
            elif _md5_file(p) != want_md5:
                differ.append(name)
        if missing:
            print(f"[verify]   MISSING {len(missing)} file(s): {missing}")
            ok = False
        if differ:
            print(f"[verify]   DIFFERS from zip ({len(differ)}): {differ}")
            ok = False
        if not missing and not differ:
            print(f"[verify]   content OK -- {len(wanted)} files match the zip "
                  f"(byte-identical; .modules compared as JSON)")

        for chain, _dirs, files in os.walk(dest):
            for fn in files:
                if fn.lower().endswith(".modules"):
                    p = os.path.join(chain, fn)
                    got = json.load(open(p, encoding="utf-8")).get("BuildId")
                    flag = "OK" if got == BUILD_ID else "BAD"
                    print(f"[verify]   BuildId {got!r} ({flag}) in {os.path.relpath(p, dest)}")
                    if got != BUILD_ID:
                        ok = False

        up = os.path.join(dest, MOD_NAME + ".uplugin")
        if os.path.exists(up):
            doc = json.load(open(up, encoding="utf-8-sig"))
            dep = [p.get("SemVersion") for p in doc.get("Plugins", [])
                   if p.get("Name") == "SML"]
            print(f"[verify]   uplugin SemVersion={doc.get('SemVersion')!r} "
                  f"GameVersion={doc.get('GameVersion')!r} SML-dep={dep}")
            if not doc.get("SemVersion") or not doc.get("GameVersion") or not (dep and dep[0]):
                print("[verify]   BAD: a required metadata field is missing")
                ok = False
        else:
            print("[verify]   MISSING .uplugin")
            ok = False

    print("\n[verify] RESULT:", "ALL TARGETS OK" if ok else "PROBLEMS FOUND")
    return 0 if ok else 3


if __name__ == "__main__":
    mode = sys.argv[1].lower() if len(sys.argv) > 1 else "all"
    if mode == "package":
        package()
    elif mode == "deploy":
        sys.exit(deploy())
    elif mode == "verify":
        sys.exit(verify())
    elif mode == "all":
        package()
        sys.exit(deploy())
    else:
        print("usage: deploy_wirelesspower.py [package|deploy|verify|all]")
        sys.exit(1)
