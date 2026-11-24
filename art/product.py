"""The product's printed identity, read from plugin/config.h, the one definition of the name."""
import pathlib
import re

_CONFIG = pathlib.Path(__file__).resolve().parent.parent / "plugin" / "config.h"

NAME = re.search(r'^#define\s+PLUG_NAME\s+"([^"]+)"', _CONFIG.read_text(encoding="utf-8"), re.M).group(1)
BADGE_TEXT = NAME.upper()
