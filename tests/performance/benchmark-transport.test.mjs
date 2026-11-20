import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { mkdir, readFile, rm } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compare, parseReport, transportMetricRules } from './benchmark-lib.mjs';
import { parseTransportOutput, splitFlags } from './benchmark-transport-lib.mjs';

function harness(overrides = {}) {
    return { type: 'transport-harness', ok: true, fingerprintVerified: true, smoke: false,
        packets: 10000, elapsedSeconds: 10,
        metrics: { transportCpuPercent: 50, transportPeakRssBytes: 10000000,
            transportDeliveredPacketsPerSecond: 1000, transportPayloadBytesPerSecond: 1000000,
            transportOneWayP50Ms: 0.1, transportOneWayP95Ms: 0.2, transportOneWayMaxMs: 0.4 },
        ...overrides };
}
function result(scale = 1) {
    return { type: 'benchmark-result', kind: 'transport', schema: 1, runId: randomUUID(), ok: true,
        fingerprintVerified: true, packets: 10000, elapsedMs: 10000,
        workload: { kind: 'transport', seconds: 10, latency: 'transport/socket/worker, not media' },
        host: { hostname: 'test', platform: 'darwin', arch: 'arm64', cpu: 'test', logicalCpus: 8 },
        provenance: { sourceCommit: 'abc', sourceDirty: false, trackedDiffSha256: 'diff',
            binaries: {}, transportBuild: { executable: { sha256: 'harness' }, inputs: { libraries: ['hash'] } } },
        metrics: Object.fromEntries(Object.keys(transportMetricRules).map(key => [key, scale * 100])) };
}

test('compiler flags preserve quoted paths without executing shell syntax', () => {
    assert.deepEqual(splitFlags('-O3 -DNDEBUG -I"with space" \'-DNAME=a b\''), [
        '-O3', '-DNDEBUG', '-Iwith space', '-DNAME=a b']);
    assert.throws(() => splitFlags('-O3; echo bad'), /Shell expansion/);
    assert.throws(() => splitFlags('$(command)'), /Shell expansion/);
    assert.throws(() => splitFlags('-I"unfinished'), /Unterminated/);
});

test('transport output requires real verified complete delivery, not a smoke or failed handshake', () => {
    assert.equal(parseTransportOutput(JSON.stringify(harness()), { seconds: 10 }).packets, 10000);
    for (const [value, pattern] of [
        [harness({ ok: false, error: 'bad fingerprint' }), /functional failure.*bad fingerprint/],
        [harness({ fingerprintVerified: false }), /verified DTLS fingerprint/],
        [harness({ smoke: true }), /Smoke output/],
        [harness({ elapsedSeconds: 1 }), /before requested duration/],
        [harness({ packets: 9999 }), /Inconsistent delivered packet rate/],
    ]) assert.throws(() => parseTransportOutput(JSON.stringify(value), { seconds: 10 }), pattern);
    assert.throws(() => parseTransportOutput('{"type":'), SyntaxError);
    assert.throws(() => parseTransportOutput(JSON.stringify(harness()) + '\n' + JSON.stringify(harness())),
        /Expected one/);
});

test('transport reports compare independently and flag CPU/rate/one-way latency regressions', () => {
    const before = Array.from({ length: 3 }, () => result());
    const after = Array.from({ length: 3 }, () => {
        const value = result(0.99);
        value.metrics.transportCpuPercent = 106;
        value.metrics.transportDeliveredPacketsPerSecond = 94;
        value.metrics.transportOneWayP95Ms = 111;
        return value;
    });
    const parsed = parseReport(JSON.stringify([...before, ...after]));
    assert.equal(parsed.length, 6);
    const report = compare(before, after);
    assert.equal(report.regression, true);
    assert.equal(report.metrics.transportCpuPercent.regressed, true);
    assert.equal(report.metrics.transportDeliveredPacketsPerSecond.regressed, true);
    assert.equal(report.metrics.transportOneWayP95Ms.regressed, true);
    assert.equal('applicationSctpRttP95Ms' in report.metrics, false);
    assert.match(report.caveat, /Neither measures media capture-to-render/);
    assert.throws(() => parseReport(JSON.stringify({ ...result(), fingerprintVerified: false })),
        /incomplete verified transport/);
    const smoke = { ...result(), type: 'transport-smoke-result' };
    assert.throws(() => parseReport(JSON.stringify(smoke)), /no benchmark results/);
    const nativeKind = { ...result(), kind: 'native' };
    assert.throws(() => compare([result()], [nativeKind]), /Incompatible workload/);
    const rebuilt = result();
    rebuilt.provenance.transportBuild.inputs.libraries = ['changed'];
    assert.throws(() => compare([result(), rebuilt], [result()]), /Mixed binary/);
});

test('transport wrapper missing metadata records failure without launching traffic', async () => {
    const directory = resolve('build', `transport-controller-${randomUUID()}`);
    await mkdir(directory, { recursive: true });
    const path = resolve(directory, 'report.jsonl');
    try {
        const script = fileURLToPath(new URL('./benchmark-transport.mjs', import.meta.url));
        const child = spawnSync(process.execPath, [script, 'run'], { encoding: 'utf8', timeout: 10000,
            env: { ...process.env, MRTC_BUILD_DIR: resolve(directory, 'missing'),
                MRTC_TRANSPORT_BINARY: resolve(directory, 'not-built'), MRTC_TRANSPORT_REPORT: path } });
        assert.equal(child.status, 1);
        const rows = (await readFile(path, 'utf8')).trim().split('\n').map(JSON.parse);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].ok, false);
        assert.equal(rows[0].kind, 'transport');
        assert.throws(() => parseReport(JSON.stringify(rows)), /failed functional run/);
    } finally { await rm(directory, { recursive: true, force: true }); }
});

test('compiled harness setup verifies SRTP payloads and rejects an incorrect certificate pin', {
    skip: !process.env.MRTC_TRANSPORT_TEST_BINARY && 'Set MRTC_TRANSPORT_TEST_BINARY for optional native setup smoke',
}, () => {
    const binary = resolve(process.env.MRTC_TRANSPORT_TEST_BINARY);
    const positive = spawnSync(binary, ['--smoke'], { encoding: 'utf8', timeout: 10000 });
    assert.equal(positive.status, 0, positive.stderr);
    const value = parseTransportOutput(positive.stdout, { smoke: true });
    assert.equal(value.packets, 32);
    const negative = spawnSync(binary, ['--bad-fingerprint'], { encoding: 'utf8', timeout: 10000 });
    assert.equal(negative.status, 1, negative.stderr);
    const failure = JSON.parse(negative.stdout);
    assert.equal(failure.ok, false);
    assert.match(failure.error, /fingerprint-verified DTLS\/SRTP setup failed/);
    assert.doesNotMatch(failure.error, /incorrect fingerprint was accepted/);
});
