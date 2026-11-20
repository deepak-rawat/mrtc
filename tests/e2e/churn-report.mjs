import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { mkdir, open } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { host, integer, percentile, provenance, resources } from '../support/process.mjs';

const names = ['server', 'observer', 'churn'];
export function churnReportSettings(env, cycles) {
    assert(Number.isSafeInteger(cycles) && cycles > 0, 'MRTC_CHURN_CYCLES must be a positive integer');
    const allowance = name => env[name] === undefined ? null
        : integer(env, name, 0, 0, 4096) * 1024 ** 2;
    const settings = { cycles, timeoutMs: integer(env, 'MRTC_CHURN_TIMEOUT_MS', 360000, 1, 2147483647),
        sampleEvery: integer(env, 'MRTC_CHURN_SAMPLE_CYCLES', 10, 1, 1000),
        warmupCycles: integer(env, 'MRTC_CHURN_WARMUP_CYCLES', Math.min(100, Math.floor(cycles / 10)), 0, cycles - 1),
        serverGrowthBytes: allowance('MRTC_CHURN_SERVER_GROWTH_MIB'),
        clientGrowthBytesPerProcess: allowance('MRTC_CHURN_CLIENT_GROWTH_MIB') };
    const regular = Math.floor(cycles / settings.sampleEvery);
    const firstSteady = Math.max(1, Math.ceil(settings.warmupCycles / settings.sampleEvery));
    const expectedSteady = Math.max(0, regular - firstSteady + 1) + (cycles % settings.sampleEvery ? 1 : 0);
    assert(expectedSteady >= 3, 'Resource report needs at least three post-warmup samples; adjust cycle/sample/warmup options');
    return settings;
}

export class ChurnMeasurements {
    constructor(settings) {
        this.settings = settings;
        this.completedCycles = 0;
        this.samples = 0;
        this.steadySamples = 0;
        this.cycleTime = { minMs: Infinity, maxMs: 0, totalMs: 0 };
        this.steadyRss = Object.fromEntries(names.map(name => [name, []]));
    }

    cycle(cycle, elapsedMs) {
        assert.equal(cycle, this.completedCycles + 1, 'Lifecycle cycle evidence must be consecutive');
        assert(Number.isFinite(elapsedMs) && elapsedMs > 0, 'Missing lifecycle cycle duration');
        this.completedCycles = cycle;
        this.cycleTime.minMs = Math.min(this.cycleTime.minMs, elapsedMs);
        this.cycleTime.maxMs = Math.max(this.cycleTime.maxMs, elapsedMs);
        this.cycleTime.totalMs += elapsedMs;
        this.lastCycleMs = elapsedMs;
    }

    shouldSample(cycle) {
        return cycle % this.settings.sampleEvery === 0 || cycle === this.settings.cycles;
    }

    observe(cycle, elapsedMs, values) {
        assert.equal(cycle, this.completedCycles, 'Sample must follow the completed post-leave cycle');
        assert(this.shouldSample(cycle) && (!this.previous || cycle > this.previous.cycle), 'Unexpected/duplicate sample');
        assert(Number.isFinite(elapsedMs) && elapsedMs > (this.previous?.elapsedMs ?? 0), 'Invalid sample clock');
        const intervalCpu = this.previous ? {} : { status: 'not-applicable-before-first-resource-sample' };
        for (const name of names) {
            const value = values[name];
            assert(value && Number.isFinite(value.cpuSeconds) && value.cpuSeconds >= 0 &&
                Number.isFinite(value.rssBytes) && value.rssBytes > 0, `Missing/invalid owned measurement: ${name}`);
            if (this.previous) {
                const delta = value.cpuSeconds - this.previous.values[name].cpuSeconds;
                assert(delta >= 0, `CPU time moved backwards: ${name}`);
                intervalCpu[name] = { cpuSeconds: delta,
                    cpuPercent: delta / ((elapsedMs - this.previous.elapsedMs) / 1000) * 100 };
            }
        }
        const sample = { type: 'sample', state: 'post-leave/observer-absent', cycle, elapsedMs,
            lastCycleMs: this.lastCycleMs, intervalCycles: cycle - (this.previous?.cycle ?? 0),
            intervalElapsedMs: this.previous ? elapsedMs - this.previous.elapsedMs : null,
            resources: values, intervalCpu, warmup: cycle < this.settings.warmupCycles };
        this.samples++;
        if (!this.first) this.first = { cycle, elapsedMs, values };
        if (!sample.warmup) {
            if (!this.baseline) this.baseline = { cycle, elapsedMs, values };
            this.steadySamples++;
            sample.baselineCycle = this.baseline.cycle;
            sample.rssGrowth = {};
            sample.violations = [];
            for (const name of names) {
                const rss = values[name].rssBytes, baseline = this.baseline.values[name].rssBytes;
                this.steadyRss[name].push(rss);
                const peak = Math.max(...this.steadyRss[name]);
                const allowance = name === 'server' ? this.settings.serverGrowthBytes : this.settings.clientGrowthBytesPerProcess;
                sample.rssGrowth[name] = { deltaBytes: rss - baseline, deltaPercent: (rss / baseline - 1) * 100,
                    maximumIncreaseBytes: Math.max(0, peak - baseline), allowanceBytes: allowance };
                if (allowance !== null && peak - baseline > allowance) sample.violations.push(name);
            }
        }
        this.previous = { cycle, elapsedMs, values };
        this.lastSample = sample;
        return sample;
    }

    summary() {
        const rss = {};
        for (const name of names) {
            const values = this.steadyRss[name];
            if (!values.length) continue;
            const baseline = this.baseline.values[name].rssBytes, last = values.at(-1);
            const median = percentile(values, 0.5), min = Math.min(...values), max = Math.max(...values);
            rss[name] = { baselineBytes: baseline, finalBytes: last, deltaBytes: last - baseline,
                deltaPercent: (last / baseline - 1) * 100, maximumIncreaseBytes: Math.max(0, max - baseline),
                minBytes: min, maxBytes: max, medianBytes: median, rangePercent: (max - min) / median * 100,
                madPercent: percentile(values.map(value => Math.abs(value - median)), 0.5) / median * 100 };
        }
        const cpuFrom = start => {
            const values = {};
            if (this.previous && start && this.previous.elapsedMs > start.elapsedMs) {
                const seconds = (this.previous.elapsedMs - start.elapsedMs) / 1000;
                for (const name of names) {
                    const delta = this.previous.values[name].cpuSeconds - start.values[name].cpuSeconds;
                    values[name] = { cpuSeconds: delta, cpuPercent: delta / seconds * 100 };
                }
            }
            return values;
        };
        const configured = this.settings.serverGrowthBytes !== null || this.settings.clientGrowthBytesPerProcess !== null;
        const complete = this.settings.serverGrowthBytes !== null && this.settings.clientGrowthBytesPerProcess !== null;
        const exceeded = Object.entries(rss).filter(([name, value]) => {
            const limit = name === 'server' ? this.settings.serverGrowthBytes : this.settings.clientGrowthBytesPerProcess;
            return limit !== null && value.maximumIncreaseBytes > limit;
        }).map(([name]) => name);
        return { completedCycles: this.completedCycles, samples: this.samples, postWarmupSamples: this.steadySamples,
            baselineCycle: this.baseline?.cycle ?? null, firstSampleCycle: this.first?.cycle ?? null,
            finalSampleCycle: this.previous?.cycle ?? null, cpu: cpuFrom(this.first),
            postWarmupCpu: cpuFrom(this.baseline),
            cpuIntervalMs: this.previous && this.first ? this.previous.elapsedMs - this.first.elapsedMs : null,
            cycleTime: { minMs: this.completedCycles ? this.cycleTime.minMs : null, maxMs: this.cycleTime.maxMs,
                meanMs: this.completedCycles ? this.cycleTime.totalMs / this.completedCycles : null },
            rss, growthGate: { configured, completeCoverage: complete, exceeded,
                status: exceeded.length ? 'exceeded' : !configured ? 'not-configured'
                    : complete ? 'within-explicit-allowances' : 'partially-configured' },
            caveat: 'Sampled owned-process RSS deltas/ranges, not live allocations or leak-freedom. Peaks between samples and growth inside the explicit allowance can be missed. First-to-last CPU excludes startup/first sampled cycles; 100% is one logical core. ps CPU-time resolution is platform-dependent.' };
    }

    qualify() {
        assert.equal(this.completedCycles, this.settings.cycles, 'Incomplete lifecycle cycles');
        assert.equal(this.samples, Math.ceil(this.settings.cycles / this.settings.sampleEvery), 'Missing resource samples');
        assert.equal(this.previous?.cycle, this.settings.cycles, 'Missing final post-leave resource sample');
        assert(this.steadySamples >= 3, 'Insufficient post-warmup resource evidence (need at least three samples)');
        const summary = this.summary();
        assert.equal(summary.growthGate.exceeded.length, 0, `RSS growth allowance exceeded: ${summary.growthGate.exceeded.join(', ')}`);
        return summary;
    }
}

export async function openChurnReport({ root, build, cycles, env = process.env }) {
    assert(typeof env.MRTC_CHURN_REPORT === 'string' && env.MRTC_CHURN_REPORT.trim(), 'MRTC_CHURN_REPORT must be a nonempty path');
    const path = resolve(root, env.MRTC_CHURN_REPORT);
    await mkdir(dirname(path), { recursive: true });
    const file = await open(path, 'wx', 0o600);
    const runId = randomUUID(), startedAt = new Date().toISOString();
    let identity, tracker, settings, ownedPids, terminal = false, started;
    const write = value => file.writeFile(JSON.stringify({ schema: 1, runId, ...value }) + '\n');
    return {
        path,
        async initialize() {
            assert(['linux', 'darwin'].includes(process.platform), 'Churn resource reporting requires Linux or macOS');
            settings = churnReportSettings(env, cycles);
            tracker = new ChurnMeasurements(settings);
            identity = await provenance(build);
            await write({ type: 'configuration', startedAt, settings, provenance: identity, host: host(),
                state: 'same processes; sampled after leave, peer_left and observer absent',
                cycleTiming: 'join through leave/peer_left/absent; resource-report overhead excluded from cycle duration',
                workload: 'two synthetic VP8/Opus native peers, fresh bidirectional decode and exact SCTP each lifecycle' });
        },
        start() { started = performance.now(); },
        async cycle(cycle, elapsedMs, processes, verifyState) {
            tracker.cycle(cycle, elapsedMs);
            if (!tracker.shouldSample(cycle)) return;
            const stateEvidence = await verifyState();
            const pids = Object.fromEntries([...processes].map(([name, child]) => [name, child.pid]));
            if (ownedPids) assert.deepEqual(pids, ownedPids, 'Owned lifecycle processes changed during qualification');
            else ownedPids = pids;
            const values = await resources(processes);
            const sample = tracker.observe(cycle, performance.now() - started, values);
            await write({ ...sample, stateEvidence, ownedPids, hostLoadAverage: host().loadAverage });
            assert(!sample.violations?.length, `RSS growth allowance exceeded: ${sample.violations?.join(', ')}`);
        },
        async finish(ok, error) {
            assert(!terminal, 'Churn report already has a terminal result');
            let summary = tracker?.summary();
            if (ok) {
                summary = tracker.qualify();
                assert.deepEqual(await provenance(build), identity, 'Source/build changed during lifecycle qualification');
            }
            await write({ type: 'churn-result', ok, startedAt, provenance: identity, settings,
                hostAfter: host(), ownedPids, resourceSummary: summary, error: error && (error.stack || String(error)) });
            terminal = true;
        },
        close() { return file.close(); },
    };
}
