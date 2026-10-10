#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Wireless Power -- build (package + deploy) and then auto-shut-down the PC.

Use this when you want to kick off a WP build and walk away / go to rest:
the machine powers itself off once the build + deploy finish successfully.

Usage:
    python wp_build_and_shutdown.py            # defaults to 'all' (package + deploy)
    python wp_build_and_shutdown.py all
    python wp_build_and_shutdown.py package    # build only, then shut down
    python wp_build_and_shutdown.py deploy     # deploy existing zip, then shut down

Behaviour:
    * Runs deploy_wirelesspower.py with the given mode.
    * On SUCCESS (exit code 0): schedules `shutdown /s /t <DELAY>` -- the PC powers off
      DELAY seconds later.
    * On FAILURE: does NOT shut down; reports the error so you can fix and re-run.

CANCEL a pending shutdown at any time (e.g. if you came back):
    shutdown /a
"""
import os
import sys
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
DEPLOY = os.path.join(HERE, "deploy_wirelesspower.py")

# Seconds of grace between "build finished" and "power off". Gives you time to
# cancel with `shutdown /a` if you are still at the keyboard.
SHUTDOWN_DELAY = 180


def main():
    mode = sys.argv[1].lower() if len(sys.argv) > 1 else "all"
    cmd = [sys.executable, DEPLOY, mode]
    print("[wp-shutdown] >>> running:", " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        print(f"[wp-shutdown] >>> build/deploy FAILED (exit code {rc}). PC will NOT shut down.")
        sys.exit(rc)

    print(f"[wp-shutdown] >>> build + deploy succeeded.")
    print(f"[wp-shutdown] >>> Scheduling PC shutdown in {SHUTDOWN_DELAY} seconds...")
    print("[wp-shutdown] >>> To CANCEL, open a terminal and run:  shutdown /a")
    # /s = full shutdown; /t = delay in seconds; /c = comment shown to the user.
    subprocess.call(
        f'shutdown /s /t {SHUTDOWN_DELAY} /c "WirelessPower build finished"',
        shell=True,
    )
    print("[wp-shutdown] >>> Shutdown scheduled. Going dark shortly. Good rest!")


if __name__ == "__main__":
    main()
