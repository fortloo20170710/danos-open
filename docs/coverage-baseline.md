# Production C coverage baseline

## Reproduce

Use Linux x86_64 CPython 3.12 (Ubuntu 24.04 CI) or 3.14 (this local baseline),
GCC and matching gcov, CMake, OpenSSL development files and openssl.

```sh
python3 -m venv build/coverage-tools
build/coverage-tools/bin/python -m pip install \
  --require-hashes --only-binary=:all: \
  -r danos-test/quality/requirements-coverage.lock
build/coverage-tools/bin/python danos-test/quality/run_coverage.py \
  --build-dir build/coverage-new-run \
  --gcovr "$PWD/build/coverage-tools/bin/gcovr" --jobs 2 \
  --fail-under-line 79
```

The build directory must not exist. The runner never deletes or reuses old
.gcda counters. It configures DANOS_COVERAGE=ON with GCC, -O0 and atomic profile
counter updates, builds, runs CTest and generates HTML, Cobertura XML and JSON
summary. Empty production-code reports fail. Failed tests remain failures even
if reporting succeeds; available test/report logs are retained.
Coverage must use a separate build from sanitizers and libFuzzer.

reports/manifest.json records commit/dirty state, GCC/gcov/gcovr versions,
command return codes, summary and file hashes. This is local measurement
provenance, not a signed attestation.

The report filters compiled production C code from existing implemented
modules; test executables/fixtures and generated model includes do not inflate
the denominator. It does not measure unimplemented modules, Python/Go tools,
protocol compliance, true VPP/FRR runtime qualification, ISO boots or PCI lanes.
Some deterministic tests exercise mocks. Line execution is not assertion
quality, correct behavior or adequate security coverage.

## 2026-10-20 current baseline

Fresh run: build-coverage/reports, GCC/gcov 15.2.0, gcovr 8.4, commit
03df625 with the P0–P2 worktree changes (dirty). CTest 81/81 passed.

| Metric | Executed / total | Coverage |
| --- | --- | --- |
| Lines | 7028 / 8756 | 80.3% |
| Functions | 611 / 690 | 88.6% |
| Branches | 4117 / 7003 | 58.8% |

A second identical run executed 7026 lines (80.2%), so individual paths
vary by about 0.1 points between runs. `run_coverage.py` accepts
`--fail-under-line PCT`, which records `line_gate` in the manifest and
exits 1 when the measured line percentage is below PCT. CI
(.github/workflows/coverage.yml) sets the gate at 79%, which leaves
roughly one point of headroom over the observed baseline and still
catches meaningful regressions. Raising the gate requires a fresh
baseline first.

## 2026-10-06 initial accepted baseline

Fresh run: build/coverage-acceptance-20261006-r2/reports, GCC/gcov 15.2.0,
gcovr 8.4. CTest 41/41 passed.

| Metric | Executed / total | Coverage |
| --- | --- | --- |
| Lines | 6070 / 7836 | 77.5% |
| Functions | 569 / 668 | 85.2% |
| Branches | 3393 / 6119 | 55.5% |

This baseline includes the dirty coverage-tool changes atop commit 3332263;
it is not attributed to an earlier clean release/tag. The first report had
zero production lines due to config-relative filter resolution and was rejected,
not counted as acceptance. The corrected fresh run used absolute-path regexes.

The first lane established a measurement rather than an arbitrary percentage
gate. The 2026-10-20 baseline now carries the non-regression gate. Further work
should improve assertions and error-path coverage; the VPP adapter's line
coverage reflects unexercised adapter paths, not evidence that real forwarding
passed.

## CI and dependency boundaries

.github/workflows/coverage.yml is an independent job. Its checkout and artifact
actions use exact upstream commit SHAs. All six resolved Python packages
(gcovr plus five dependencies) use exact versions and wheel SHA256s
(requirements-coverage.lock).
The lock deliberately includes only the tested Linux x86_64 3.12/3.14 wheels;
other platforms fail rather than silently installing source/unlisted packages.
Refresh hashes explicitly from trusted upstream downloads.

The [gcovr 8.4 documentation](https://gcovr.com/en/8.4/) describes the report
formats. The pinned upload action is
[upload-artifact v4.6.2](https://github.com/actions/upload-artifact/releases/tag/v4.6.2).
CI always attempts to retain reports/test logs, with read-only repository
permissions and a job timeout. The GitHub account billing lock still prevents
remote execution; local acceptance is not a remote green workflow.

System apt dependencies, other workflows, VPP package repositories and container
base images remain unpinned. SBOM, signing/attestations and removal of the large
DOCX from history remain open. No history rewrite or release publication was
performed by this change.
