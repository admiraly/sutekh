#!/usr/bin/env python3
"""Rebuild combined handoff documents, validate them, and create the ZIP."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path
from validate_pack import pack_files

ROOT = Path(__file__).resolve().parents[1]
SPEC_ORDER = [
    'ENGINE_CONSTITUTION.md', 'ARCHITECTURE.md', 'LIVE_IR_SPEC.md',
    'AGENT_PROTOCOL.md', 'MILESTONES.md', 'AGENTS.md',
    'docs/HOT_RELOAD_AND_STATE.md', 'docs/DATA_ABI_AND_STORAGE.md',
    'docs/DETERMINISM_AND_BFME.md', 'docs/BUILD_AND_VALIDATION.md',
    'docs/PERFORMANCE.md', 'docs/AGENT_RPC.md',
    'docs/BACKENDS_AND_DEPENDENCIES.md', 'docs/TEST_SPEC.md',
    'docs/GAME_PROFILES.md', 'docs/RENDERING_AND_ASSETS.md',
    'docs/RISK_REGISTER.md', 'docs/SOURCE_NOTES.md',
]
LINK = re.compile(r'(?<!!)\[([^\]]+)\]\(([^)]+)\)')

def root_links(text: str, source: Path) -> str:
    def replace(match: re.Match[str]) -> str:
        label, target = match.group(1), match.group(2)
        if '://' in target or target.startswith(('mailto:', 'sandbox:')):
            return match.group(0)
        filename, sep, fragment = target.partition('#')
        if not filename:
            return f'[{label}]({source.relative_to(ROOT).as_posix()}#{fragment})'
        resolved = (source.parent / filename).resolve()
        if not resolved.is_relative_to(ROOT):
            raise ValueError(f'Link escapes pack: {source} -> {target}')
        relative = resolved.relative_to(ROOT).as_posix()
        return f'[{label}]({relative}{sep}{fragment})'
    return LINK.sub(replace, text)

def combine(filename: str, title: str, paths: list[str], introduction: str) -> None:
    chunks = [f'# {title}\n\n{introduction}\n\n## Contents\n']
    for i, rel in enumerate(paths, 1):
        heading = (ROOT/rel).read_text(encoding='utf-8').splitlines()[0].lstrip('# ')
        chunks.append(f'{i}. [{heading}](#{"section-" + str(i)}) — `{rel}`\n')
    for i, rel in enumerate(paths, 1):
        source = ROOT / rel
        text = root_links(source.read_text(encoding='utf-8'), source)
        chunks.append(f'\n---\n\n<a id="section-{i}"></a>\n\n**Source document: `{rel}`**\n\n{text}')
    (ROOT/filename).write_text(''.join(chunks), encoding='utf-8')

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT.parent/'SUTEKH_Engine_Specs_and_Prompts.zip')
    args = parser.parse_args()
    combine('SUTEKH_FULL_SPEC.md', 'SUTEKH — Complete engineering specification', SPEC_ORDER,
            'Version 0.1 • 6 October 2026. This convenience copy combines the authoritative '
            'individual specifications. Timing values are unmeasured targets. Machine-readable '
            'schemas, examples, task DAG, checker, and specialized prompts are in the accompanying '
            'archive. Edit individual documents, then regenerate this copy. This is not an implemented engine.')
    prompts = [p.relative_to(ROOT).as_posix() for p in sorted((ROOT/'prompts').glob('*.md'))]
    combine('SUTEKH_ALL_PROMPTS.md', 'SUTEKH — All implementation and continuation prompts', prompts,
            'Version 0.1 • 6 October 2026. Give the master prompt to the orchestrator with the '
            'extracted specification pack. Give each specialist only its role prompt and task packet. '
            'The role prompts are copy-ready; no provider-specific agent API is assumed.')
    subprocess.run([sys.executable, str(ROOT/'tools/validate_pack.py')], check=True)
    files = sorted(p for p in pack_files()
                   if p.name not in ('PACK_INDEX.json', 'SHA256SUMS'))
    index = {
        'project':'Sutekh', 'spec_version':'0.1', 'date':'2026-10-06',
        'status':'specification_and_prompt_package; native engine not implemented',
        'spec_documents':SPEC_ORDER,
        'prompts':prompts,
        'task_count':len(json.loads((ROOT/'planning/tasks.json').read_text())['tasks']),
        'schemas':sorted(p.relative_to(ROOT).as_posix() for p in (ROOT/'schemas').glob('*.json')),
        'files':[{'path':p.relative_to(ROOT).as_posix(), 'bytes':p.stat().st_size,
                  'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in files],
    }
    (ROOT/'PACK_INDEX.json').write_text(json.dumps(index, indent=2)+'\n', encoding='utf-8')
    checksum_files = sorted(p for p in pack_files()
                            if p.name != 'SHA256SUMS')
    (ROOT/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  '
                                         f'{p.relative_to(ROOT).as_posix()}\n' for p in checksum_files), encoding='utf-8')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(pack_files()):
            z.write(p, arcname=f'{ROOT.name}/{p.relative_to(ROOT).as_posix()}')
    with zipfile.ZipFile(args.output) as z:
        bad = z.testzip()
        if bad:
            raise RuntimeError(f'ZIP integrity failed for {bad}')
    print(json.dumps({'archive':str(args.output),'bytes':args.output.stat().st_size,
                      'archive_sha256':hashlib.sha256(args.output.read_bytes()).hexdigest(),
                      'files_in_archive':len(z.namelist()),'spec_documents':len(SPEC_ORDER),
                      'prompts':len(prompts),'tasks':index['task_count']},indent=2))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
