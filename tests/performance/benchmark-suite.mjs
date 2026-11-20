import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { access, mkdir, readFile, realpath, writeFile } from 'node:fs/promises';
import { basename, dirname, isAbsolute, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { hash, host, parseReport, provenance, run } from './benchmark-lib.mjs';
import { assertCompatible, combineEvidence, normalizedProfile, pairedPlan, pressureEvidence,
    releaseOptions, runOwned, suiteExitCode, suiteSettings, toolingIdentity } from './benchmark-suite-lib.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const action = process.argv[2];
assert(['prepare', 'verify', 'run', 'all', 'compare'].includes(action) && process.argv.length === 3,
    'Usage: node benchmark-suite.mjs prepare|verify|run|all|compare');
assert(['darwin', 'linux'].includes(process.platform), 'Performance suite requires Linux or macOS');
assert(process.env.MRTC_PERF_DIR, 'Set MRTC_PERF_DIR to a new, persistent evidence directory');
const directory = resolve(root, process.env.MRTC_PERF_DIR);
const manifestPath = join(directory, 'manifest.json'), comparisonPath = join(directory, 'comparison.json');
const write = (path, value) => writeFile(path, JSON.stringify(value, null, 2) + '\n',
    { flag: 'wx', mode: 0o600 });
const node = process.execPath, script = name => join(root, 'tests/performance', name);
const within = (parent, child) => {
    const path = relative(parent, child);
    return !path || (!path.startsWith('..') && !isAbsolute(path));
};
let manifest, deadline;
const entries = [];
async function command(command, args, label, env = process.env, timeoutMs = 600000) {
    try {
        return await runOwned(command, args, { cwd: root, env,
            timeoutMs: Math.min(timeoutMs, deadline - Date.now()), log: join(directory, `${label}.log`) });
    } catch (error) {
        if (error.interrupted) deadline = 0;
        throw error;
    }
}

async function configuredDefaults(source) {
    try {
        const header = await readFile(join(source, 'app/common/app_session.h'), 'utf8');
        const body = await readFile(join(source, 'app/common/app_session.c'), 'utf8');
        const get = name => Number(new RegExp(`#define\\s+${name}\\s+(\\d+)`).exec(header)?.[1]) || 'unknown';
        return { width: get('APP_SESSION_VIDEO_WIDTH'), height: get('APP_SESSION_VIDEO_HEIGHT'),
            fps: get('APP_SESSION_VIDEO_FPS'),
            initialCameraBitrateKbps: Number(/APP_SESSION_VIDEO_FPS,\s*(\d+)\)/.exec(body)?.[1]) || 'unknown' };
    } catch (error) { return { status: 'unknown', reason: String(error) }; }
}

async function dependencies(identity) {
    const paths = new Set();
    for (const binary of Object.values(identity.binaries)) {
        for (const line of binary.linkedLibraries.split('\n')) {
            const text = line.includes('=>') ? line.split('=>')[1].trim() : line.trim();
            if (text.startsWith('/') && !text.endsWith(':'))
                paths.add(text.split(/\s+\(/)[0].trim());
        }
    }
    assert(paths.size, 'No linked dependency identity was captured');
    const result = {};
    for (const path of [...paths].sort()) {
        try {
            const actual = await realpath(path);
            result[path] = { path: actual, sha256: hash(await readFile(actual)) };
        } catch (error) {
            assert(process.platform === 'darwin' && path.startsWith('/usr/lib/'),
                `Cannot fingerprint linked dependency ${path}: ${error}`);
            result[path] = { status: 'OS shared-cache image; identified by same host OS release', path };
        }
    }
    return result;
}

async function validateSource(source, expectedRef) {
    const commit = await run('git', ['rev-parse', 'HEAD'], source);
    const status = await run('git', ['status', '--porcelain', '--untracked-files=all'], source);
    assert(!status, `Freeze/commit the source before preparation: ${source}\n${status}`);
    if (expectedRef) {
        const expected = await run('git', ['rev-parse', '--verify', '--end-of-options', `${expectedRef}^{commit}`], source);
        assert.equal(commit, expected, `Unexpected source commit for ${source}`);
    }
    return commit;
}

async function prepare(buildProjects) {
    const settings = suiteSettings(process.env);
    const tooling = await toolingIdentity(root);
    deadline = Date.now() + settings.timeoutSeconds * 1000;
    await mkdir(dirname(directory), { recursive: true });
    await mkdir(directory); // Never reuse a partially completed preparation or old reports.
    const baseline = await realpath(resolve(root, process.env.MRTC_PERF_BASELINE_SOURCE || 'missing-baseline-source'));
    const candidate = await realpath(resolve(root, process.env.MRTC_PERF_CANDIDATE_SOURCE || root));
    assert.notEqual(baseline, candidate, 'Use distinct baseline/candidate source worktrees');
    assert(!within(baseline, directory), 'Evidence/build output must not mutate the baseline worktree');
    const variants = {};
    const commits = {
        baseline: await validateSource(baseline, process.env.MRTC_PERF_BASELINE_REF || '9a9319b'),
        candidate: await validateSource(candidate, process.env.MRTC_PERF_CANDIDATE_REF),
    };
    for (const [variant, source] of Object.entries({ baseline, candidate })) {
        const commit = commits[variant];
        const override = process.env[`MRTC_PERF_${variant.toUpperCase()}_BUILD`];
        const build = resolve(root, override || join(directory, `${variant}-build`));
        assert(!within(baseline, build) && build !== source, 'Use out-of-source build paths outside baseline source');
        if (buildProjects) {
            await command('cmake', ['-S', source, '-B', build, ...releaseOptions], `${variant}-configure`);
            await command('cmake', ['--build', build, '--parallel', String(settings.buildJobs)], `${variant}-build`);
            const log = await readFile(join(directory, `${variant}-build.log`), 'utf8');
            assert(!/\bwarning:/i.test(log), `Compiler warning in ${variant}-build.log; resolve before comparing`);
        } else assert(override, `verify requires MRTC_PERF_${variant.toUpperCase()}_BUILD`);
        const identity = await provenance(build);
        assert.equal(await realpath(identity.source), source, 'CMake source worktree mismatch');
        assert.equal(identity.sourceCommit, commit, 'Source changed while building');
        assert.equal(identity.sourceDirty, false, 'Source changed while building');
        for (const [key, expected] of [['MRTC_BUILD_TESTS', 'OFF'], ['MRTC_BUILD_DESKTOP', 'OFF'],
            ['MRTC_ENABLE_CLIENT_API', 'ON'], ['MRTC_ENABLE_ROUTING_API', 'ON'],
            ['MRTC_ENABLE_TWCC', 'ON'], ['MRTC_ENABLE_RATE_CONTROL', 'ON']])
            assert.equal(identity.buildProfile[key], expected, `Unexpected build option ${key}`);
        const nativeDefaults = await configuredDefaults(source);
        const profile = normalizedProfile(identity, await dependencies(identity), nativeDefaults);
        const transportBinary = join(directory, `transport-${variant}`);
        const env = { ...process.env, MRTC_BUILD_DIR: build, MRTC_TRANSPORT_BINARY: transportBinary };
        await command(node, [script('benchmark-transport.mjs'), 'build'], `${variant}-transport-build`, env);
        await command(node, [script('benchmark-transport.mjs'), 'smoke'], `${variant}-transport-smoke`, env, 30000);
        await command(node, ['--test', script('benchmark-transport.test.mjs')], `${variant}-fingerprint-verification`,
            { ...env, MRTC_TRANSPORT_TEST_BINARY: transportBinary }, 30000);
        variants[variant] = { source, build, transportBinary, provenance: identity, profile,
            configuredMediaDefaults: nativeDefaults,
            runtimeResolution: { status: 'unknown', reason: 'Captured only when exposed by native stats' },
            runtimeBitrate: { status: 'unknown', reason: 'Configured initial bitrate is not actual encoded bitrate' } };
    }
    assertCompatible(variants.baseline.profile, variants.candidate.profile);
    const priorEvidence = [];
    for (const path of (process.env.MRTC_PERF_PRIOR_REPORTS || '').split('\n').filter(Boolean)) {
        const text = await readFile(resolve(root, path), 'utf8');
        const destination = join(directory, `retained-${priorEvidence.length + 1}-${basename(path)}`);
        await writeFile(destination, text, { flag: 'wx', mode: 0o600 });
        priorEvidence.push({ original: resolve(root, path), retained: destination, sha256: hash(text),
            results: parseReport(text, path),
            warning: 'Historical evidence retained unchanged; not mixed into differently configured paired comparisons.' });
    }
    manifest = { schema: 1, type: 'performance-suite-manifest', id: randomUUID(),
        preparedAt: new Date().toISOString(), host: host(), settings, releaseOptions, variants, tooling,
        compatibility: { matched: true, profile: variants.baseline.profile },
        priorEvidence, warning: 'Preparation is not a measurement/pass. Coordinate an exclusive frozen-source window before run.' };
    assert.deepEqual(await toolingIdentity(root), tooling, 'Benchmark/controller sources changed during preparation');
    await write(manifestPath, manifest);
    console.log(`Prepared/verified both variants: ${manifestPath}`);
}

async function loadManifest() {
    manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
    assert(manifest.schema === 1 && manifest.type === 'performance-suite-manifest' &&
        manifest.compatibility?.matched, 'Invalid/incomplete suite manifest');
    const settingsEnv = Object.fromEntries(Object.entries(manifest.settings).map(([key, value]) => [
        `MRTC_PERF_${key.replace(/[A-Z]/g, letter => `_${letter}`).toUpperCase()}`,
        key === 'sharedRunner' ? value ? '1' : '0' : String(value),
    ]));
    assert.deepEqual(suiteSettings(settingsEnv), manifest.settings, 'Invalid/out-of-bounds manifest settings');
    deadline = Date.now() + manifest.settings.timeoutSeconds * 1000;
}

async function verifyFrozen() {
    assert.deepEqual(await toolingIdentity(root), manifest.tooling, 'Benchmark/controller sources changed; prepare a new suite');
    for (const value of Object.values(manifest.variants)) {
        assert.deepEqual(await provenance(value.build), value.provenance, 'Prepared source/build changed; prepare a new suite');
        assert.deepEqual(normalizedProfile(value.provenance, await dependencies(value.provenance),
            await configuredDefaults(value.source)), value.profile, 'Installed dependencies/defaults changed since preparation');
    }
}

async function readEntry(kind, { pair, variant }) {
    const report = join(directory, `${kind}-${variant}-${pair}.jsonl`);
    const text = await readFile(report, 'utf8');
    const results = parseReport(text, report);
    assert.equal(results.length, 1, 'Expected exactly one terminal result per paired report');
    const result = results[0], prepared = manifest.variants[variant];
    assert.equal(result.kind || 'native', kind === 'media' ? 'native' : 'transport', 'Wrong benchmark kind');
    const { transportBuild, ...identity } = result.provenance;
    assert.deepEqual(identity, prepared.provenance, 'Report does not belong to the prepared variant');
    assert.equal(result.workload.seconds, kind === 'media' ? manifest.settings.seconds : manifest.settings.transportSeconds,
        'Report duration differs from suite plan');
    assert.equal(result.workload.warmupSeconds, kind === 'media'
        ? manifest.settings.warmupSeconds : manifest.settings.transportWarmupSeconds, 'Report warmup differs from suite plan');
    if (kind === 'media') {
        assert.equal(result.workload.peers, manifest.settings.peers, 'Report peer count differs from suite plan');
        assert.equal(result.workload.sampleMs, manifest.settings.sampleMs, 'Report sample interval differs from suite plan');
    } else assert.equal(result.workload.packetCap, manifest.settings.transportPacketCap, 'Report packet cap differs from suite plan');
    return { kind, pair, variant, report, sha256: hash(text), result };
}

async function measure() {
    await verifyFrozen();
    const s = manifest.settings, plan = pairedPlan(s.repeats);
    for (const kind of ['media', 'transport']) for (const slot of plan) {
        const name = `${kind}-${slot.variant}-${slot.pair}`, value = manifest.variants[slot.variant];
        const before = await pressureEvidence();
        let entry;
        try {
            assert(Date.now() < deadline, 'Suite measurement deadline exhausted');
            await verifyFrozen();
            const env = { ...process.env, MRTC_BUILD_DIR: value.build,
                MRTC_BENCH_PEERS: String(s.peers), MRTC_BENCH_SECONDS: String(s.seconds),
                MRTC_BENCH_WARMUP_SECONDS: String(s.warmupSeconds), MRTC_BENCH_SAMPLE_MS: String(s.sampleMs),
                MRTC_BENCH_REPORT: join(directory, `${name}.jsonl`),
                MRTC_TEST_ARTIFACTS: join(directory, 'failures', name),
                MRTC_TRANSPORT_BINARY: value.transportBinary,
                MRTC_TRANSPORT_REPORT: join(directory, `${name}.jsonl`),
                MRTC_TRANSPORT_SECONDS: String(s.transportSeconds),
                MRTC_TRANSPORT_WARMUP_SECONDS: String(s.transportWarmupSeconds),
                MRTC_TRANSPORT_PACKET_CAP: String(s.transportPacketCap) };
            const timeout = kind === 'media' ? (s.warmupSeconds + s.seconds + 240) * 1000
                : (s.transportWarmupSeconds + s.transportSeconds + 30) * 1000;
            await command(node, kind === 'media' ? [script('benchmark.mjs')]
                : [script('benchmark-transport.mjs'), 'run'], name, env, timeout);
            entry = await readEntry(kind, slot);
        } catch (error) {
            entry = { kind, ...slot, report: join(directory, `${name}.jsonl`), error: String(error) };
        }
        entry.pressure = { before, after: await pressureEvidence() };
        entries.push(entry);
        await write(join(directory, `${name}-environment.json`), entry.pressure);
        if (Date.now() >= deadline) break;
        await new Promise(resolve => setTimeout(resolve, Math.min(s.idleSeconds * 1000,
            Math.max(0, deadline - Date.now()))));
    }
    await verifyFrozen();
}

try {
    if (['prepare', 'verify', 'all'].includes(action)) await prepare(action !== 'verify');
    else await loadManifest();
    if (['run', 'all', 'compare'].includes(action)) {
        let exists = true;
        try { await access(comparisonPath); } catch (error) {
            if (error.code === 'ENOENT') exists = false; else throw error;
        }
        assert(!exists, 'Refusing to rerun/overwrite a terminal suite; use a new evidence directory');
    }
    if (['run', 'all'].includes(action)) await measure();
    if (action === 'compare') {
        for (const kind of ['media', 'transport']) for (const slot of pairedPlan(manifest.settings.repeats)) {
            try {
                const entry = await readEntry(kind, slot);
                try { entry.pressure = JSON.parse(await readFile(join(directory,
                    `${kind}-${slot.variant}-${slot.pair}-environment.json`), 'utf8')); }
                catch { entry.pressure = { status: 'unknown', reason: 'Environment evidence unavailable' }; }
                entries.push(entry);
            } catch (error) { entries.push({ kind, ...slot, error: String(error) }); }
        }
    }
    if (['run', 'all', 'compare'].includes(action)) {
        const result = combineEvidence(manifest, entries);
        result.comparisonTooling = await toolingIdentity(root);
        await write(comparisonPath, result);
        console.log(`Suite ${result.status}: ${comparisonPath}`);
        process.exitCode = suiteExitCode(result);
    }
} catch (error) {
    const result = { type: 'performance-suite-comparison', schema: 1, ok: false,
        status: 'preparation-or-execution-failure', regression: false, insufficientEvidence: true,
        error: error.stack || String(error), settings: manifest?.settings, runs: entries };
    await mkdir(directory, { recursive: true });
    // Never overwrite an earlier terminal artifact, even after an attempted rerun.
    try { await write(comparisonPath, result); } catch (writeError) { console.error(String(writeError)); }
    console.error(result.error);
    process.exitCode = 1;
}
