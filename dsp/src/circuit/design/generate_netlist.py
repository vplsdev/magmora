"""Write dsp/src/circuit/NetlistData.inc from the Python prototype, which is the executable specification.

    source scripts/env.sh && $BLOCKAMP_PY dsp/src/circuit/design/generate_netlist.py [--check]

Everything the C++ engine knows about the circuit comes from this: elements, values, device parameters, relay
variants, taper laws, the partition and the solver constants. Nothing is typed into C++. Every value set listed in
value_sets.json gets its own tables, so the as-drawn and the fitted circuit both run from one binary.

How: proto/blockamp/amp.py is executed unchanged for every combination of the switches. Knob positions are replaced by
symbols, so that each knob-dependent resistor is recorded as a law `max(offset + slope * taper(position / 10), rmin)`
and every other element as a list of variants (nodes, value) with the switch combinations each one belongs to.
--check exits 1 if the committed file is out of date.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

from blockamp import amp, circuit, devices, dk, models, partition

OUT = Path(__file__).resolve().parent.parent / "NetlistData.inc"
VALUE_SETS = Path(__file__).resolve().with_name("value_sets.json")
# bit, key, value when 0, value when 1, where the prototype takes it
SWITCHES = [("channel", "rhythm", "lead", "ctl"), ("crunch", False, True, "ctl"), ("bright", False, True, "ctl"),
            ("input", "high", "normal", "ctl"), ("load", "reactive", "resistive", "phys")]
KINDS = dict(R="Resistor", C="Capacitor", L="Inductor", V="VoltageSource", I="CurrentSource", T="Triode", P="Pentode",
             D="Diode", X="Transformer")
MAX_NODES = 8
MAX_PARAMS = 12


class Law:
    """offset + slope * taper(knob), where taper is linear (mid = 0) or the audio taper through (0.5, mid)."""

    def __init__(self, key, mid=0.0, offset=0.0, slope=1.0, rmin=-1e300):
        self.key, self.mid, self.offset, self.slope, self.rmin = key, mid, offset, slope, rmin

    def _with(self, offset, slope):
        if self.rmin != -1e300:
            raise ValueError(f"{self.key}: arithmetic after max() is not representable")
        return Law(self.key, self.mid, offset, slope)

    def __rmul__(self, k):
        return self._with(k * self.offset, k * self.slope)

    __mul__ = __rmul__

    def __rsub__(self, k):
        return self._with(k - self.offset, -self.slope)

    def floor(self, rmin):
        return Law(self.key, self.mid, self.offset, self.slope, rmin)

    def record(self):
        if not self.rmin > 0.0:
            raise ValueError(f"{self.key}: a knob-dependent resistor needs a positive minimum")
        return (self.key, self.mid, self.offset, self.slope, self.rmin)


def symbolic_taper(x, mid):
    if not (isinstance(x, Law) and x.mid == 0.0 and x.offset == 0.0 and x.slope == 1.0):
        raise ValueError("taper_log must be applied to a knob position")
    return Law(x.key, mid)


def symbolic_max(*args):
    if len(args) == 2 and isinstance(args[0], Law):
        return args[0].floor(args[1])
    return max(*args)


class RecordingCircuit(circuit.Circuit):
    def R(self, name, a, b, ohms):
        if isinstance(ohms, Law):
            return self._add("R", name, (a, b), ohms)
        return super().R(name, a, b, ohms)

    def pot(self, name, top, wiper, bottom, ohms, frac, rmin=1.0):
        if not isinstance(frac, Law):
            return super().pot(name, top, wiper, bottom, ohms, frac, rmin)
        self.R(name + "a", top, wiper, (ohms * (1.0 - frac)).floor(rmin))
        self.R(name + "b", wiper, bottom, (ohms * frac).floor(rmin))
        return self


def combination(combo):
    where = dict(ctl={}, phys={})
    for bit, (key, off, on, kind) in enumerate(SWITCHES):
        where[kind][key] = on if combo >> bit & 1 else off
    return where["ctl"], where["phys"]


def element_value(e):
    """(scalar, parameter vector, law)"""
    if e.kind == "T":
        return 0.0, tuple(models.triode_vec(e.value)), None
    if e.kind == "P":
        return 0.0, tuple(models.pentode_vec(e.value, e.extra.get("n_parallel", 1))), None
    if e.kind == "D":
        return 0.0, tuple(models.diode_vec(e.value)), None
    if e.kind == "X":
        return float(len(e.value)), tuple(e.value), None
    if isinstance(e.value, Law):
        return 0.0, (), e.value.record()
    return float(e.value), (), None


def collect(overrides):
    unknown = set(overrides) - set(amp.PHYS)
    if unknown:
        raise ValueError(f"value_sets.json: not in amp.PHYS: {sorted(unknown)}")
    names = ("_x", "taper_log", "Circuit")
    saved = {n: getattr(amp, n) for n in names}
    amp._x = lambda ctl, key: Law(key)
    amp.taper_log = symbolic_taper
    amp.Circuit = RecordingCircuit
    amp.max = symbolic_max                                        # shadows the builtin inside amp only
    try:
        elements = []
        for section_index, section in enumerate(amp.ALL):
            order, found = [], {}
            for combo in range(1 << len(SWITCHES)):
                ctl, phys = combination(combo)
                c = amp.build(ctl, dict(overrides, **phys), sections=(section,))
                previous = -1
                for e in dk.expand(c):                            # includes each tube's inter-electrode capacitors
                    if e.kind not in KINDS or len(e.nodes) > MAX_NODES:
                        raise ValueError(f"cannot export element {e.name}")
                    scalar, par, law = element_value(e)
                    key = (tuple(e.nodes), scalar, law)
                    if e.name not in found:
                        previous += 1
                        order.insert(previous, e.name)
                        found[e.name] = dict(kind=e.kind, name=e.name, section=section_index, par=par, variants={},
                                             is_input=e.name in c.inputs)
                    else:
                        previous = order.index(e.name)
                        if found[e.name]["par"] != par or found[e.name]["kind"] != e.kind:
                            raise ValueError(f"{e.name}: device parameters may not depend on a switch")
                    variants = found[e.name]["variants"]
                    variants[key] = variants.get(key, 0) | 1 << combo
            elements += [found[n] for n in order]
    finally:
        for n, v in saved.items():
            setattr(amp, n, v)
        del amp.max
    if len({e["name"] for e in elements}) != len(elements):
        raise ValueError("element names must be unique across sections")
    return elements


def model_hash(overrides):
    """Same definition as scripts/export_model_data.py; a test compares it with ModelData.inc."""
    data = dict(T12AX7=models.T12AX7, P6L6GC=models.P6L6GC, D1N4007=models.D1N4007,
                PHYS={k: v for k, v in dict(amp.PHYS, **overrides).items() if not isinstance(v, str)}, DEFAULT_LOAD=amp.PHYS["load"])
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()[:16]


def num(x):
    return repr(float(x))


def text(x):
    return "nullptr" if x is None else json.dumps(x)


def tables(index, overrides, knobs):
    """The C++ tables of one value set, and the initialiser that describes it."""
    laws, variants, rows = [], [], []
    for e in collect(overrides):
        first = len(variants)
        for (nodes, scalar, law), mask in e["variants"].items():
            law_index = -1
            if law:
                key, mid, offset, slope, rmin = law
                law_index = len(laws)
                laws.append(f"  {{{knobs.index(key)}, {num(mid)}, {num(offset)}, {num(slope)}, {num(rmin)}}},")
            variants.append(f"  {{0x{mask:x}ull, {{{', '.join(text(n) for n in nodes)}}}, {len(nodes)}, "
                            f"{num(scalar)}, {law_index}}},")
        if len(e["par"]) > MAX_PARAMS:
            raise ValueError(f"{e['name']}: too many parameters")
        par = ", ".join(num(p) for p in e["par"])
        rows.append(f"  {{Kind::{KINDS[e['kind']]}, {text(e['name'])}, {e['section']}, {first}, "
                    f"{len(variants) - first}, {'true' if e['is_input'] else 'false'}, {{{par}}}}},")
    body = hashlib.sha256("\n".join(rows + variants + laws).encode()).hexdigest()[:16]
    out = [f"inline constexpr LawDef kLaws{index}[] = {{", *laws, "};",
           f"inline constexpr VariantDef kVariants{index}[] = {{", *variants, "};",
           f"inline constexpr ElementDef kElements{index}[] = {{", *rows, "};"]
    return out, f'{text(model_hash(overrides))}, {text(body)}, kLaws{index}, kVariants{index}, kElements{index}, {len(rows)}'


def render():
    value_sets = json.loads(VALUE_SETS.read_text())["sets"]
    if value_sets[0]["phys"]:
        raise ValueError("value_sets.json: the first (default) set must be the prototype itself")
    knobs = [k for k, v in amp.DEFAULTS.items() if isinstance(v, float)]
    out = ["// GENERATED by dsp/src/circuit/design/generate_netlist.py from proto/blockamp. Do not edit.",
           "// Sources and confidence of every value: docs/research/CIRCUIT.md, proto/blockamp.",
           "",
           f"inline constexpr double kLnMax = {num(devices.LN_MAX)};",
           f"inline constexpr double kKneeBeta = {num(devices.KNEE_BETA)};",
           f"inline constexpr double kPentodeEps3 = {num(devices.PEN_EPS3)};",
           f"inline constexpr double kPentodeEps1 = {num(devices.PEN_EPS1)};",
           f"inline constexpr double kGridStep = {num(dk.GRID_STEP)};",
           f"inline constexpr double kGridOn = {num(dk.GRID_ON)};",
           f"inline constexpr double kDiodeStep = {num(dk.DIODE_STEP)};",
           f"inline constexpr double kDiodeOn = {num(dk.DIODE_ON)};",
           f"inline constexpr double kDcCapLeak = {num(dk.DC_CAP_LEAK)};",
           "// coupling of the signal cuts (partition.COMPANION): true = companion coupling, held on every cut",
           f"inline constexpr bool kCompanionCoupling = {'true' if partition.COMPANION else 'false'};",
           "",
           "enum SwitchBit : unsigned {"]
    out += [f"  kSwitch_{key} = 1u << {bit}," for bit, (key, *_) in enumerate(SWITCHES)]
    out += ["};", "inline constexpr SwitchDef kSwitches[] = {"]
    for key, off, on, kind in SWITCHES:
        default = (amp.DEFAULTS if kind == "ctl" else amp.PHYS)[key]
        off_text, on_text = ("0", "1") if isinstance(on, bool) else (off, on)
        out.append(f"  {{{text(key)}, {text(off_text)}, {text(on_text)}, {'true' if default == on else 'false'}}},")
    out += ["};", "inline constexpr KnobDef kKnobs[] = {"]
    out += [f"  {{{text(k)}, {num(amp.DEFAULTS[k])}}}," for k in knobs]
    out += ["};", "inline constexpr const char* kSections[] = {", *[f"  {text(s)}," for s in amp.ALL], "};"]
    entries = []
    for index, value_set in enumerate(value_sets):
        rows, entry = tables(index, value_set["phys"], knobs)
        out += ["", f"// value set {index}: {value_set['name']}", *rows]
        entries.append(f"  {{{text(value_set['name'])}, {entry}}},")
    out += ["", "inline constexpr ValueSetDef kValueSets[] = {", *entries, "};"]
    for label, clusters in (("Split", partition.CLUSTERS_SPLIT), ("Merged", partition.CLUSTERS)):
        out.append(f"inline constexpr ClusterDef k{label}Partition[] = {{")
        for name, sections, cut_in, cut_out, rails in clusters:
            mask = sum(1 << amp.ALL.index(s) for s in sections)
            rail_list = ", ".join(text(r) for r in rails)
            extrap = "false" if name in partition.NO_EXTRAP else "true"
            out.append(f"  {{{text(name)}, 0x{mask:x}u, {text(cut_in)}, {text(cut_out)}, {{{rail_list}}}, "
                       f"{len(rails)}, {extrap}}},")
        out.append("};")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    content = render()
    if a.check:
        if not OUT.exists() or OUT.read_text() != content:
            print(f"{OUT} is out of date", file=sys.stderr)
            sys.exit(1)
        print("up to date")
    else:
        OUT.write_text(content)
        print("wrote", OUT, "model", model_hash({}))
