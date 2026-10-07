> Historical record. Current licensing is governed by [LICENSE](../../LICENSE) and [NOTICE.md](../../NOTICE.md); prior decisions below are not current grants.

# Public repository and license — 7 October 2026

The owner requested a public repository and commercial monetization along
the lines of Godot. Selected the standard MIT license, which Godot also uses,
and added `LICENSE` with the owner's GitHub identity, admiraly, as copyright
holder. Updated README, the dependency decision ledger, and durable status.
Earlier handoffs describe historical setup; this decision supersedes their
private-repository and pending-license state.

MIT permits commercial use, modification, distribution, and sale while
requiring preservation of copyright and license notices. Games can be
proprietary. Engine copies can also be sold, and paid support, services,
hosting, training, or separate products are possible business models.
Others receive the same commercial freedoms without mandatory royalties;
MIT does not reserve commercial use exclusively for the owner. Third-party
dependency and asset licenses remain separate.

Sources checked: https://godotengine.org/license/ and
https://opensource.org/license/mit. Standard license text retrieved through
`gh api licenses/mit`; only the year and copyright holder were substituted.

Rebuilt combined specs, inventory, and checksums with the pack assembler.
Pack/example and JSON Schema checks passed; no native engine implementation
or milestone acceptance is claimed. Git tracks specs, examples, setup tools,
and evidence; local toolchains, environments, and build artifacts remain
excluded. A scan for common credential patterns found no matches in source.

Repository publication uses `gh repo edit admiraly/sutekh --visibility public
--accept-visibility-change-consequences`. The next implementation task remains
M0-00.
