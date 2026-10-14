# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Author candidate for B3. Does not modify canonical RTS capsules or install anything.

Interpretation for independent review: boost formation catch-up speed using Euclidean
squared distance >4096, retaining the existing combat stop gate. No acceptance claimed.
"""
import argparse
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]


class IR:
    def __init__(self): self.instructions=[]; self.counter=0; self.loads={}; self.constants={}
    def emit(self, op, **args):
        dst = "b3_r"+str(self.counter); self.counter += 1
        self.instructions.append(dict(op=op,dst=dst,**args)); return dst
    def const(self, value):
        if value not in self.constants: self.constants[value]=self.emit("const_u32",value=value)
        return self.constants[value]
    def load(self, field):
        if field not in self.loads: self.loads[field]=self.emit("load_u32",field=field)
        return self.loads[field]
    def binary(self, op,a,b): return self.emit(op,a=a,b=b)
    def select(self, cond,a,b): return self.emit("select_u32",cond=cond,on_true=a,on_false=b)
    def flag(self, cond): return self.select(cond,self.const(1),self.const(0))


def build(out):
    b=IR(); axes=[]
    for axis in ("x","y"):
        pos=b.load("pos_"+axis); goal=b.load("goal_"+axis)
        forward=b.binary("lt_u32",pos,goal)
        distance=b.select(forward,b.binary("sub_u32",goal,pos),b.binary("sub_u32",pos,goal))
        axes.append((pos,forward,distance))
    distance_sq=b.binary("add_u32",*[b.binary("mul_u32",a[2],a[2]) for a in axes])
    far=b.binary("lt_u32",b.const(4096),distance_sq)
    formation=b.binary("eq_u32",b.load("anchor_valid"),b.const(1))
    no_order=b.binary("eq_u32",b.load("order_mode"),b.const(0))
    both=b.binary("mul_u32",b.flag(formation),b.flag(no_order))
    eligible=b.binary("mul_u32",both,b.flag(far))
    boost=b.binary("eq_u32",eligible,b.const(1))
    speed=b.load("speed")
    effective=b.select(boost,b.binary("mul_u32",speed,b.const(2)),speed)
    mode_move=b.binary("eq_u32",b.load("order_mode"),b.const(1))
    has_target=b.binary("eq_u32",b.load("target_valid"),b.const(1))
    moving_flag=b.select(mode_move,b.const(1),b.select(has_target,b.const(0),b.const(1)))
    moving=b.binary("eq_u32",moving_flag,b.const(1))
    for axis,(pos,forward,distance) in zip(("x","y"),axes):
        step=b.select(b.binary("lt_u32",distance,effective),distance,effective)
        next_pos=b.select(forward,b.binary("add_u32",pos,step),b.binary("sub_u32",pos,step))
        value=b.select(moving,next_pos,pos)
        b.instructions.append(dict(op="store_u32",field="pos_"+axis,src=value))
    capsule=json.loads((ROOT/"capsules/rts/move.capsule.json").read_bytes())
    capsule["system_rev"]=2; capsule["source"]="move_b3.lir.json"
    capsule["fields"].append(dict(id=23,name="anchor_valid",type="u32",read=True,write=False))
    capsule["fields"].sort(key=lambda f:f["id"])
    module=dict(ir_version="0.1",system_id="rts.move",numeric_profile="u32_mod_v1",query="rts",instructions=b.instructions)
    out.mkdir(parents=True,exist_ok=True)
    for name,obj in (("move_b3.capsule.json",capsule),("move_b3.lir.json",module)):
        (out/name).write_text(json.dumps(obj,indent=2)+"\n")
    print("wrote author B3 candidate",len(b.instructions),"instructions to",out)

if __name__=="__main__":
    ap=argparse.ArgumentParser();ap.add_argument("--out",type=Path,required=True)
    build(ap.parse_args().out)
