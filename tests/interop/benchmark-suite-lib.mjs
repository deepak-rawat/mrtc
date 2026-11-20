import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createWriteStream } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { compare, host, integer, percentile } from './benchmark-lib.mjs';

export const releaseOptions = [
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_FLAGS=', '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG',
    '-DCMAKE_EXE_LINKER_FLAGS=', '-DMRTC_BUILD_DESKTOP=OFF', '-DMRTC_BUILD_TESTS=OFF',
    '-DMRTC_ENABLE_CLIENT_API=ON', '-DMRTC_ENABLE_ROUTING_API=ON',
    '-DMRTC_ENABLE_TWCC=ON', '-DMRTC_ENABLE_RATE_CONTROL=ON', '-DMRTC_ENABLE_ASAN=OFF',
];

export function suiteSettings(env) {
    return {
        repeats: integer(env, 'MRTC_PERF_REPEATS', 3, 3, 5),
        peers: integer(env, 'MRTC_PERF_PEERS', 8, 2, 8),
        warmupSeconds: integer(env, 'MRTC_PERF_WARMUP_SECONDS', 60, 5, 120),
        seconds: integer(env, 'MRTC_PERF_SECONDS', 120, 60, 300),
        sampleMs: integer(env, 'MRTC_PERF_SAMPLE_MS', 2000, 1000, 10000),
        idleSeconds: integer(env, 'MRTC_PERF_IDLE_SECONDS', 15, 0, 60),
        transportWarmupSeconds: integer(env, 'MRTC_PERF_TRANSPORT_WARMUP_SECONDS', 5, 1, 60),
        transportSeconds: integer(env, 'MRTC_PERF_TRANSPORT_SECONDS', 15, 1, 60),
        transportPacketCap: integer(env, 'MRTC_PERF_TRANSPORT_PACKET_CAP', 4000000, 1024, 8000000),
        buildJobs: integer(env, 'MRTC_PERF_BUILD_JOBS', 4, 1, 8),
        timeoutSeconds: integer(env, 'MRTC_PERF_TIMEOUT_SECONDS', 3600, 300, 7200),
        sharedRunner: env.MRTC_PERF_SHARED_RUNNER === '1' || env.GITHUB_ACTIONS === 'true',
    };
}

export function pairedPlan(repeats) {
    assert(Number.isSafeInteger(repeats) && repeats >= 3 && repeats <= 5, 'Expected 3..5 pairs');
    return Array.from({ length: repeats }, (_, index) =>
        (index % 2 ? ['candidate', 'baseline'] : ['baseline', 'candidate'])
            .map(variant => ({ pair: index + 1, variant }))).flat();
}

export function normalizedProfile(provenance, dependencyHashes, defaults) {
    const profile = provenance.buildProfile;
    assert.equal(profile.CMAKE_BUILD_TYPE, 'Release', 'Expected Release build');
    for (const key of ['MRTC_ENABLE_ASAN', 'MRTC_ENABLE_UBSAN', 'MRTC_ENABLE_TSAN', 'MRTC_BUILD_FUZZERS'])
        assert(profile[key] !== 'ON', `Unsupported instrumentation: ${key}`);
    const keys = Object.keys(profile).filter(key => /^MRTC_ENABLE_/.test(key) &&
        !/-ADVANCED$/.test(key) && profile[key] === 'ON');
    const compiler = provenance.compilers.map(value =>
        [...value.configuration.matchAll(/set\((CMAKE_C_COMPILER_(?:ID|VERSION|ARCHITECTURE_ID)) "([^"]*)"\)/g)]
            .map(([, key, value]) => [key, value]));
    assert(compiler.length && compiler.every(value => value.length >= 2), 'Missing compiler ID/version');
    const versions = Object.fromEntries(Object.entries(profile)
        .filter(([key, value]) => /(?:OPENSSL|OPUS|VPX|SRTP|SCTP|WEBSOCKET|LWS|CJSON).*_VERSION$/i.test(key) && value)
        .sort(([a], [b]) => a.localeCompare(b)));
    return { features: keys.sort(), compiler,
        flags: Object.fromEntries(['CMAKE_C_FLAGS', 'CMAKE_C_FLAGS_RELEASE', 'CMAKE_EXE_LINKER_FLAGS',
            'CMAKE_EXE_LINKER_FLAGS_RELEASE'].map(key => [key, profile[key] || ''])),
        versions, dependencyHashes, configuredMediaDefaults: defaults };
}

export function assertCompatible(baseline, candidate) {
    assert.deepEqual(candidate, baseline,
        'Baseline/candidate compiler, features, dependencies, flags or source-configured media defaults differ');
}

export async function pressureEvidence() {
    const value = { capturedAt: new Date().toISOString(), host: host(), status: 'available' };
    try {
        if (process.platform === 'linux') {
            value.meminfo = await readFile('/proc/meminfo', 'utf8');
            try { value.memoryPressureStall = await readFile('/proc/pressure/memory', 'utf8'); }
            catch (error) { value.memoryPressureStall = { status: 'unknown', reason: String(error) }; }
        } else if (process.platform === 'darwin') {
            const { run } = await import('./benchmark-lib.mjs');
            value.vmStat = await run('/usr/bin/vm_stat', []);
        } else throw Error('Unsupported VM pressure interface');
    } catch (error) { value.status = 'unknown'; value.reason = String(error); }
    return value;
}

// Each bounded command owns a process group, including native children of the Node runner.
export async function runOwned(command, args, { cwd, env, timeoutMs, log, quiet = false }) {
    assert(timeoutMs > 0 && Number.isFinite(timeoutMs), 'Suite deadline exhausted');
    const output = createWriteStream(log, { flags: 'wx', mode: 0o600 });
    await new Promise((resolve, reject) => { output.once('open', resolve); output.once('error', reject); });
    let child, expired = false, interrupted = false, killTimer, tail = '';
    const signal = value => {
        if (!child?.pid) return;
        try { process.kill(-child.pid, value); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    };
    const stop = () => {
        signal('SIGTERM');
        clearTimeout(killTimer);
        killTimer = setTimeout(() => signal('SIGKILL'), 2000);
    };
    const interrupt = () => { interrupted = true; stop(); };
    let timer;
    try {
        const result = await new Promise((resolve, reject) => {
            child = spawn(command, args, { cwd, env, detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
            output.write(JSON.stringify({ command, args, cwd, ownedPid: child.pid,
                startedAt: new Date().toISOString() }) + '\n');
            process.once('SIGINT', interrupt);
            process.once('SIGTERM', interrupt);
            timer = setTimeout(() => {
                expired = true;
                stop();
            }, timeoutMs);
            for (const [stream, destination] of [[child.stdout, process.stdout], [child.stderr, process.stderr]])
                stream.on('data', data => {
                    output.write(data);
                    tail = (tail + data).slice(-16000);
                    if (!quiet) destination.write(data);
                });
            child.once('error', reject);
            child.once('close', (code, signal) => resolve({ code, signal }));
        });
        if (expired || interrupted || result.code !== 0) {
            const error = Error(`${command} ${interrupted ? 'interrupted' : expired ? 'exceeded owned-process deadline'
                : `exited ${result.code}/${result.signal}`}\n${tail}`);
            error.ownedPid = child.pid;
            error.timedOut = expired;
            error.interrupted = interrupted;
            throw error;
        }
        return result;
    } finally {
        clearTimeout(timer);
        clearTimeout(killTimer);
        process.off('SIGINT', interrupt);
        process.off('SIGTERM', interrupt);
        signal('SIGTERM');
        // A failed runner must not strand media processes or contaminate the next pair.
        if (child?.pid) {
            let alive = false;
            try { process.kill(-child.pid, 0); alive = true; } catch (error) { if (error.code !== 'ESRCH') throw error; }
            if (alive) {
                await new Promise(resolve => setTimeout(resolve, 200));
                signal('SIGKILL');
            }
        }
        await new Promise(resolve => output.end(resolve));
    }
}

export function combineEvidence(manifest, entries) {
    const sections = {}, errors = [];
    for (const kind of ['media', 'transport']) {
        const selected = entries.filter(entry => entry.kind === kind);
        const baseline = [], candidate = [], pairs = [];
        for (let pair = 1; pair <= manifest.settings.repeats; pair++) {
            const a = selected.filter(entry => entry.pair === pair && entry.variant === 'baseline');
            const b = selected.filter(entry => entry.pair === pair && entry.variant === 'candidate');
            if (a.length !== 1 || b.length !== 1 || !a[0].result || !b[0].result ||
                a[0].error || b[0].error) {
                errors.push({ kind, pair, error: 'Missing/failed/duplicate paired functional result' });
                continue;
            }
            baseline.push(a[0].result); candidate.push(b[0].result);
            pairs.push({ pair, baseline: a[0], candidate: b[0] });
        }
        try {
            const comparison = compare(baseline, candidate);
            const pairedMetrics = Object.fromEntries(Object.entries(comparison.metrics).map(([key, rule]) => {
                const observations = pairs.map(({ pair, baseline, candidate }) => {
                    const before = baseline.result.metrics[key], after = candidate.result.metrics[key];
                    const deltaPercent = (after / before - 1) * 100;
                    return { pair, baseline: before, candidate: after, deltaPercent,
                        regressed: rule.direction === 'increase'
                            ? after > before * (1 + rule.thresholdPercent / 100)
                            : after < before * (1 - rule.thresholdPercent / 100) };
                });
                const medianDeltaPercent = percentile(observations.map(value => value.deltaPercent), 0.5);
                const adverse = rule.direction === 'increase' ? medianDeltaPercent : -medianDeltaPercent;
                return [key, { observations, medianDeltaPercent, thresholdPercent: rule.thresholdPercent,
                    regressed: adverse > rule.thresholdPercent + 1e-10 }];
            }));
            sections[kind] = { ...comparison, pairedMetrics,
                regression: comparison.regression || Object.values(pairedMetrics).some(value => value.regressed) };
        } catch (error) {
            sections[kind] = { insufficientEvidence: true, error: String(error) };
            errors.push({ kind, error: String(error) });
        }
    }
    for (const entry of entries) if (entry.error) errors.push({
        kind: entry.kind, pair: entry.pair, variant: entry.variant, error: entry.error });
    const regression = Object.values(sections).some(value => value.regression);
    const insufficientEvidence = errors.length > 0 || manifest.settings.sharedRunner ||
        Object.values(sections).some(value => value.insufficientEvidence);
    return { type: 'performance-suite-comparison', schema: 1, ok: errors.length === 0,
        status: errors.length ? 'functional-or-comparability-failure' : regression ? 'regression' :
            insufficientEvidence ? 'insufficient-evidence' : 'no-observed-regression',
        regression, insufficientEvidence, errors, sections, settings: manifest.settings,
        baseline: manifest.variants.baseline, candidate: manifest.variants.candidate,
        compatibility: manifest.compatibility, tooling: manifest.tooling, priorEvidence: manifest.priorEvidence,
        runs: entries, sharedRunnerNoise: manifest.settings.sharedRunner
            ? 'Shared-runner CPU, RSS, throughput and scheduling noise are uncontrolled; evidence remains advisory/inconclusive.'
            : 'Local exclusivity must be coordinated; this runner cannot exclude unrelated host work.',
        limitations: [
            'Historical noisy baseline is retained separately and is not mixed with changed workloads.',
            'Unknown runtime dimensions/bitrate and VM pressure are explicitly recorded, not inferred from decoded counters.',
            'Native latency is application SCTP/controller RTT. Transport one-way timing includes socket and worker scheduling.',
            'No media capture-to-render latency, leak-freedom, long-haul or capacity certification.',
        ] };
}

export const suiteExitCode = value => !value.ok || value.regression ? 1 : value.insufficientEvidence ? 2 : 0;
