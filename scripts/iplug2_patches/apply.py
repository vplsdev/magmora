#!/usr/bin/env python3
"""Patches made to the vendored iPlug2, applied by scripts/vendor_iplug2.sh to a fresh copy.

    apply.py <vendored iPlug2 folder>

Patch 1 replaces the text entry number formatter of IGraphicsMac_view.h/.mm. Upstream credits it to a blog post
that states no licence. The replacement (formatter_*.mm next to this file) is MIT, part of this project, and was
written from the NSFormatter API without using the removed code. The upstream text is located by its structure
and never stored here. Fails loudly when upstream no longer has that shape.
"""
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent

CALL_UPSTREAM = (
    "    [mTextFieldView setFormatter:[[[IGRAPHICS_FORMATTER alloc] init] autorelease]];\n"
    "    [[mTextFieldView formatter] setAcceptableCharacterSet:characterSet];\n"
    "    [[mTextFieldView formatter] setMaximumLength:length];\n")
CALL_OURS = (
    "    IGRAPHICS_FORMATTER* pFilter = [[IGRAPHICS_FORMATTER alloc] initWithAllowed:characterSet // Magmora patch 1\n"
    "                                                                          limit:(NSUInteger) MAX(length, 0)];\n"
    "    [mTextFieldView setFormatter:pFilter];\n"
    "    [pFilter release];\n")


def replace_once(path, pattern, replacement, flags=re.M | re.S):
    text = path.read_text()
    text, n = re.subn(pattern, lambda _: replacement, text, flags=flags)
    if n != 1:
        sys.exit(f"apply.py: {path}: expected one match of the patched region, found {n}")
    path.write_text(text)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = pathlib.Path(sys.argv[1])
    view = root / "IGraphics/Platforms"
    interface = (HERE / "formatter_interface.mm").read_text().rstrip("\n")
    implementation = (HERE / "formatter_implementation.mm").read_text().rstrip("\n")
    replace_once(view / "IGraphicsMac_view.h",
                 r"^// based on code by Scott Gruby[^\n]*\n@interface IGRAPHICS_FORMATTER.*?^@end", interface)
    replace_once(view / "IGraphicsMac_view.mm", r"^@implementation IGRAPHICS_FORMATTER.*?^@end", implementation)
    replace_once(view / "IGraphicsMac_view.mm", re.escape(CALL_UPSTREAM), CALL_OURS, flags=0)


if __name__ == "__main__":
    main()
