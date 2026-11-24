#!/usr/bin/env python3
"""Gate G7: licence audit of the files that were actually compiled, linked and bundled.

    python3 scripts/license_audit.py [--cmake-build DIR]... [--plugin-build DIR] [--no-plugin] [--notices FILE]

Defaults: $BLOCKAMP_PRIVATE/build/core (cmake -G Ninja) and $BLOCKAMP_PRIVATE/build/plugin (build_mac.sh).

Evidence, none of it taken from project files:
  CMake    compile_commands.json for the sources, Ninja's dependency log for every header each one included
  Xcode    each target's LinkFileList for the objects linked, the compiler's .d file of each object (and
           of the precompiled headers) for what went in, the linker's dependency info for libraries
  bundles  every file inside the built .app, .component and .vst3
Out-of-date or missing evidence is a failure, never a pass. Standard library only, Python 3.9 or later.
Exit status: 0 clean, 1 violations, 2 no evidence.
"""
import argparse
import collections
import fnmatch
import hashlib
import json
import os
import pathlib
import plistlib
import re
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "plugin" / "scripts"))
from product import config as product_config  # noqa: E402

MARKERS = re.compile(
    r"(?<![A-Za-z])(?:[AL]?GPL(?:v?\d(?:\.\d)?)?\+?)(?![A-Za-z])"
    r"|GNU[\s/*#;]+(?:(?:Lesser|Library|Affero)[\s/*#;]+)?General[\s/*#;]+Public"  # may wrap across comment lines
    r"|non-?commercial"
    r"|creative\s*commons[^\n]{0,40}\bNC\b|\bCC[ -]BY-NC"
    r"|WTFPL|DO WHAT THE FUCK",
    re.I,
)
SPDX = re.compile(r"SPDX-License-Identifier:\s*([A-Za-z0-9.+ ()-]+)")
COPYRIGHT = re.compile(r"copyright\s*(?:\(c\)|©)?\s*(?:19|20)\d\d|(?:\(c\)|©)\s*(?:19|20)\d\d", re.I)
BAD_PATH = re.compile(r"juce|vst2|aax|asio|hiir|faust", re.I)
# Amendment A3: tracked files must not name the commercial sims, so this one knows them only by digest
# (SHA-256, first 16 hex digits, of the lower-case word or words).
SIMS_AB = {"c887981e1d699ec9", "645633078cb04ed1"}
OTHER_PRODUCTS = {"1d40e096ac2053dd", "66cd4317fa695002", "8b1436406f8d9fc9", "85a3bfc866c6aee9", "277ab9e03cda0bec", "b3398084c05d5e5b", "72069474870d80c5"}
# Amendment A31: nor the maker, model names and model numbers of the studied amplifier and overdrive, also known
# only by digest (SHA-256, first 16 hex digits, of the lower-case name without spaces, hyphens or underscores).
BRANDS = {"f868ce518d195af5", "0b204f0523cd16b0", "9ff8476644903ba6", "a7a84d7ed00e48b8", "3a9be8c308aa033d",
          "6ca879b0e26d002e", "d7b30cd38561a765", "bbbfc2b63b885539", "6d7bc334f6901670", "c6c9f0e352b2acf8",
          "09838f747452840e"}
BRAND_INSIDE = (6, 8, 12)  # names this long are also found inside longer words
RUNS = re.compile(r"[a-z]+|[0-9]+")
HOME_PATH = re.compile(r"/Users/(?!USERNAME|runner\b)[A-Za-z0-9._-]+|/home/(?!runner\b)[a-z0-9._-]+/|[A-Za-z]:\\Users\\\w+")

AUDIO = set("wav w64 rf64 aif aiff flac mp3 m4a aac ogg opus caf irs".split())
CAPTURE = set("nam aidax tsr kipr pt ckpt".split())
SCAN = set("pdf djvu tif tiff bmp heic".split())
IMAGE = set("png jpg jpeg gif webp exr".split())
RAW = set("f32 f64 npy npz".split())
SECRET = set("env pem key p8 p12 mobileprovision".split())
IMAGE_DIRS = ("plugin/resources/img/", "plugin/resources/icon/", "art/textures/", "docs/images/")
PRIVATE_FILES = {"docs/BRIEF.local.md", "scripts/env.local.sh", "eval/local_assets.json"}


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def brand(text, numbers=True):
    """True if text names the studied amplifier or overdrive. A name is found as a whole run of letters, a run of
    digits not glued to a letter before it, two runs side by side or joined by one hyphen or underscore (or by one
    space when one of them is digits), or, for the lengths in BRAND_INSIDE, anywhere inside letters joined by at
    most one space, hyphen or underscore. numbers=False skips the names
    that are only digits (for documentation and data, where they are ordinary numbers)."""
    low = text.lower()
    last_end, last = -2, ""
    for m in RUNS.finditer(low):
        run, start = m.group(), m.start()
        glued = run[0].isdigit() and start > 0 and low[start - 1].isalpha()
        candidates = [] if glued else [run]
        gap = low[last_end:start]
        if last and (gap in ("", "-", "_") or (gap == " " and (last[0].isdigit() or run[0].isdigit()))):
            candidates.append(last + run)
        if any((numbers or not c.isdigit()) and digest(c) in BRANDS for c in candidates):
            return True
        last_end, last = m.end(), run
    joined = re.sub(r"(?<=[a-z])[ _-](?=[a-z])", "", low)
    for m in re.finditer(r"[a-z]{%d,}" % min(BRAND_INSIDE), joined):
        word = m.group()
        for n in BRAND_INSIDE:
            if any(digest(word[i:i + n]) in BRANDS for i in range(len(word) - n + 1)):
                return True
    return False


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def output_of(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True).stdout.decode("utf-8", "replace")


def private_dir():
    if os.environ.get("BLOCKAMP_PRIVATE"):
        return pathlib.Path(os.environ["BLOCKAMP_PRIVATE"])
    try:
        out = output_of(["bash", "-c", '. "$1" && printf %s "$BLOCKAMP_PRIVATE"', "-", str(ROOT / "scripts/env.sh")])
    except (OSError, subprocess.CalledProcessError):
        out = ""
    return pathlib.Path(out or ROOT / ".tools")


def system_roots():
    """Compiler and OS headers and libraries. Deliberately not /usr/local or Homebrew."""
    if sys.platform == "darwin":
        roots = ["/usr/include", "/usr/lib", "/System/Library", "/Library/Developer/CommandLineTools"]
        roots += [output_of(["xcode-select", "-p"]).strip(), output_of(["xcrun", "--show-sdk-path"]).strip()]
    elif os.name == "nt":
        roots = [os.path.join(os.environ.get(var, ""), sub)
                 for var in ("ProgramFiles", "ProgramFiles(x86)")
                 for sub in ("Microsoft Visual Studio", "Windows Kits")]
    else:
        roots = ["/usr/include", "/usr/lib/gcc", "/usr/lib/clang", "/usr/lib/llvm"]
    resolved = {pathlib.Path(r).resolve() for r in roots if r}
    return sorted(resolved)


def under(path, root):
    return path == root or root in path.parents


def font_license(path):
    """The licence strings (name ids 13 and 14) of a TrueType/OpenType font."""
    data = path.read_bytes()
    try:
        for i in range(struct.unpack(">H", data[4:6])[0]):
            tag, _, offset, _ = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
            if tag != b"name":
                continue
            _, count, strings = struct.unpack(">HHH", data[offset:offset + 6])
            found = []
            for k in range(count):
                at = offset + 6 + 12 * k
                platform, _, _, name_id, length, where = struct.unpack(">6H", data[at:at + 12])
                if name_id in (13, 14):
                    raw = data[offset + strings + where:offset + strings + where + length]
                    found.append(raw.decode("utf-16-be" if platform in (0, 3) else "latin-1", "replace"))
            return " ".join(found)
    except struct.error:
        pass
    return ""


class Audit:
    def __init__(self):
        self.errors = {}
        self.uses = collections.defaultdict(set)  # file -> {"dev", "plugin"}
        self.texts = {}
        self.fonts = set()
        self.system = system_roots()
        self.table = json.loads((ROOT / "third_party/LICENSES.json").read_text())
        self.tree = self.read_tree()

    def fail(self, rule, where, why):
        self.errors[(rule, str(where), why)] = None

    def rel(self, path):
        return path.relative_to(ROOT).as_posix() if under(path, ROOT) else str(path)

    def text(self, path):
        if path not in self.texts:
            self.texts[path] = path.read_bytes().decode("utf-8", "replace") if path.is_file() else ""
        return self.texts[path]

    def read_tree(self):
        """Tracked plus would-be-tracked files: there may be no commit yet."""
        out = output_of(["git", "--no-optional-locks", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], ROOT)
        return sorted({p for p in out.split("\0") if p})

    def is_system(self, path):
        return any(under(path, root) for root in self.system)

    def add_input(self, raw, base, build, role, built_at=None):
        path = pathlib.Path(base, raw).resolve()
        if self.is_system(path):
            return
        if under(path, build) and path.suffix in (".pch", ".gch"):
            return  # what went into a precompiled header is collected from its own .d file
        if not path.is_file():
            self.fail("evidence", self.rel(path), "was compiled but no longer exists: rebuild")
            return
        if built_at is not None and path.stat().st_mtime > built_at:
            self.fail("evidence", self.rel(path), "is newer than what was built from it: rebuild")
        self.uses[path].add(role)

    # ---- 1. what was compiled, linked and bundled

    def cmake_build(self, build, role):
        db, cache = build / "compile_commands.json", build / "CMakeCache.txt"
        if not db.is_file() or not cache.is_file():
            self.fail("evidence", build, "no compile database: configure and build first")
            return
        if not (build / "build.ninja").is_file():
            self.fail("evidence", build, "header dependencies need the Ninja generator (-G Ninja)")
            return
        ninja = re.search(r"^CMAKE_MAKE_PROGRAM:\w+=(.+)$", cache.read_text(), re.M).group(1)
        deps, current = {}, None
        for line in output_of([ninja, "-C", str(build), "-t", "deps"]).splitlines():
            header = re.match(r"(\S.*): #deps \d+.*\((VALID|STALE)\)$", line)
            if header:
                current = deps.setdefault((build / header.group(1)).resolve(), [])
            elif line.startswith("    ") and current is not None:
                current.append(line.strip())
            else:
                current = None

        entries = json.loads(db.read_text())
        known, compiled = set(), 0
        for entry in entries:
            source = pathlib.Path(entry["directory"], entry["file"]).resolve()
            obj = pathlib.Path(entry["directory"], entry["output"]).resolve()
            known.add(source)
            # The plug-in build configures the whole tree but builds only the library target.
            if not obj.is_file():
                if role == "plugin" and not under(source, ROOT / "dsp/src"):
                    continue
                self.fail("evidence", self.rel(source), f"is not built in {build}")
                continue
            if obj not in deps:
                self.fail("evidence", self.rel(source), f"has no dependency record in {build}")
                continue
            compiled += 1
            for raw in [str(source)] + deps[obj]:
                self.add_input(raw, build, build, role, obj.stat().st_mtime)
        for source in (ROOT / "dsp/src").rglob("*.cpp"):
            if source.resolve() not in known:
                self.fail("evidence", self.rel(source), f"is missing from the compile database of {build}: rebuild")
        if not compiled:
            self.fail("evidence", build, "nothing compiled")

    @staticmethod
    def read_depfile(path):
        body = re.split(r":(?:\s|$)", path.read_text().replace("\\\n", " "), maxsplit=1)[-1]
        return [p.replace("\\ ", " ") for p in re.findall(r"(?:\\ |\S)+", body)]

    def xcode_build(self, build, name):
        projects = ROOT / "plugin/projects"
        targets = build / "obj/Magmora-macOS.build/Release"
        bundles = {"APP": f"{name}.app", "AU": f"{name}.component", "VST3": f"{name}.vst3"}
        self.cmake_build(build / "dsp", "plugin")
        dsp_lib = build / "dsp/dsp/libmagmora_dsp.a"
        for target, bundle_name in bundles.items():
            bundle = build / "products/Release" / bundle_name
            binary = bundle / "Contents/MacOS" / name
            lists = sorted((targets / f"{target}.build").glob("Objects-normal/*/*.LinkFileList"))
            lists = [x for x in lists if x.stem == name]
            if not lists or not binary.is_file():
                self.fail("evidence", target, f"not built in {build}: run plugin/scripts/build_mac.sh")
                continue
            linked_at = binary.stat().st_mtime
            if not dsp_lib.is_file() or dsp_lib.stat().st_mtime > linked_at:
                self.fail("evidence", target, "older than the DSP library it links: rebuild")
            for linked in lists:
                objects = {pathlib.Path(x).resolve() for x in linked.read_text().splitlines() if x}
                for obj in objects:
                    dep = obj.with_suffix(".d")
                    if not obj.is_file() or not dep.is_file():
                        self.fail("evidence", obj, "linked object without a dependency file")
                        continue
                    for raw in self.read_depfile(dep):
                        self.add_input(raw, projects, build, "plugin", obj.stat().st_mtime)
                self.linker_inputs(linked, objects, dsp_lib, build, target)
            self.bundle(bundle, name, target)
        for dep in (build / "obj/PCH").rglob("*.pch.d"):
            for raw in self.read_depfile(dep):
                self.add_input(raw, projects, build, "plugin")

    def linker_inputs(self, linked, objects, dsp_lib, build, target):
        info = linked.parent / f"{linked.stem}_dependency_info.dat"
        if not info.is_file():
            self.fail("evidence", target, "no linker dependency info")
            return
        for record in info.read_bytes().split(b"\0"):
            if len(record) < 2 or record[0] != 0x10:  # 0x10: an input the linker read
                continue
            path = pathlib.Path(ROOT / "plugin/projects", record[1:].decode()).resolve()
            if self.is_system(path) or path in objects or path in (linked.resolve(), dsp_lib.resolve()):
                continue
            if under(path, build) and path.suffix in (".xcent", ".der"):
                continue  # entitlements embedded by the linker
            if under(path, dsp_lib.parent.resolve()) and path.suffix == ".o":
                continue
            self.fail("linked", self.rel(path), f"{target} links something that is not built from audited sources")

    def bundle(self, bundle, name, target):
        sources = {}
        for sub in ("img", "fonts"):
            folder = ROOT / "plugin/resources" / sub
            for f in folder.iterdir() if folder.is_dir() else []:
                if f.is_file() and not f.name.startswith("."):
                    sources[f.name] = f
        if target == "APP":
            icon = ROOT / "plugin/resources/icon/AppIcon.icns"  # the stand-alone app's icon (product.py)
            sources[icon.name] = icon
        generated = {"Contents/Info.plist", "Contents/PkgInfo", f"Contents/MacOS/{name}",
                     "Contents/_CodeSignature/CodeResources"}
        seen = set()
        for f in sorted(bundle.rglob("*")):
            if not (f.is_file() or f.is_symlink()):
                continue
            inside = f.relative_to(bundle).as_posix()
            where = f"{bundle.name}/{inside}"
            if inside in generated or (target == "APP" and inside.startswith("Contents/Resources/MainMenu.nib")):
                if inside == "Contents/Info.plist":
                    self.names(where, str(plistlib.loads(f.read_bytes())))
                continue
            source = sources.get(f.name)
            if inside != f"Contents/Resources/{f.name}" or source is None or f.is_symlink():
                self.fail("bundled", where, "has no source under plugin/resources/img, fonts or icon")
                continue
            seen.add(f.name)
            if sha256(source) != sha256(f):
                self.fail("evidence", where, "differs from its source: rebuild")
            self.resource(source.resolve(), where)
        for missing in sorted(set(sources) - seen):
            self.fail("evidence", f"{bundle.name}/{missing}", "is not in the bundle: rebuild")

    def resource(self, source, where):
        if not under(source, ROOT / "plugin/resources"):
            self.fail("bundled", where, "resolves to a file outside plugin/resources")
        ext = source.suffix.lower().lstrip(".")
        if BAD_PATH.search(source.name):
            self.fail("path", where, "forbidden component in the name")
        if brand(source.name):
            self.fail("brand", where, "protected name in an asset name")
        if ext in ("ttf", "otf", "ttc", "woff", "woff2"):
            self.fonts.add(source)
            stated = re.sub(r"\s+", " ", font_license(source)).lower()
            if not re.search(r"open font license|openfontlicense|scripts\.sil\.org/ofl", stated):
                self.fail("licence", where, "font does not state OFL-1.1")
            texts = [self.text(t) for t in source.parent.glob("OFL*.txt")]
            if not any("SIL OPEN FONT LICENSE Version 1.1" in t for t in texts):
                self.fail("licence", where, "no OFL-1.1 text next to the font")
        elif ext in ("txt", "json", "md", "svg"):
            body = self.text(source)
            self.names(where, body)
            if not source.name.startswith("OFL") and MARKERS.search(body):
                self.fail("marker", where, "forbidden licence marker in a bundled file")

    # ---- 2, 3. licences of the compiled files

    def components(self, rel):
        return [c for c in self.table["components"] if any(fnmatch.fnmatchcase(rel, g) for g in c["files"])]

    def licences(self):
        allowed = set(self.table["allowed"])
        if not allowed <= {"MIT", "BSD-2-Clause", "BSD-3-Clause", "Zlib", "ISC", "Apache-2.0", "BSL-1.0",
                           "Public-Domain", "Unlicense"}:
            self.fail("licence", "third_party/LICENSES.json", "the allow-list was widened beyond gate G7")
        for c in self.table["components"]:
            if not c["licenses"] or not set(c["licenses"]) <= allowed:
                self.fail("licence", c["id"], f"{c['licenses']} is not on the allow-list")
            if not (ROOT / c["text"]).is_file():
                self.fail("licence", c["id"], f"licence text {c['text']} is missing")
        waived = collections.defaultdict(set)
        for e in self.table["marker_exceptions"]:
            waived[e["path"]].add(e["line"])

        for path, roles in sorted(self.uses.items()):
            rel = self.rel(path)
            if BAD_PATH.search(rel):
                self.fail("path", rel, "JUCE, VST2, AAX, ASIO, HIIR or Faust in a compiled path")
            body = self.text(path)
            # Markers may be wrapped across comment lines, so match on the whole file.
            for match in MARKERS.finditer(body):
                start = body.rfind("\n", 0, match.start()) + 1
                end = body.find("\n", match.start())
                line = body[start:end if end >= 0 else len(body)].strip()
                if line not in waived[rel]:
                    number = body.count("\n", 0, match.start()) + 1
                    self.fail("marker", f"{rel}:{number}", f"forbidden licence marker {match.group(0)!r}")
            for spdx in SPDX.findall(body):
                ids = re.findall(r"[A-Za-z0-9.+-]+", spdx)
                if any(i not in allowed | {"AND", "OR"} for i in ids):
                    self.fail("marker", rel, f"SPDX licence {spdx.strip()!r} is not on the allow-list")

            if not under(path, ROOT):
                self.fail("location", rel, "compiled from outside the repository")
            elif rel.startswith("third_party/"):
                owners = self.components(rel)
                if not owners:
                    self.fail("licence", rel, "no licence recorded in third_party/LICENSES.json")
                if "plugin" in roles and any(c.get("dev_only") for c in owners):
                    self.fail("location", rel, "developer-only component in a plug-in")
                prefix = "third_party/iPlug2/WDL/"
                if rel.startswith(prefix) and rel[len(prefix):] not in self.table["wdl_files"]:
                    self.fail("licence", rel, "WDL file is not on the reviewed list")
            else:
                if rel.startswith(("proto/", "eval/", "bench/", "dsp/tests/", "dsp/tools/")):
                    if "plugin" in roles:
                        self.fail("location", rel, "developer-only code in a plug-in target")
                    if rel.startswith(("proto/", "eval/")):
                        self.fail("location", rel, "proto/ and eval/ are never compiled")
                elif not rel.startswith(("dsp/", "plugin/")):
                    self.fail("location", rel, "compiled from outside dsp/, plugin/ and third_party/")
                if rel not in self.tree:
                    self.fail("location", rel, "compiled but git-ignored, so it would not be in the repository")
                for line in body.splitlines():
                    if COPYRIGHT.search(line) and "Magmora contributors" not in line:
                        self.fail("licence", rel, "someone else's copyright notice: third-party code belongs "
                                                  "in third_party/ with a recorded licence")
                        break

    def vendored(self):
        for folder in ("third_party/iPlug2", "third_party/catch2"):
            base, want = ROOT / folder, {}
            manifest = base / "MANIFEST.sha256"
            if not manifest.is_file():
                self.fail("vendored", folder, "MANIFEST.sha256 is missing")
                continue
            for line in manifest.read_text().splitlines():
                value, name = line.split(None, 1)
                want[name.lstrip("*")] = value
            have = {p.relative_to(base).as_posix(): p for p in base.rglob("*")
                    if p.is_file() and p != manifest and p.name != ".DS_Store"}
            for name in sorted(set(want) | set(have)):
                rel = f"{folder}/{name}"
                if name not in have:
                    self.fail("vendored", rel, "missing")
                elif name not in want:
                    self.fail("vendored", rel, "not in the manifest")
                elif sha256(have[name]) != want[name]:
                    self.fail("vendored", rel, "was modified")
                elif rel not in self.tree:
                    self.fail("vendored", rel, "is git-ignored and would be lost")
                elif not self.components(rel):
                    self.fail("licence", rel, "no licence recorded in third_party/LICENSES.json")

    # ---- 4, 5. what git would publish

    def private_data(self):
        for rel in self.tree:
            ext = rel.rsplit(".", 1)[-1].lower()
            base = rel.rsplit("/", 1)[-1].lower()
            path = ROOT / rel
            if path.is_symlink() and not under(path.resolve(), ROOT):
                self.fail("private", rel, "symlink out of the repository")
            if rel in PRIVATE_FILES:
                self.fail("private", rel, "local file would be published")
            if ext in CAPTURE:
                self.fail("private", rel, "capture or trained model")
            if ext in AUDIO:
                self.fail("private", rel, "audio file")
            if ext in RAW and not rel.startswith("reference/"):
                self.fail("private", rel, "raw signal file outside reference/")
            if ext in SCAN or ("schematic" in base and ext in IMAGE | {"svg"}):
                self.fail("private", rel, "document, scan or schematic")
            elif ext in IMAGE and not rel.startswith(IMAGE_DIRS):
                self.fail("private", rel, "image outside " + ", ".join(IMAGE_DIRS))
            if ext in SECRET or base.startswith(".env") and base != ".env.example":
                self.fail("private", rel, "environment or credential file")
            if rel.startswith("third_party/") or ext in IMAGE | RAW | {"ttf", "otf"} or not path.is_file():
                continue
            body = self.text(path)
            found = HOME_PATH.search(body)
            if found:
                self.fail("private", rel, "personal absolute path")

    def names(self, where, body, sims_only=False):
        if brand(body, numbers=not sims_only):
            self.fail("brand", where, "names the studied amplifier or overdrive" +
                      (" (not allowed in any tracked file)" if sims_only else " in product-facing content"))
        words = re.findall(r"[a-z0-9]+", body.lower())
        seen = set(words) if sims_only else set(words) | {" ".join(p) for p in zip(words, words[1:])}
        banned = SIMS_AB if sims_only else SIMS_AB | OTHER_PRODUCTS
        if any(digest(word) in banned for word in seen):
            self.fail("brand", where, "names a commercial product")

    def product_names(self, product):
        for rel in self.tree:
            path = ROOT / rel
            if rel.startswith("third_party/") or not path.is_file():
                continue
            if rel.startswith(("plugin/", "presets/", "art/")):
                self.names(f"{rel} (file name)", rel)
            if path.suffix.lower() in (".ttf", ".otf", ".png", ".jpg", ".ico", ".icns", ".f32", ".f64", ".csv"):
                continue
            # art/pages/ holds the block pages' printed legends (XP8), as art/product.py does the head's name
            if rel.startswith(("plugin/", "presets/", "art/pages/")) or rel in ("art/product.py", "art/layout.json"):
                self.names(rel, self.text(path))
            elif rel.startswith(("dsp/include/", "dsp/src/")):
                self.names(rel, " | ".join(re.findall(r'"([^"\n]*)"', self.text(path))))
            if not rel.startswith("reference/"):
                self.names(rel, self.text(path), sims_only=True)
        art = ROOT / "art/product.py"
        printed = re.search(r'^NAME\s*=\s*["\']([^"\']+)', self.text(art), re.M) if art.is_file() else None
        if printed and printed.group(1) != product["PLUG_NAME"]:
            self.fail("name", "art/product.py", "differs from PLUG_NAME in plugin/config.h")

    # ---- 6. notices

    def write_notices(self, out, product):
        shipped = collections.defaultdict(list)
        for path, roles in sorted(self.uses.items()):
            rel = self.rel(path)
            if "plugin" in roles and rel.startswith("third_party/"):
                for c in self.components(rel):
                    shipped[c["id"]].append(rel)
        lines = ["# Third-party notices", "",
                 f"Generated by `scripts/license_audit.py --notices` from the files compiled into {product['PLUG_NAME']}.",
                 f"{product['PLUG_NAME']} itself is MIT licensed, see LICENSE.", ""]
        for c in self.table["components"]:
            if c["id"] not in shipped:
                continue
            lines += [f"## {c['name']}", "", f"Licence: {', '.join(c['licenses'])}. Copyright: {c['copyright']}"]
            lines += [c["url"]] if c.get("url") else []
            lines += [c["note"]] if c.get("note") else []
            lines += ["", "```", (ROOT / c["text"]).read_text().strip(), "```", "", "Compiled files:", ""]
            lines += [f"- `{rel}`" for rel in shipped[c["id"]]] + [""]
        if self.fonts:
            lines += ["## Fonts (OFL-1.1)", ""] + [f"- `{self.rel(f)}`" for f in sorted(self.fonts)] + [""]
            for text in sorted({t for f in self.fonts for t in f.parent.glob("OFL*.txt")}):
                lines += ["```", text.read_text().strip(), "```", ""]
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cmake-build", type=pathlib.Path, action="append")
    ap.add_argument("--plugin-build", type=pathlib.Path)
    ap.add_argument("--no-plugin", action="store_true", help="DSP, tests and tools only (not a G7 pass)")
    ap.add_argument("--notices", type=pathlib.Path, help="write THIRD_PARTY_NOTICES.md here")
    args = ap.parse_args()
    if args.notices and args.no_plugin:
        ap.error("--notices needs the plug-in build")

    default = private_dir() / "build"
    cmake_dirs = [d.resolve() for d in args.cmake_build or [default / "core"]]
    plugin_dir = None if args.no_plugin else (args.plugin_build or default / "plugin").resolve()

    audit, product = Audit(), product_config()
    for build in cmake_dirs:
        audit.cmake_build(build, "dev")
    if plugin_dir:
        audit.xcode_build(plugin_dir, product["BUNDLE_NAME"])
    if not audit.uses:
        for _, where, why in audit.errors:
            print(f"license audit: {where}: {why}", file=sys.stderr)
        print("license audit: no compiled files found, nothing was audited", file=sys.stderr)
        return 2
    audit.licences()
    audit.vendored()
    audit.private_data()
    audit.product_names(product)

    in_plugin = [p for p, roles in audit.uses.items() if "plugin" in roles]
    third = [p for p in in_plugin if audit.rel(p).startswith("third_party/")]
    print(f"audited {len(audit.uses)} compiled files ({len(in_plugin)} in plug-in targets, {len(third)} of those "
          f"third-party), {len(audit.fonts)} bundled fonts, {len(audit.tree)} files git would publish")
    if audit.errors:
        for rule, where, why in sorted(audit.errors):
            print(f"FAIL [{rule}] {where}: {why}")
        print(f"license audit: FAILED with {len(audit.errors)} findings")
        return 1
    if args.notices:
        audit.write_notices(args.notices, product)
    print("license audit: OK" + (" (without the plug-in: not a G7 pass)" if args.no_plugin else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
