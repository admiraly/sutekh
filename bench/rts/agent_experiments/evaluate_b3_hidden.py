# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Run locked independently authored B3 fixtures through the real candidate evaluator."""
import argparse, hashlib, json, shutil, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'bench/rts/replay'))
import rts_session as rs

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--out',required=True);a=ap.parse_args()
    here=Path(__file__).parent; hidden=here/'hidden'; manifest=json.loads((hidden/'manifest.json').read_text())
    selected=[f for f in manifest['fixtures'] if f['id'].startswith('b3_')]
    for f in selected:
        for name,sha in f['files'].items():
            assert hashlib.sha256((hidden/name).read_bytes()).hexdigest()==sha
    s=rs.Session(str(Path(a.binary).resolve()));results={}
    try:
        scratch=Path(s.dir)
        for p in (here/'candidates/b3').iterdir(): shutil.copyfile(p,scratch/p.name)
        for label,cap in [('baseline','move.capsule.json'),('candidate','move_b3.capsule.json')]:
            submitted=s.call('capsule.submit',{'capsule_path':cap}); outcomes=[]
            for f in selected:
                test=json.loads((hidden/f['test']).read_text());test['capsule']=cap
                shutil.copyfile(hidden/f['world'],scratch/f['world'])
                name=label+'_'+f['test'];(scratch/name).write_text(json.dumps(test), newline="\n")
                answer=s.call('candidate.evaluate',{'candidate_id':submitted['candidate_id'],'test_path':name})
                outcomes.append({'fixture_id':f['id'],'result':answer})
                assert answer['status']==('failed' if label=='baseline' else 'passed'),answer
                if label=='baseline': assert any('column_mismatch' in x['reason'] for x in answer['failures'])
            results[label]={'submission':submitted,'outcomes':outcomes}
        record={'status':'passed_hidden_phase_checks','implementation_author':'/root','fixture_author':manifest['author_agent'],
                'manifest_sha256':hashlib.sha256((hidden/'manifest.json').read_bytes()).hexdigest(),
                'binary_sha256':hashlib.sha256(Path(a.binary).read_bytes()).hexdigest(),'results':results,
                'limitations':['phase fixtures only; full schedule/install and independent source review are separate gates']}
        Path(a.out).write_text(json.dumps(record,indent=2)+'\n', newline="\n");print(record['status'])
    finally:s.close()
if __name__=='__main__':main()
