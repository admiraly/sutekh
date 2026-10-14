# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Regenerate all experiment candidates in isolation and compare exact reviewed bytes."""
import importlib.util, tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parent

def load(name):
    spec=importlib.util.spec_from_file_location(name,HERE/(name+'.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
    with tempfile.TemporaryDirectory(prefix='sutekh-behavior-regeneration-') as directory:
        temp=Path(directory)
        load('build_b3_candidate').build(temp/'candidates/b3')
        b2=load('build_b2_candidate');b2.HERE=temp;b2.build()
        b1=load('build_b1_candidate');b1.HERE=temp;b1.build()
        expected=sorted((HERE/'candidates').glob('*/*.json'))
        actual=sorted((temp/'candidates').glob('*/*.json'))
        assert len(actual)==len(expected)==10
        for source in expected:
            relative=source.relative_to(HERE);assert source.read_bytes()==(temp/relative).read_bytes(),relative
        print('10 reviewed candidate files regenerate byte-for-byte')
if __name__=='__main__':main()
