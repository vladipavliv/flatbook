# Releasing

## Version bump rules

- `0.x` - the minor version may break the API (`0.1.0` → `0.2.0`); a patch
  (`0.1.0` → `0.1.1`) is a backward-compatible fix only.
- `1.0.0` - freezes the public API.

The version is declared in **three places** and must stay in sync (checked by
`./dev.sh` in CI via `scripts/check_version.py`):

- `CMakeLists.txt` (`project(... VERSION x.y.z)`)
- `vcpkg.json` (`"version"`)
- `conanfile.py` (`version = "..."`)

## Checklist for a release

1. `./dev.sh test` and `./dev.sh sanitize` green.
2. `./dev.sh tsan` green.
3. `./dev.sh headers` - every header self-contained.
4. `./dev.sh tidy` - clang-tidy clean.
5. `./dev.sh bench --refs --report` - regenerate `docs/BENCHMARKS.md` on a quiet,
   frequency-locked machine.
6. Update the version in all three files (above) and run `scripts/check_version.py`.
7. Update `CHANGELOG.md` - move `Unreleased` to a dated release, list all Added /
   Changed / Breaking changes.
8. Tag `vX.Y.Z` and push.

## Exit criteria for 1.0.0

The items below are the plan for leaving `0.x`. Check them off as they land.

- [x] Reason codes on every result (`Reason` + `CancelResult::reason`).
- [x] `docs/SEMANTICS.md` (edge cases tied to tests) and `docs/API.md`.
- [x] Snapshot / journal / persistence with a round-trip test.
- [x] LOBSTER feed-driven replay and a validator with a fixture.
- [x] Python bindings (`pip install .`, pytest, stubs, batch API).
- [x] Honest benchmarks (baseline subtraction, `--report`, real matcher baseline).
- [x] libFuzzer targets (engine, persistence, LOBSTER parser) in CI.
- [ ] aarch64 CI matrix green.
- [ ] `pip install` / `find_package` smoke tests for the C++ consumer in CI.
- [ ] At least one external user / third-party audit.

`1.0.0` is **not** a claim of production-readiness for live trading; it is a
promise that the API is stable.
