# Source this file. Locations of developer-only tools and private data. Nothing here is shipped or linked.
# Machine-specific values go in scripts/env.local.sh (git-ignored); see scripts/env.local.example.sh.
_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
[ -f "$_ROOT/scripts/env.local.sh" ] && . "$_ROOT/scripts/env.local.sh"
: "${BLOCKAMP_PRIVATE:=$_ROOT/.tools}"                           # bulky working data, venv, ngspice (never committed)
: "${BLOCKAMP_PY:=$BLOCKAMP_PRIVATE/venv/bin/python}"            # numpy scipy numba soundfile matplotlib cma
: "${NGSPICE:=$BLOCKAMP_PRIVATE/tools/ngspice/bin/ngspice}"
: "${BLOCKAMP_NAM_PY:=$BLOCKAMP_PY}"                             # a Python with torch (optional)
: "${BLOCKAMP_ASSETS:=}"                                         # folder with the private captures, DI and renders
: "${BLOCKAMP_IR:=}"                                             # cabinet IR used by the reference renders
export BLOCKAMP_PRIVATE BLOCKAMP_PY NGSPICE BLOCKAMP_NAM_PY BLOCKAMP_ASSETS BLOCKAMP_IR
export PYTHONPATH="$_ROOT/proto${PYTHONPATH:+:$PYTHONPATH}"
