# bench

- IR 0.2 entity tick (1,000,000 rows, N tick workers, 5 runs): `python3 bench/ir02_entities_bench.py --binary <build>/sutekh --workers 1,4 --runs 5 --steps 5 --out <build>/bench_ir02.json` (build the release `sutekh` target first; see `docs/AGENT_RPC.md` section 16 for why the script raises the per-tick caps).
- Integrated audio runtime CPU per second of audio (512 logical / 64 physical voices, 16 buses, 8 ducks, 3 music stems, clustering): build `bench_audio_system` in a Release configuration and run `<build>/bench_audio_system [seconds] [json-out] [physical-voices]`; results and method are in `docs/EVENT_AUDIO_INTEGRATION.md`.
