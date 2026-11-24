"""Circuit description shared by the SPICE exporter and the discrete-time solver.

One Circuit object is the single source of truth for a netlist: `spice.py` writes it as an
ngspice deck and `dk.py` turns it into the matrices of the nodal K-method solver, so both
simulators always see the same elements and values.
"""
from __future__ import annotations
from dataclasses import dataclass, field

GND = "0"


@dataclass
class Element:
    kind: str          # R C L V I T (triode) P (pentode) D (diode) X (ideal transformer)
    name: str
    nodes: tuple
    value: object = None
    extra: dict = field(default_factory=dict)


class Circuit:
    def __init__(self, title="circuit"):
        self.title = title
        self.elements: list[Element] = []
        self.inputs: list[str] = []     # names of V/I sources driven per sample, in order
        self.outputs: list[tuple] = []  # (label, node_plus, node_minus)
        self._names = set()

    def _add(self, kind, name, nodes, value=None, **extra):
        if name in self._names:
            raise ValueError(f"duplicate element {name}")
        self._names.add(name)
        self.elements.append(Element(kind, name, tuple(nodes), value, extra))
        return self

    def R(self, name, a, b, ohms):
        if ohms <= 0:
            raise ValueError(f"{name}: non-positive resistance")
        return self._add("R", name, (a, b), float(ohms))

    def C(self, name, a, b, farads):
        return self._add("C", name, (a, b), float(farads))

    def L(self, name, a, b, henries):
        return self._add("L", name, (a, b), float(henries))

    def V(self, name, p, n, dc=0.0, is_input=False):
        """Voltage source. If is_input, its value is a per-sample input (dc is the bias value)."""
        self._add("V", name, (p, n), float(dc))
        if is_input:
            self.inputs.append(name)
        return self

    def I(self, name, a, b, dc=0.0, is_input=False):
        """Current source; the current flows from a through the source to b."""
        self._add("I", name, (a, b), float(dc))
        if is_input:
            self.inputs.append(name)
        return self

    def out_i(self, label, vsource_name):
        """Output the branch current of a voltage source (positive = flowing from its + node into the source)."""
        self.outputs.append((label, "#i", vsource_name))
        return self

    def triode(self, name, p, g, k, model):
        return self._add("T", name, (p, g, k), model)

    def pentode(self, name, p, g2, g, k, model, n_parallel=1):
        return self._add("P", name, (p, g2, g, k), model, n_parallel=n_parallel)

    def diode(self, name, a, k, model):
        return self._add("D", name, (a, k), model)

    def xfmr(self, name, windings):
        """Ideal multi-winding transformer. windings = [(n_plus, n_minus, turns), ...]."""
        return self._add("X", name, tuple(n for w in windings for n in w[:2]),
                         [float(w[2]) for w in windings])

    def pot(self, name, top, wiper, bottom, ohms, frac, rmin=1.0):
        """Three-terminal pot; frac = resistance fraction between bottom and wiper (0..1)."""
        frac = min(max(frac, 0.0), 1.0)
        self.R(name + "a", top, wiper, max(ohms * (1.0 - frac), rmin))
        self.R(name + "b", wiper, bottom, max(ohms * frac, rmin))
        return self

    def out(self, label, p, n=GND):
        self.outputs.append((label, p, n))
        return self

    def node_names(self):
        seen = []
        for e in self.elements:
            for n in e.nodes:
                if n != GND and n not in seen:
                    seen.append(n)
        return seen
