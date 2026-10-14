# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""M3-M27 detector: mutate only a session's scratch IR, then run independent model comparison."""
import argparse
import hashlib
import json
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/"bench/rts/replay"))
sys.path.insert(0, str(ROOT/"tests/rts/replay"))
import rts_session
import test_model_snapshot as check

OriginalSession = rts_session.Session


class MutantSession(OriginalSession):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        path = Path(self.dir, "combat.lir.json")
        doc = json.loads(path.read_bytes())
        arrival = next(i for i in doc["instructions"] if i.get("dst") == "s40")
        assert arrival["on_true"] == "k1" and arrival["on_false"] == "v39"
        arrival["on_true"] = "v39"
        path.write_text(json.dumps(doc, indent=2)+"\n")
        print("M3-M27 scratch IR sha256:"+hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--binary", required=True); args = ap.parse_args()
    spec = next(s for s in check.rtsgen.SCENE_SPECS if s["name"] == "duel16")
    rts_session.Session = MutantSession
    detected = False
    try:
        try:
            check.run(str(Path(args.binary).resolve()), spec, 8)
        except AssertionError as error:
            # A creation/refusal/syntax failure is not a semantic mutation detection.
            if "tick 3, field proj_target_gen, slot 0: model 0, engine 1" not in str(error):
                raise
            detected = True
            print("M3-M27 detected:", error)
    finally:
        rts_session.Session = OriginalSession
    if not detected:
        raise AssertionError("M3-M27 survived independent model comparison")
    check.run(str(Path(args.binary).resolve()), spec, 8)
    print("M3-M27 restored baseline passed; repository IR untouched")
    return 0

if __name__ == "__main__": sys.exit(main())
