# Performance comparison

Use Linux or macOS with the [native dependencies](../README.md#dependencies-and-build).
Compare native SFU media and a separate C DTLS/SRTP UDP workload.
No browser/npm dependencies are needed; native runs use [shared support](../tests/support/).

## Recommended procedure

Commit/freeze the candidate. Use a distinct clean baseline and **new** evidence directory:
```sh
npm --prefix tests/performance test
git worktree add --detach ../mrtc-baseline 9a9319b
export MRTC_PERF_BASELINE_SOURCE="$PWD/../mrtc-baseline"
export MRTC_PERF_CANDIDATE_SOURCE="$PWD"
export MRTC_PERF_DIR=build/performance-comparison
node tests/performance/benchmark-suite.mjs prepare

# Coordinate an exclusive measurement window before this step.
node tests/performance/benchmark-suite.mjs run
```

Both sources must be clean and committed. Preparation builds matched,
unsanitized Release variants, checks dependency/compiler/feature identity,
and verifies transport delivery and rejection of an incorrect certificate pin.
Do not edit sources, rebuild, or change dependencies between `prepare` and `run`.
Output/builds must remain outside the baseline worktree.

The default plan is three AB/BA/AB pairs: eight native peers, 60-second warmup,
120-second media measurement; then separate five-second warmup/15-second
transport measurements. Runs have a fixed 15-second idle gap.

## Results

Read `comparison.json` alongside raw JSONL reports, per-run logs, manifest and
environment captures under `MRTC_PERF_DIR`. All paired/group deltas and noise
flags are retained; do not discard inconvenient repeats.

| Exit | Meaning |
|---|---|
| 0 | No flagged regression, with sufficient repeat evidence |
| 1 | Functional/configuration failure or flagged regression |
| 2 | Insufficient/noisy evidence without a flagged regression |

CPU increases/throughput decreases over 5% and RSS/latency increases over 10% are regression
flags. Noise does not erase a flagged regression. Shared GitHub runners always
produce advisory/insufficient evidence unless a failure already requires exit 1.

**Metric meanings:** native throughput counts decoded media; native latency is
controller-mediated SCTP round-trip time. Transport latency is one-way
crypto/socket/worker time, not capture-to-render media latency. Native RSS is
sampled resident memory; transport RSS is process lifetime peak. Neither is
live heap, leak-freedom, visual quality or maximum supported capacity.

## Common overrides (set before preparation)
| Variable | Default | Bounds |
|---|---:|---|
| `MRTC_PERF_BASELINE_REF` | `9a9319b` | Expected baseline commit/ref |
| `MRTC_PERF_REPEATS` | 3 | 3..5 pairs |
| `MRTC_PERF_PEERS` | 8 | 2..8 |
| `MRTC_PERF_WARMUP_SECONDS` | 60 | 5..120 |
| `MRTC_PERF_SECONDS` | 120 | 60..300 |
| `MRTC_PERF_BUILD_JOBS` | 4 | 1..8 |
| `MRTC_PERF_TIMEOUT_SECONDS` | 3600 | 300..7200 |

`verify` replaces `prepare` when supplying both `MRTC_PERF_BASELINE_BUILD` and
`MRTC_PERF_CANDIDATE_BUILD`; profiles must still match. `all` prepares and
measures together. `compare` reconstructs a missing terminal comparison from
existing reports without rerunning workloads. Existing terminal outputs are
never overwritten; source/tool changes require a fresh preparation.

## Individual tools and CI
`MRTC_BUILD_DIR` selects an existing Release build (root-relative or absolute).
Under `tests/performance`, npm scripts `benchmark`,
`benchmark:transport:build`, `benchmark:transport:smoke`, `benchmark:transport`,
and `benchmark:compare` expose the individual tools. Prefer the paired suite
above for a baseline/candidate conclusion.

On GitHub, select **manual bounded performance comparison > Run workflow** and
choose `baseline_ref`. It builds both variants and uploads evidence even when
regressions or inconclusive results make the job fail.
