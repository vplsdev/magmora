"""Writes head-state-0.1.0.bin: a session as plug-in 0.1.0 saved it (head state chunk, version 1), for
test_chain. The layout is the one documented at "the state chunk" in plugin/Magmora.cpp; little-endian, as on
every supported machine. Run from the repository root: python3 dsp/tests/data/gen_state_fixture.py"""
import struct, pathlib

VALUES = [("input_trim", 3.5), ("input_jack", 1), ("channel", 0), ("bright", 1), ("crunch", 1), ("pre_rhythm", 7.25),
          ("low", 6.5), ("mid", 3.0), ("high", 7.0), ("post_rhythm", 5.5), ("resonance", 8.0), ("presence", 2.5),
          ("output_level", -6.0), ("gate_threshold", -70.0), ("phase_mode", 1), ("unknown_future_key", 3.0)]


def string(s):
    b = s.encode()
    return struct.pack("<i", len(b)) + b


body = struct.pack("<ii", 0, len(VALUES))
for key, value in VALUES:
    body += string(key) + struct.pack("<d", value)
body += string("Talus") + struct.pack("<f", 1.25)
chunk = struct.pack("<Iii", 0x41524353, 1, len(body)) + body
pathlib.Path(__file__).with_name("head-state-0.1.0.bin").write_bytes(chunk)
print(len(chunk), "bytes")
