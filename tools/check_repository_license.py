#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Check Sutekh's current licensing metadata; not a legal opinion or CLA verifier."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IDENTIFIER = 'LicenseRef-PolyForm-Perimeter-1.0.1'
UPSTREAM_SHA256 = '5c7a5ccd847fcc285dda039e511ba013693fe979dfc5faee47f6fb59c7add337'


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def main() -> int:
    try:
        license_bytes = (ROOT / 'LICENSE').read_bytes()
        require(hashlib.sha256(license_bytes).hexdigest() == UPSTREAM_SHA256,
                'LICENSE differs from the pinned upstream Perimeter 1.0.1 text')
        require((ROOT / 'LICENSES' / (IDENTIFIER + '.txt')).read_bytes() == license_bytes,
                'SPDX reference text differs from LICENSE')
        status = json.loads((ROOT / 'planning/status.json').read_text(encoding='utf-8'))
        require(status['license_decision'] == 'PolyForm-Perimeter-1.0.1', 'Stale license decision')
        require(status['license_spdx_expression'] == IDENTIFIER, 'Wrong SPDX expression')
        require(status['copyright_holder'] == 'admiraly', 'Owner copyright identity changed')
        index = json.loads((ROOT / 'PACK_INDEX.json').read_text(encoding='utf-8'))
        require(index['license_spdx_expression'] == IDENTIFIER, 'Stale generated license metadata')
        require(index['license_file'] == 'LICENSE' and index['notice_file'] == 'NOTICE.md',
                'Generated manifest points to incorrect license/notice')

        current = ['README.md', 'NOTICE.md', 'CONTRIBUTING.md', 'CLA.md',
                   'SECURITY.md', 'COMMERCIAL_LICENSE.md', 'THIRD_PARTY_NOTICES.md',
                   '.github/PULL_REQUEST_TEMPLATE.md']
        broad_grants = [r'Sutekh is (?:licensed|released) under (?:the )?\[?(?:MIT|Apache|GPL|MPL)',
                        r'(?:competitive|competing) (?:use|redistribution) (?:is|remains) permitted',
                        r'contributors (?:assign|transfer) (?:their )?copyright',
                        r'(?:opening|submitting) a (?:PR|pull request) constitutes CLA acceptance']
        for rel in current:
            text = (ROOT / rel).read_text(encoding='utf-8')
            for pattern in broad_grants:
                require(not re.search(pattern, text, re.I), f'Possible conflicting grant in {rel}')

        notice = (ROOT / 'NOTICE.md').read_text(encoding='utf-8')
        require('Required Notice: Copyright (c) 2026 admiraly' in notice, 'Missing owner notice')
        require('not OSI Open' in notice and 'Source' in notice, 'Missing source-available distinction')
        require('does not\npurport to revoke rights already granted' in notice, 'Missing prior-release boundary')
        cla = (ROOT / 'CLA.md').read_text(encoding='utf-8')
        for clause in ['You retain copyright', 'nonexclusive', 'irrevocable', 'sublicense',
                       'commercial', 'Patent grant', 'authority', 'express']:
            require(clause.lower() in cla.lower(), f'Missing CLA clause: {clause}')
        contributing = (ROOT / 'CONTRIBUTING.md').read_text(encoding='utf-8')
        require('must not merge without' in contributing, 'Missing explicit CLA merge gate')

        for rel in ['tools/validate_pack.py', 'tools/assemble_handoff.py',
                    'tools/env-windows.ps1', 'tools/requirements-dev.txt',
                    'tools/check_repository_license.py']:
            text = (ROOT / rel).read_text(encoding='utf-8')
            require('SPDX-License-Identifier: ' + IDENTIFIER in text, f'Missing header in {rel}')
            require(not re.search(r'SPDX-License-Identifier: (?:MIT|Apache|GPL|MPL)', text),
                    f'Conflicting project-source header in {rel}')

        for rel in ['SUTEKH_FULL_SPEC.md', 'SUTEKH_ALL_PROMPTS.md']:
            text = (ROOT / rel).read_text(encoding='utf-8')
            require('not OSI Open Source' in text[:1800] and IDENTIFIER in text[:1800],
                    f'Missing current-license banner in {rel}')
            require('those entries do not grant a different' in text[:1800],
                    f'Unqualified historical licensing text in {rel}')
        require(not (ROOT / 'LICENSE-MIT').exists(), 'Unexpected parallel project license')
        print(json.dumps({'status': 'passed', 'scope': 'current_repository_license_metadata',
                          'upstream_license_sha256': UPSTREAM_SHA256,
                          'spdx_expression': IDENTIFIER,
                          'cla_acceptances': 'manual review required; not checked by this tool',
                          'legal_validity': 'not assessed'}, indent=2))
        return 0
    except (ValueError, KeyError, OSError) as error:
        print(json.dumps({'status': 'failed', 'error': str(error)}, indent=2))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
