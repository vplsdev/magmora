#!/usr/bin/env python3
"""Everything derived from plugin/config.h at build time: values, Info.plists and bundle resources.

    product.py get KEY            print a value from config.h (PLUG_NAME, PLUG_UNIQUE_ID, ...)
    product.py bundle APP|AU|VST3 run by the Xcode projects: writes the Info.plist and fills Resources

No Info.plist and no menu file in the tree spells the product name, so a rename is one line in config.h.
"""
import os
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

PLUGIN = pathlib.Path(__file__).resolve().parent.parent
APP_ICON = PLUGIN / "resources" / "icon" / "AppIcon.icns"   # made by art/icon/make_icon.py; the app only
TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|\'([^\']*)\'|([^\s"\']+)')


def config(path=PLUGIN / "config.h"):
    """The #defines of config.h, with macro references and adjacent strings resolved."""
    raw = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\s*#define\s+(\w+)(?:\s+(.*?))?\s*$", line)
        if m:
            raw[m.group(1)] = m.group(2) or ""

    def resolve(value, seen):
        out = ""
        for string, char, bare in TOKEN.findall(value):
            if bare in raw and bare not in seen:
                out += resolve(raw[bare], seen | {bare})
            else:
                out += string or char or bare
        return out

    return {key: resolve(value, {key}) for key, value in raw.items()}


def info_plist(kind, c, min_os):
    name, version = c["BUNDLE_NAME"], c["PLUG_VERSION_STR"]
    major, minor, patch = (int(x) for x in version.split("."))
    if int(c["PLUG_VERSION_HEX"], 16) != major << 16 | minor << 8 | patch:
        sys.exit("config.h: PLUG_VERSION_HEX and PLUG_VERSION_STR disagree")
    middle = {"APP": "app", "AU": "audiounit", "VST3": "vst3"}[kind]
    p = {
        "CFBundleDevelopmentRegion": "English",
        "CFBundleExecutable": name,
        "CFBundleGetInfoString": f"{name} v{version} {c['PLUG_COPYRIGHT_STR']}",
        "CFBundleIdentifier": f"{c['BUNDLE_DOMAIN']}.{c['BUNDLE_MFR']}.{middle}.{name}",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name,
        "CFBundlePackageType": "APPL" if kind == "APP" else "BNDL",
        "CFBundleShortVersionString": version,
        "CFBundleSignature": c["PLUG_UNIQUE_ID"],
        "CFBundleVersion": version,
        "CSResourcesFileMapped": True,
        "LSMinimumSystemVersion": min_os,
    }
    if kind == "APP":
        p.update(
            NSPrincipalClass="SWELLApplication",
            NSMainNibFile="MainMenu",
            CFBundleIconFile=APP_ICON.stem,
            NSHighResolutionCapable=True,
            LSApplicationCategoryType="public.app-category.music",
            NSMicrophoneUsageDescription=f"{c['PLUG_NAME']} processes the sound of your audio input.",
        )
    if kind == "AU":
        p["NSPrincipalClass"] = c["AUV2_VIEW_CLASS_STR"]
        p["AudioUnit Version"] = c["PLUG_VERSION_HEX"]
        p["AudioComponents"] = [{
            "type": "aumf" if c["PLUG_DOES_MIDI_IN"] == "1" else "aufx",
            "subtype": c["PLUG_UNIQUE_ID"],
            "manufacturer": c["PLUG_MFR_ID"],
            "name": f"{c['PLUG_MFR']}: {c['PLUG_NAME']}",
            "description": c["PLUG_NAME"],
            "factoryFunction": c["AUV2_FACTORY"],
            "version": int(c["PLUG_VERSION_HEX"], 16),
            "sandboxSafe": True,
        }]
    return p


def bundle(kind):
    c, env = config(), os.environ
    if env.get("PRODUCT_NAME") != c["BUNDLE_NAME"]:
        sys.exit("product name is not set: build with plugin/scripts/build_mac.sh (it reads plugin/config.h)")
    min_os = env["MACOSX_DEPLOYMENT_TARGET"]

    plist = pathlib.Path(env["DERIVED_FILE_DIR"]) / "Info.plist"
    plist.parent.mkdir(parents=True, exist_ok=True)
    plist.write_bytes(plistlib.dumps(info_plist(kind, c, min_os)))

    # Resources are rebuilt each time, so a deleted or renamed asset cannot stay in the bundle.
    # The loaders look files up by name, so img/ and fonts/ are copied flat.
    dst = pathlib.Path(env["TARGET_BUILD_DIR"]) / env["UNLOCALIZED_RESOURCES_FOLDER_PATH"]
    shutil.rmtree(dst, ignore_errors=True)
    dst.mkdir(parents=True)
    for sub in ("img", "fonts"):
        src = PLUGIN / "resources" / sub
        for f in sorted(src.iterdir()) if src.is_dir() else []:
            if f.is_file() and not f.name.startswith("."):
                if (dst / f.name).exists():
                    sys.exit(f"two resources are called {f.name}")
                shutil.copyfile(f, dst / f.name)

    if kind == "APP":
        shutil.copyfile(APP_ICON, dst / APP_ICON.name)
        if '"PLUG_NAME"' in (PLUGIN / "resources" / "main.rc_mac_dlg").read_text():
            sys.exit("main.rc_mac_dlg: swell_resgen.pl quoted PLUG_NAME; remove the quotes")
        with tempfile.TemporaryDirectory(dir=plist.parent) as tmp:
            xib = pathlib.Path(tmp) / "MainMenu.xib"
            text = (PLUGIN / "resources" / "MainMenu.xib").read_text()
            xib.write_text(text.replace("@PLUG_NAME@", c["PLUG_NAME"]))
            subprocess.run(["xcrun", "ibtool", "--minimum-deployment-target", min_os,
                            "--compile", str(dst / "MainMenu.nib"), str(xib)], check=True)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "get":
        print(config()[sys.argv[2]])
    elif len(sys.argv) == 3 and sys.argv[1] == "bundle" and sys.argv[2] in ("APP", "AU", "VST3"):
        bundle(sys.argv[2])
    else:
        sys.exit(__doc__)
