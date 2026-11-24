"""Panel states used for review renders and composites. No bpy."""
import layout as L

HERO = dict(pre_rhythm=0.42, pre_lead=0.62, low=0.55, mid=0.36, high=0.64, post_rhythm=0.4,
            post_lead=0.45, resonance=0.6, presence=0.5, channel=1, bright=0, crunch=1,
            input_jack=0, standby=1, power=1)
STATES = {
    "hero": HERO,
    "off": dict(HERO, power=0),
    "standby": dict(HERO, standby=0),
    "rhythm": dict(HERO, channel=0, bright=1, crunch=0, input_jack=1),
    "low": dict({key: 0.0 for key, *_ in L.KNOBS}, channel=0, bright=1, crunch=0, input_jack=1, standby=0, power=1),
    "high": dict({key: 1.0 for key, *_ in L.KNOBS}, channel=1, bright=0, crunch=1, input_jack=0, standby=1, power=1),
}


def on_frames(values):
    """The same state with every knob on the atlas frame the plugin would draw."""
    last = L.ATLASES["knob"][2] - 1
    return dict(values, **{key: round(values[key] * last) / last for key, *_ in L.KNOBS})
