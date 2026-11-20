import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { cpuSeconds, integer } from '../support/process.mjs';
import { parseReport, compare, metricRules, decodedDelta } from './benchmark-lib.mjs';

const script = fileURLToPath(new URL('./benchmark.mjs', import.meta.url));
function result(scale = 1) {
    return { type: 'benchmark-result', schema: 1, runId: randomUUID(), ok: true, workload: { peers: 8 },
        host: { hostname: 'test', platform: 'darwin', arch: 'arm64', cpu: 'test', logicalCpus: 8 },
        provenance: { sourceCommit: 'abc', sourceDirty: false, trackedDiffSha256: 'diff',
            binaries: { cli: { sha256: 'hash' } } },
        metrics: Object.fromEntries(Object.keys(metricRules).map(key => [key, 100 * scale])) };
}

test('CPU-time parsing supports macOS fractions, Linux hours and day rollover', () => {
    assert.equal(cpuSeconds('1:02.34'), 62.34);
    assert.equal(cpuSeconds('02:03:04'), 7384);
    assert.equal(cpuSeconds('1-02:03:04.5'), 93784.5);
    for (const text of ['oops', '1:60', '-1:00', '00:99:00']) assert.throws(() => cpuSeconds(text));
    assert.throws(() => integer({ N: '60seconds' }, 'N', 60, 60, 600));
    assert.throws(() => integer({ N: '59' }, 'N', 60, 60, 600));
});

test('reports accept JSON/JSONL and reject failed, truncated and incomplete results', () => {
    assert.equal(parseReport(JSON.stringify(result())).length, 1);
    assert.equal(parseReport('{"type":"sample"}\n' + JSON.stringify(result()) + '\n').length, 1);
    assert.equal(parseReport(JSON.stringify([result(), result()])).length, 2);
    assert.throws(() => parseReport(JSON.stringify({ ...result(), ok: false, error: 'decode stalled' })),
        /failed functional run.*decode stalled/);
    assert.throws(() => parseReport('{"type":"sample"}\n{"type":'), /invalid JSON/);
    assert.throws(() => parseReport('{"type":"sample"}'), /no benchmark results/);
    const missing = result();
    delete missing.metrics.serverCpuPercent;
    assert.throws(() => parseReport(JSON.stringify(missing)), /invalid metric serverCpuPercent/);
});

test('comparison reports all median deltas, threshold regressions and repeat noise', () => {
    const before = [result(), result(), result(1.01)];
    const after = [result(1.12), result(1.12), result(2)];
    const report = compare(before, after);
    assert.equal(Object.keys(report.metrics).length, Object.keys(metricRules).length);
    assert.equal(report.regression, true);
    assert.equal(report.insufficientEvidence, true);
    assert(Math.abs(report.metrics.serverCpuPercent.deltaPercent - 12) < 1e-8);
    assert.equal(report.metrics.serverPeakRssBytes.regressed, true);
    assert.equal(report.metrics.decodedVideoFramesPerSecond.regressed, false);
    const slower = result();
    slower.metrics.decodedVideoFramesPerSecond = 94;
    slower.metrics.serverCpuPercent = 105;
    const strict = compare([result(), result(), result()],
        Array.from({ length: 3 }, () => ({ ...slower, runId: randomUUID() })));
    assert.equal(strict.metrics.decodedVideoFramesPerSecond.regressed, true);
    assert.equal(strict.metrics.serverCpuPercent.regressed, false);
    assert.equal(compare([result()], [result()]).insufficientEvidence, true);
    const noisy = compare([result(0.9), result(), result(1.2)], [result(), result(1.06), result(1.3)]);
    assert.equal(noisy.metrics.serverCpuPercent.evidence, 'noisy-repeat-more');
    assert.equal(noisy.metrics.serverCpuPercent.regressed, true);
    const incompatible = result();
    incompatible.workload.peers = 2;
    assert.throws(() => compare([result()], [incompatible]), /Incompatible workload/);
    const rebuilt = result();
    rebuilt.provenance.binaries.cli.sha256 = 'new';
    assert.throws(() => compare([result(), rebuilt], [result()]), /Mixed binary/);
    const duplicate = result();
    assert.throws(() => compare([duplicate, duplicate], [result()]), /Duplicate run IDs/);
});

test('decoded throughput rejects stale sources, stalled decode and changed epochs', () => {
    const snapshot = count => ({ mode: 'sfu', status: 'active', epoch: 2, generation: 1,
        sources: [{ peer_id: 'b', label: 'camera', epoch: 2, generation: 1, video_frames: count },
            { peer_id: 'b', label: 'mic', epoch: 2, generation: 1, audio_samples: count * 480 }] });
    assert.deepEqual(decodedDelta([snapshot(1)], [snapshot(4)], ['a', 'b']),
        { videoFrames: 3, audioSamples: 1440 });
    assert.throws(() => decodedDelta([snapshot(1)], [snapshot(1)], ['a', 'b']), /No fresh decoded/);
    const stale = snapshot(4);
    stale.sources[0].epoch = 1;
    assert.throws(() => decodedDelta([snapshot(1)], [stale], ['a', 'b']), /Missing current decoded/);
    assert.throws(() => decodedDelta([snapshot(1)], [{ ...snapshot(4), epoch: 3 }], ['a', 'b']),
        /Topology changed/);
});

test('comparison CLI exits distinguish exact thresholds, regressions and insufficient evidence', async () => {
    const directory = resolve('build', `benchmark-compare-controller-${randomUUID()}`);
    await mkdir(directory, { recursive: true });
    const before = resolve(directory, 'before.json');
    const after = resolve(directory, 'after.json');
    const compareScript = fileURLToPath(new URL('./benchmark-compare.mjs', import.meta.url));
    const invoke = () => spawnSync(process.execPath, [compareScript, '--baseline', before, '--candidate', after],
        { encoding: 'utf8', timeout: 10000 });
    const repeats = value => Array.from({ length: 3 }, () => ({ ...value, runId: randomUUID() }));
    try {
        await writeFile(before, JSON.stringify(repeats(result())));
        const boundary = result(0.99);
        Object.assign(boundary.metrics, { serverCpuPercent: 105, decodedVideoFramesPerSecond: 95,
            serverPeakRssBytes: 110, applicationSctpRttP95Ms: 110 });
        await writeFile(after, JSON.stringify(repeats(boundary)));
        const accepted = invoke();
        assert.equal(accepted.status, 0, accepted.stderr);
        assert.equal(JSON.parse(accepted.stdout).regression, false);
        for (const [key, value] of Object.entries({ serverCpuPercent: 105.01,
            decodedVideoFramesPerSecond: 94.99, serverPeakRssBytes: 110.01,
            applicationSctpRttP95Ms: 110.01 })) {
            const regressed = structuredClone(boundary);
            regressed.metrics[key] = value;
            await writeFile(after, JSON.stringify(repeats(regressed)));
            const rejected = invoke();
            assert.equal(rejected.status, 1, rejected.stderr);
            assert.equal(JSON.parse(rejected.stdout).metrics[key].regressed, true);
        }
        await writeFile(after, JSON.stringify(boundary));
        const insufficient = invoke();
        assert.equal(insufficient.status, 2, insufficient.stderr);
        assert.equal(JSON.parse(insufficient.stdout).insufficientEvidence, true);
        await writeFile(after, '{"truncated":');
        const malformed = invoke();
        assert.equal(malformed.status, 1);
        assert.match(malformed.stderr, /invalid JSON/);
        assert.equal(malformed.stdout, '');
    } finally { await rm(directory, { recursive: true, force: true }); }
});

test('missing build produces exactly one failed result without launching a workload', async () => {
    const directory = resolve('build', `benchmark-controller-${randomUUID()}`);
    await mkdir(directory, { recursive: true });
    try {
        const path = resolve(directory, 'report.jsonl');
        const child = spawnSync(process.execPath, [script], { encoding: 'utf8', timeout: 10000,
            env: { ...process.env, MRTC_BUILD_DIR: resolve(directory, 'missing'),
                MRTC_BENCH_REPORT: path } });
        assert.equal(child.status, 1);
        const rows = (await readFile(path, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
        assert.equal(rows.length, 1);
        assert.equal(rows[0].type, 'benchmark-result');
        assert.equal(rows[0].ok, false);
        assert.match(rows[0].error, /CMakeCache/);
        assert.doesNotMatch(child.stdout, /PASS/);
        assert.throws(() => parseReport(JSON.stringify(rows[0])), /failed functional run/);
    } finally { await rm(directory, { recursive: true, force: true }); }
});
