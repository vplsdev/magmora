"""Lets the art scripts be started with any python3.

They need numpy and Pillow. If the running interpreter has neither, the script restarts under
$BLOCKAMP_PY, the project's Python from scripts/env.sh.
"""
import importlib.util
import os
import subprocess
import sys

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_GUARD = "MAGMORA_ART_REEXEC"


def ensure():
    if all(importlib.util.find_spec(name) for name in ("numpy", "PIL")):
        return
    python = os.environ.get("BLOCKAMP_PY")
    if not python:
        python = subprocess.run(
            ["bash", "-c", '. "$1" >/dev/null && printf %s "$BLOCKAMP_PY"', "bash", os.path.join(_REPO, "scripts", "env.sh")],
            capture_output=True, text=True).stdout
    if python and os.access(python, os.X_OK) and not os.environ.get(_GUARD):
        os.execve(python, [python] + sys.argv, dict(os.environ, **{_GUARD: "1"}))
    sys.exit(f"{os.path.basename(sys.argv[0])} needs numpy and Pillow. Run scripts/bootstrap_tools.sh, "
             "or set BLOCKAMP_PY to a Python that has them.")


def build_dir():
    private = os.environ.get("BLOCKAMP_PRIVATE")
    return os.path.join(private, "build", "art") if private else os.path.join(_REPO, "art", "build")
