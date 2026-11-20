import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mediaObservations, metricRules, normalizeLinkedLibraries, transportMetricRules } from './benchmark-lib.mjs';
import { assertCompatible, combineEvidence, normalizedProfile, pairedPlan, runOwned,
    suiteExitCode, suiteSettings } from './benchmark-suite-lib.mjs';

const script = fileURLToPath(new URL('./benchmark-suite.mjs', import.meta.url));
function fixture() {
    const settings = suiteSettings({});
    const provenance = variant => ({ sourceCommit: variant, sourceDirty: false, trackedDiffSha256: variant,
        binaries: { cli: { sha256: `${variant}-binary` } } });
    const manifest = { schema: 1, type: 'performance-suite-manifest', settings,
        compatibility: { matched: true }, priorEvidence: [{ warning: 'Retained noisy baseline, excluded from comparison' }],
        variants: Object.fromEntries(['baseline', 'candidate'].map(variant => [variant, { provenance: provenance(variant) }])) };
    const entries = [];
    for (const kind of ['media', 'transport']) for (const slot of pairedPlan(3)) {
        const transport = kind === 'transport', scale = slot.variant === 'baseline' ? 100 : 99;
        const result = { type: 'benchmark-result', schema: 1, runId: randomUUID(), ok: true,
            ...(transport ? { kind: 'transport', fingerprintVerified: true, packets: 32 } : {}),
            elapsedMs: transport ? 15000 : 120000, host: { hostname: 'unit', cpu: 'unit', logicalCpus: 8 },
            workload: transport ? { seconds: 15, warmupSeconds: 5, packetCap: 4000000 }
                : { seconds: 120, warmupSeconds: 60, peers: 8, sampleMs: 2000 },
            provenance: { ...provenance(slot.variant), ...(transport ? { transportBuild: {} } : {}) },
            metrics: Object.fromEntries(Object.keys(transport ? transportMetricRules : metricRules).map(key => [key, scale])) };
        entries.push({ kind, ...slot, result });
    }
    return { manifest, entries };
}

test('bounded suite defaults and chronological AB/BA/AB plan are explicit', () => {
    assert.equal(suiteSettings({}).warmupSeconds, 60);
    assert.equal(suiteSettings({}).seconds, 120);
    assert.deepEqual(pairedPlan(3).map(value => `${value.variant}:${value.pair}`), [
        'baseline:1', 'candidate:1', 'candidate:2', 'baseline:2', 'baseline:3', 'candidate:3']);
    assert.throws(() => suiteSettings({ MRTC_PERF_SECONDS: '120; echo bad' }), /must be/);
    assert.throws(() => suiteSettings({ MRTC_PERF_REPEATS: '100' }), /must be/);
    assert.throws(() => pairedPlan(2), /3..5/);
});

test('compatibility compares compiler/features/flags/dependency hashes, not source commit', () => {
    const p = { buildProfile: { CMAKE_BUILD_TYPE: 'Release', CMAKE_C_FLAGS_RELEASE: '-O3 -DNDEBUG',
        MRTC_ENABLE_TWCC: 'ON', MRTC_ENABLE_TSAN: 'OFF', OPUS_VERSION: '1.6.1' },
    compilers: [{ configuration: 'set(CMAKE_C_COMPILER_ID "AppleClang")\nset(CMAKE_C_COMPILER_VERSION "21")' }] };
    const profile = normalizedProfile(p, { ssl: 'hash' }, { width: 320 });
    assertCompatible(profile, structuredClone(profile));
    for (const change of [
        value => { value.features = []; },
        value => { value.dependencyHashes.ssl = 'changed'; },
        value => { value.flags.CMAKE_C_FLAGS_RELEASE = '-O0'; },
        value => { value.configuredMediaDefaults.width = 640; },
    ]) {
        const value = structuredClone(profile); change(value);
        assert.throws(() => assertCompatible(profile, value), /compiler, features, dependencies/);
    }
    assert.throws(() => normalizedProfile({ ...p,
        buildProfile: { ...p.buildProfile, MRTC_ENABLE_TSAN: 'ON' } }, {}, {}), /instrumentation/);
});

test('Linux ASLR addresses do not make unchanged linked-library provenance appear dirty', () => {
    const before = 'libssl.so.3 => /lib/libssl.so.3 (0x00007abc)\n/lib/ld-linux.so.2 (0x000012ab)';
    const after = 'libssl.so.3 => /lib/libssl.so.3 (0x00003def)\n/lib/ld-linux.so.2 (0x000045cd)';
    assert.equal(normalizeLinkedLibraries(before), normalizeLinkedLibraries(after));
    assert.match(normalizeLinkedLibraries(before), /\/lib\/libssl.so.3/);
});

test('unknown runtime dimensions/bitrate are never inferred from counters or target bitrate', () => {
    const snapshot = [{ sources: [{ label: 'camera', peer_id: 'p', video_frames: 500, target_bitrate_bps: 350000 }] }];
    const unknown = mediaObservations(snapshot);
    assert.equal(unknown.runtimeResolution, 'unknown');
    assert.equal(unknown.runtimeEncodedBitrate, 'unknown');
    snapshot[0].sources[0] = { ...snapshot[0].sources[0], width: 320, height: 240, encoded_bitrate_bps: 300000 };
    const exposed = mediaObservations(snapshot);
    assert.equal(exposed.runtimeResolution, 'exposed-in-stats');
    assert.equal(exposed.sources[0].encodedBitrateBps, 300000);
});

test('suite comparison preserves every group/pair delta, historical caveat and all failure flags', () => {
    const { manifest, entries } = fixture();
    const good = combineEvidence(manifest, entries);
    assert.equal(suiteExitCode(good), 0);
    assert.equal(good.sections.media.pairedMetrics.serverCpuPercent.observations.length, 3);
    assert(Math.abs(good.sections.media.pairedMetrics.serverCpuPercent.medianDeltaPercent + 1) < 1e-8);
    assert.equal(Object.keys(good.sections.transport.metrics).length, Object.keys(transportMetricRules).length);
    assert.equal(good.priorEvidence.length, 1);
    assert.match(good.limitations.join(' '), /No media capture-to-render/);
    manifest.settings.sharedRunner = true;
    assert.equal(suiteExitCode(combineEvidence(manifest, entries)), 2);
    manifest.settings.sharedRunner = false;
    for (const entry of entries.filter(value => value.kind === 'media' && value.variant === 'candidate'))
        entry.result.metrics.serverCpuPercent = 106;
    const regression = combineEvidence(manifest, entries);
    assert.equal(suiteExitCode(regression), 1);
    assert.equal(regression.sections.media.pairedMetrics.serverCpuPercent.regressed, true);
    const missing = combineEvidence(manifest, entries.slice(1));
    assert.equal(missing.ok, false);
    assert.equal(missing.insufficientEvidence, true);
    assert.equal(suiteExitCode(missing), 1);
    const duplicate = combineEvidence(manifest, [...entries, entries[0]]);
    assert.equal(duplicate.ok, false);
});

test('owned-command deadline fails and terminates its bounded process group', async () => {
    const directory = resolve('build', `suite-deadline-${randomUUID()}`);
    await mkdir(directory, { recursive: true });
    try {
        const log = resolve(directory, 'owned.log');
        let ownedPid;
        await assert.rejects(runOwned(process.execPath, ['-e',
            'console.log("owned deadline fixture"); setInterval(() => {}, 1000)'], {
            cwd: directory, env: process.env, timeoutMs: 150, log, quiet: true,
        }), error => { ownedPid = error.ownedPid; return error.timedOut && /deadline/.test(error.message); });
        assert(Number.isSafeInteger(ownedPid));
        assert.throws(() => process.kill(ownedPid, 0), { code: 'ESRCH' });
        assert.match(await readFile(log, 'utf8'), /ownedPid/);
    } finally { await rm(directory, { recursive: true, force: true }); }
});

test('suite compare CLI persists one artifact and fails missing/mismatched evidence', async () => {
    const parent = resolve('build', `suite-cli-${randomUUID()}`);
    await mkdir(parent, { recursive: true });
    try {
        for (const mode of ['good', 'shared', 'missing', 'wrong-warmup']) {
            const directory = resolve(parent, mode);
            await mkdir(directory);
            const { manifest, entries } = fixture();
            if (mode === 'shared') manifest.settings.sharedRunner = true;
            if (mode === 'wrong-warmup') entries[0].result.workload.warmupSeconds = 15;
            await writeFile(resolve(directory, 'manifest.json'), JSON.stringify(manifest));
            for (const entry of entries.slice(mode === 'missing' ? 1 : 0))
                await writeFile(resolve(directory, `${entry.kind}-${entry.variant}-${entry.pair}.jsonl`),
                    JSON.stringify(entry.result) + '\n');
            const result = spawnSync(process.execPath, [script, 'compare'], { encoding: 'utf8', timeout: 10000,
                env: { ...process.env, MRTC_PERF_DIR: directory } });
            assert.equal(result.status, mode === 'good' ? 0 : mode === 'shared' ? 2 : 1, result.stderr);
            const artifact = JSON.parse(await readFile(resolve(directory, 'comparison.json'), 'utf8'));
            assert.equal(artifact.ok, ['good', 'shared'].includes(mode));
            assert.equal(artifact.sections.transport.metrics.transportOneWayP95Ms.deltaPercent < 0, true);
        }
        const directory = resolve(parent, 'bad-input');
        const failed = spawnSync(process.execPath, [script, 'prepare'], { encoding: 'utf8', timeout: 10000,
            env: { ...process.env, MRTC_PERF_DIR: directory, MRTC_PERF_SECONDS: 'not-seconds' } });
        assert.equal(failed.status, 1);
        const artifact = JSON.parse(await readFile(resolve(directory, 'comparison.json'), 'utf8'));
        assert.equal(artifact.ok, false);
        assert.equal(artifact.insufficientEvidence, true);
        assert.match(artifact.error, /MRTC_PERF_SECONDS must be/);
    } finally { await rm(parent, { recursive: true, force: true }); }
});
