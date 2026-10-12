# bench

- IR 0.2 entity tick (1,000,000 rows, N tick workers, 5 runs): `python3 bench/ir02_entities_bench.py --binary <build>/sutekh --workers 1,4 --runs 5 --steps 5 --out <build>/bench_ir02.json` (build the release `sutekh` target first; see `docs/AGENT_RPC.md` section 16 for why the script raises the per-tick caps).
