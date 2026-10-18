# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Candidate-only low-HP retreat layered on B3+B2, preserving in-flight projectiles.

Live units have positive hp; hp0 anchors retain prior goal/move behavior.
"""
import json
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2]

def build():
    out=HERE/'candidates/b1';out.mkdir(parents=True,exist_ok=True)
    for system in ['goal','combat','move']:
        base=HERE/'candidates/b2' if system=='move' else ROOT/'capsules/rts'
        stem='move_b2' if system=='move' else system
        module=json.loads((base/(stem+'.lir.json')).read_text());cap=json.loads((base/(stem+'.capsule.json')).read_text())
        ins=module['instructions']
        prefix=[dict(op='const_u32',dst='b1_zero',value=0),dict(op='const_u32',dst='b1_one',value=1),dict(op='const_u32',dst='b1_three',value=3),
                dict(op='load_u32',dst='b1_hp',field='hp'),dict(op='lt_u32',dst='b1_positive',a='b1_zero',b='b1_hp'),
                dict(op='lt_u32',dst='b1_healthy',a='b1_three',b='b1_hp'),
                dict(op='select_u32',dst='b1_lowflag',cond='b1_healthy',on_true='b1_zero',on_false='b1_one'),
                dict(op='select_u32',dst='b1_retreatflag',cond='b1_positive',on_true='b1_lowflag',on_false='b1_zero'),
                dict(op='eq_u32',dst='b1_retreat',a='b1_retreatflag',b='b1_one')]
        if system=='goal':
            prefix += [dict(op='load_u32',dst='b1_team',field='team'),dict(op='eq_u32',dst='b1_team0',a='b1_team',b='b1_zero'),
                       dict(op='const_u32',dst='b1_base0',value=2048),dict(op='const_u32',dst='b1_base1',value=30720),dict(op='const_u32',dst='b1_basey',value=16384),
                       dict(op='select_u32',dst='b1_basex',cond='b1_team0',on_true='b1_base0',on_false='b1_base1')]
            result=[]
            for x in ins:
                if x['op']=='store_u32':
                    axis=x['field'][-1];dst='b1_goal'+axis;result.append(dict(op='select_u32',dst=dst,cond='b1_retreat',on_true='b1_base'+axis,on_false=x['src']));x['src']=dst
                result.append(x)
            ins=result
        elif system=='combat':
            fire=next(x for x in ins if x.get('dst')=='t13');assert fire==dict(op='eq_u32',dst='t13',a='s12',b='k2')
            index=ins.index(fire)+1
            extra=[dict(op='select_u32',dst='b1_fireflag',cond='b1_healthy',on_true='s12',on_false='k1'),dict(op='eq_u32',dst='b1_fire',a='b1_fireflag',b='k2')]
            for x in ins[index:]:
                if x.get('cond')=='t13':x['cond']='b1_fire'
            ins[index:index]=extra
        else:
            result=[]
            bydst={x.get('dst'):x for x in ins}
            for x in ins:
                if x['op']=='store_u32':
                    old=bydst[x['src']];assert old['op']=='select_u32'
                    axis=x['field'][-1];dst='b1_move'+axis;result.append(dict(op='select_u32',dst=dst,cond='b1_retreat',on_true=old['on_true'],on_false=x['src']));x['src']=dst
                result.append(x)
            ins=result
        module['instructions']=prefix+ins
        reads=[(3,'hp')]+([(2,'team')] if system=='goal' else [])
        for fid,name in reads:
            match=next((x for x in cap['fields'] if x['id']==fid),None)
            if match:match['read']=True
            else:cap['fields'].append(dict(id=fid,name=name,type='u32',read=True,write=False))
        cap['fields'].sort(key=lambda x:x['id']);cap['system_rev']=4 if system=='move' else 2;cap['source']=system+'_b1.lir.json'
        if system in ['goal','move']:cap['schedule']['before']=sorted(set(cap['schedule']['before']+['rts.damage']))
        for name,value in [(system+'_b1.capsule.json',cap),(system+'_b1.lir.json',module)]: (out/name).write_text(json.dumps(value,indent=2)+'\n', newline="\n")
if __name__=='__main__':build()
