# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Reproduce B1 hot installation in the full six-system RTS world without a native build."""
import argparse, hashlib, json, shutil, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];sys.path.insert(0,str(ROOT/'bench/rts/replay'))
import rts_session as rs

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--out',required=True);a=ap.parse_args()
    here=Path(__file__).parent;hidden=here/'hidden';s=rs.Session(str(Path(a.binary).resolve()))
    try:
        scratch=Path(s.dir)
        for directory in ['b3','b2','b1']:
            for p in (here/'candidates'/directory).iterdir():shutil.copyfile(p,scratch/p.name)
        wid,rev=s.create_rts(4);pid=s.worker_pid()
        rows=[{'values':{'kind':1,'team':0,'hp':3,'pos_x':4000,'pos_y':16384,'speed':96,'range_sq':65536,'reload':6,'damage':3,'flight':2,'order_mode':2,'order_x':300,'order_y':100}},
              {'values':{'kind':1,'team':1,'hp':65535,'pos_x':4200,'pos_y':16384,'reload':1,'damage':1,'flight':1}}]
        spawn=s.call('world.spawn_batch',{'world_id':wid,'expected_world_revision':rev,'rows':rows});rev=spawn['world_revision']
        member=spawn['entities'][0]
        manifest=json.loads((hidden/'manifest.json').read_text());evaluations=[];submissions=[];installs=[]
        for system in ['goal','combat','move']:
            cap=system+'_b1.capsule.json';sub=s.call('capsule.submit',{'capsule_path':cap});submissions.append(sub)
            for fixture in manifest['fixtures']:
                if fixture['system_id']!='rts.'+system:continue
                for name,sha in fixture['files'].items():assert hashlib.sha256((hidden/name).read_bytes()).hexdigest()==sha
                test=json.loads((hidden/fixture['test']).read_text());test['capsule']=cap
                (scratch/fixture['test']).write_text(json.dumps(test));shutil.copyfile(hidden/fixture['world'],scratch/fixture['world'])
                ev=s.call('candidate.evaluate',{'candidate_id':sub['candidate_id'],'test_path':fixture['test']});assert ev['status']=='passed';evaluations.append(ev)
            install=s.call('candidate.install',{'world_id':wid,'candidate_id':sub['candidate_id'],'expected_world_revision':rev,'allowed_effective_tick':0});rev=install['world_revision'];installs.append(install)
        step=s.call('world.step',{'world_id':wid,'expected_world_revision':install['world_revision'],'ticks':1})
        inspect=s.call('world.inspect',{'world_id':wid,'entity':member})
        values=inspect['entity']['values'];assert [values['pos_x'],values['pos_y']]==[3904,16384]
        assert values['cooldown']==0 and values['target_valid']==1 and values['proj_active']==0
        assert [values['goal_x'],values['goal_y']]==[2048,16384]
        assert s.worker_pid()==pid
        record={'status':'passed_full_schedule_hidden_evaluation_install','submissions':submissions,'evaluations':evaluations,'installs':installs,'step':step,
                'entity':inspect['entity'],'worker_preserved':True,'native_build_commands':[],
                'artifact_sha256':hashlib.sha256(Path(a.binary).read_bytes()).hexdigest(),
                'fixture_author':manifest['author_agent'],'implementation_author':'/root',
                'model':'gpt-6.1-sol','token_usage':None,'token_usage_reason':'not available per experiment',
                'limitations':['No performance qualification; wider earlier-suite checks are recorded separately.']}
        Path(a.out).write_text(json.dumps(record,indent=2)+'\n');print(record['status'])
    finally:s.close()
if __name__=='__main__':main()
