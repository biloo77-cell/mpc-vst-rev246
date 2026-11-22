#!/usr/bin/env python3
"""Write vst/presets.json (MPC's PRESET menu) from upstream's factory presets.

    python3 tools/make_presets.py

Upstream's list (upstream/plugin/src/presets/PresetManager.cpp, factoryState): first "Init S" ... "Init N", one per
programme, then FactoryPresets.h's 65 presets. Each resets every sound setting to its default, sets the programme
(E: LF x1.5 / HF x0.7, the nearest steps to the family's 1.49 / 0.69) and, for a factory preset, its six joystick
steps. Preferences (XFMR and MODE here; also CAL, POWER and LINK upstream) are left as they are.
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "upstream", "plugin", "src", "presets", "FactoryPresets.h")
PARAMS = os.path.join(ROOT, "vst", "params.json")
OUT = os.path.join(ROOT, "vst", "presets.json")

JOYS = ["lowpass", "delay", "ampl", "lf", "rt", "hf"]
PREFERENCES = {"xfmr", "mode"}
E_LF_STEP, E_HF_STEP = 13, 6  # lfStepsX[13] = 1.5, HF step 6 = x0.7 (params::joyNearestStep for 1.49 / 0.69)


def main():
    plist = json.load(open(PARAMS))["params"]
    programs = next(p for p in plist if p["key"] == "program")["options"]
    # sound settings only: no preferences, read-only lights and displays ("live"), or the levers' step buttons
    defaults = {p["key"]: p["default"] for p in plist
                if p["key"] not in PREFERENCES and not p.get("live") and not p.get("momentary")}

    def preset(name, program, steps=None):
        values = {"program": programs[program]}  # the programme first: the joystick texts follow it
        for k, v in defaults.items():
            if k != "program":
                values[k] = v
        if program == 3:
            values["lf"], values["hf"] = E_LF_STEP, E_HF_STEP
        if steps:
            values.update(dict(zip(JOYS, steps)))
        return {"name": name, "values": values}

    out = [preset("Init " + "SMLEN"[i], i) for i in range(5)]
    text = open(SRC).read()
    rows = re.findall(r'FactoryPreset\{"([^"]+)",\s*(\d+),\s*\{\{([\d,\s]+)\}\}', text)
    if len(rows) != 65:
        raise SystemExit("FactoryPresets.h: expected 65 presets, found %d" % len(rows))
    for name, program, steps in rows:
        out.append(preset(name, int(program), [int(s) for s in steps.split(",")]))
    json.dump({"presets": out}, open(OUT, "w"), indent=1)
    print("%s: %d presets" % (os.path.relpath(OUT, ROOT), len(out)))


if __name__ == "__main__":
    main()
