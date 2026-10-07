# Contributing to Sutekh

<!-- SPDX-FileCopyrightText: 2026 admiraly -->
<!-- SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 -->

Sutekh is a source-available engine project under
[PolyForm Perimeter License 1.0.1](LICENSE), not OSI Open Source. Please read
[NOTICE.md](NOTICE.md) and the [contributor agreement](CLA.md) before submitting
material. Contributors retain copyright; the additional grant to **admiraly**
preserves the owner's ability to offer commercial and other licenses.

## Scope and workflow

The project currently contains specifications, fixtures, and development tools,
not an implemented engine. Start with the M0 tasks in
[planning/tasks.json](planning/tasks.json) and follow [AGENTS.md](AGENTS.md).
Discuss significant work in an issue first. Use a scoped branch, preserve
existing work, and include the problem, change, and relevant evidence in a PR.
Keep shared-contract changes coordinated with the maintainer. Do not broaden
milestones or change expected results just to make a check pass.

Run the appropriate checks. For package/documentation changes:

```powershell
.\.venv\Scripts\python.exe tools/validate_pack.py
.\.venv\Scripts\python.exe tools/check_repository_license.py
```

Native changes also need targeted compilation and relevant execution tests.
Report checks actually run, failures, and unrun checks. Timing targets are not
benchmark results. Preserve the architecture and specification contracts.

## Contributor agreement: explicit acceptance required

Before a contribution is merged, each copyright holder must expressly accept
[CLA.md, version 1.0](CLA.md). For each PR, post the following statement in a
comment from your own GitHub account, replacing the placeholders:

> I, [name or GitHub identity], have read and agree to the Sutekh Contributor
> License Agreement version 1.0 in CLA.md at [full base commit SHA] for my
> contributions in this pull request [PR URL]. I retain my copyright and grant
> admiraly the rights described in that agreement. I have authority to make
> these grants, including any required employer authorization.

The maintainer must verify the referenced CLA text, contributor identity,
scope, and authority; retain the comment permalink, agreement version/base
SHA, and accepted contribution commit SHAs in the PR record. Reconfirm if the
CLA or covered authorship changes. An authorized employer representative must
provide acceptance where the employer holds the rights. Do not post private
legal documents publicly; arrange their exchange separately with the owner.

Opening a PR, a checkbox, a signed-off commit, or running an automated check
is **not** treated as CLA acceptance. No CLA bot or signature service is
currently configured. The maintainer must not merge without the recorded
acceptance and authority checks. Existing contributions are not retroactively
bound by this process. The CLA grants rights to the owner; it does not grant
the general public unrestricted rights to use Sutekh.

## Licensing and attribution

New project-owned source files should include the appropriate SPDX header:

```text
SPDX-FileCopyrightText: [year] [actual copyright holder]
SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
```

Use language-appropriate comments, after a shebang where necessary. Do not
insert comments into strict JSON or change invalid test fixtures. Preserve
contributor ownership and existing third-party headers. Identify all copied
material, its source, license, and notices; discuss dependencies before adding
them. Do not claim third-party material as your own or grant rights you lack.
Synthetic examples must be original or properly cleared; proprietary BFME
assets and installation data must remain outside the repository.

## Reports and communication

Use GitHub issues for reproducible bugs and scoped proposals; include versions,
commands, and minimal fixtures. Keep discussions respectful and technical.
For vulnerabilities or sensitive reports, follow [SECURITY.md](SECURITY.md).
