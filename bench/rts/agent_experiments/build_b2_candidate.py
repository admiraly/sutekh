# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Preserve B3 and gate engagement stop on post-combat cooldown zero (contract B2)."""
import json
from pathlib import Path
HERE=Path(__file__).resolve().parent

def build():
    module=json.loads((HERE/'candidates/b3/move_b3.lir.json').read_text())
    instructions=module['instructions'];loads={x.get('field'):x['dst'] for x in instructions if x['op']=='load_u32'}
    zero=next(x['dst'] for x in instructions if x['op']=='const_u32' and x['value']==0)
    one=next(x['dst'] for x in instructions if x['op']=='const_u32' and x['value']==1)
    target=next(x for x in instructions if x['op']=='eq_u32' and x['a']==loads['target_valid'] and x['b']==one)
    index=instructions.index(target)+1;old=target['dst']
    extra=[dict(op='load_u32',dst='b2_cd',field='cooldown'),dict(op='eq_u32',dst='b2_ready',a='b2_cd',b=zero),
           dict(op='select_u32',dst='b2_stop_flag',cond='b2_ready',on_true=one,on_false=zero),
           dict(op='eq_u32',dst='b2_stop',a='b2_stop_flag',b=one)]
    # Select the ready predicate only for a valid target; otherwise false.
    extra.insert(3,dict(op='select_u32',dst='b2_target_stop',cond=old,on_true='b2_stop_flag',on_false=zero))
    extra[-1]['a']='b2_target_stop'
    for x in instructions[index:]:
        if x.get('cond')==old:x['cond']='b2_stop'
    instructions[index:index]=extra
    cap=json.loads((HERE/'candidates/b3/move_b3.capsule.json').read_text());cap['system_rev']=3;cap['source']='move_b2.lir.json'
    cap['fields'].append(dict(id=9,name='cooldown',type='u32',read=True,write=False));cap['fields'].sort(key=lambda x:x['id'])
    out=HERE/'candidates/b2';out.mkdir(parents=True,exist_ok=True)
    for name,value in [('move_b2.capsule.json',cap),('move_b2.lir.json',module)]: (out/name).write_text(json.dumps(value,indent=2)+'\n', newline="\n")
if __name__=='__main__':build()
