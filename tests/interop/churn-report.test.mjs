import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { mkdir, readFile, rm } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ChurnMeasurements, churnReportSettings, openChurnReport } from './churn-report.mjs';

const mib = 1024 ** 2;
const values = (cycle, serverRss = 100 * mib) => ({
    server: { cpuSeconds: cycle, rssBytes: serverRss },
    observer: { cpuSeconds: cycle / 2, rssBytes: 30 * mib },
    churn: { cpuSeconds: cycle / 4, rssBytes: 40 * mib },
});
function measured(settings, rss = () => 100 * mib, skip = () => false) {
    const tracker = new ChurnMeasurements(settings);
    for (let cycle = 1; cycle <= settings.cycles; cycle++) {
        tracker.cycle(cycle, 1000);
        if (tracker.shouldSample(cycle) && !skip(cycle))
            tracker.observe(cycle, cycle * 1000, values(cycle, rss(cycle)));
    }
    return tracker;
}

test('resource reporting has explicit sample/warmup/timeout/budget settings and rejects insufficient plans', () => {
    const settings = churnReportSettings({}, 1000);
    assert.equal(settings.sampleEvery, 10);
    assert.equal(settings.warmupCycles, 100);
    assert.equal(settings.timeoutMs, 360000);
    assert.equal(settings.serverGrowthBytes, null);
    assert.equal(churnReportSettings({ MRTC_CHURN_CLIENT_GROWTH_MIB: '0' }, 1000).clientGrowthBytesPerProcess, 0);
    assert.throws(() => churnReportSettings({ MRTC_CHURN_SAMPLE_CYCLES: '10cycles' }, 1000), /must be/);
    assert.throws(() => churnReportSettings({ MRTC_CHURN_WARMUP_CYCLES: '1000' }, 1000), /must be/);
    assert.throws(() => churnReportSettings({ MRTC_CHURN_TIMEOUT_MS: '2147483648' }, 1000), /must be/);
    assert.throws(() => churnReportSettings({}, 10), /three post-warmup/);
});

test('same-state samples exclude warmup growth and retain CPU, RSS ranges and cycle timing', () => {
    const settings = churnReportSettings({ MRTC_CHURN_WARMUP_CYCLES: '20' }, 40);
    const tracker = measured(settings, cycle => cycle === 10 ? 300 * mib : (100 + (cycle - 20) / 10) * mib);
    const result = tracker.qualify();
    assert.equal(result.baselineCycle, 20);
    assert.equal(result.samples, 4);
    assert.equal(result.postWarmupSamples, 3);
    assert.equal(result.rss.server.deltaBytes, 2 * mib);
    assert.equal(result.rss.server.maximumIncreaseBytes, 2 * mib);
    assert.equal(result.rss.server.minBytes, 100 * mib);
    assert.equal(result.rss.server.maxBytes, 102 * mib);
    assert.equal(result.cpu.server.cpuPercent, 100);
    assert.equal(result.postWarmupCpu.observer.cpuPercent, 50);
    assert.equal(result.cycleTime.meanMs, 1000);
    assert.equal(result.growthGate.status, 'not-configured');
    assert.equal(result.growthGate.completeCoverage, false);
    assert.match(result.caveat, /not live allocations or leak-freedom/);
});

test('a transient peak exceeds the allowance even if final RSS shrinks', () => {
    const settings = churnReportSettings({ MRTC_CHURN_WARMUP_CYCLES: '0',
        MRTC_CHURN_SERVER_GROWTH_MIB: '1', MRTC_CHURN_CLIENT_GROWTH_MIB: '0' }, 30);
    const tracker = measured(settings, cycle => (cycle === 10 ? 100 : cycle === 20 ? 102 : 99) * mib);
    const result = tracker.summary();
    assert.equal(result.rss.server.deltaBytes, -mib);
    assert.equal(result.rss.server.maximumIncreaseBytes, 2 * mib);
    assert.equal(result.growthGate.status, 'exceeded');
    assert.deepEqual(result.growthGate.exceeded, ['server']);
    assert.throws(() => tracker.qualify(), /allowance exceeded.*server/);
    const stable = measured(settings).qualify();
    assert.equal(stable.growthGate.status, 'within-explicit-allowances');
    assert.equal(stable.growthGate.completeCoverage, true);
});

test('missing samples, nonfinite RSS and backwards CPU never qualify lifecycle evidence', () => {
    const settings = churnReportSettings({ MRTC_CHURN_WARMUP_CYCLES: '0' }, 30);
    assert.throws(() => measured(settings, undefined, cycle => cycle === 20).qualify(), /Missing resource samples/);
    const tracker = new ChurnMeasurements(settings);
    for (let cycle = 1; cycle <= 10; cycle++) tracker.cycle(cycle, 1000);
    assert.throws(() => tracker.observe(10, 10000, { ...values(10), observer: undefined }), /owned measurement/);
    tracker.observe(10, 10000, values(10));
    for (let cycle = 11; cycle <= 20; cycle++) tracker.cycle(cycle, 1000);
    const invalid = values(20);
    invalid.churn.rssBytes = NaN;
    assert.throws(() => tracker.observe(20, 20000, invalid), /owned measurement/);
    assert.throws(() => tracker.observe(20, 20000, values(5)), /CPU time moved backwards/);
});

test('report writer emits exactly one failed terminal result and refuses existing output', async () => {
    const directory = resolve('build', `churn-report-writer-${randomUUID()}`);
    await mkdir(directory, { recursive: true });
    const path = resolve(directory, 'report.jsonl');
    try {
        const report = await openChurnReport({ root: directory, cycles: 1000, build: resolve(directory, 'missing'),
            env: { MRTC_CHURN_REPORT: path } });
        await report.finish(false, Error('intentional setup failure'));
        await assert.rejects(report.finish(false, Error('duplicate terminal')), /already has a terminal/);
        await report.close();
        await assert.rejects(openChurnReport({ root: directory, cycles: 1000, build: 'missing',
            env: { MRTC_CHURN_REPORT: path } }), { code: 'EEXIST' });
        const rows = (await readFile(path, 'utf8')).trim().split('\n').map(JSON.parse);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].type, 'churn-result');
        assert.equal(rows[0].ok, false);
        assert.match(rows[0].error, /intentional setup failure/);
    } finally { await rm(directory, { recursive: true, force: true }); }
});

test('churn report records setup/configuration failure before any native workload is launched', async () => {
    const directory = resolve('build', `churn-report-errors-${randomUUID()}`);
    await mkdir(directory, { recursive: true });
    const script = fileURLToPath(new URL('./churn.mjs', import.meta.url));
    try {
        for (const [label, extra, error] of [
            ['missing-build', {}, /CMakeCache/],
            ['malformed-sampling', { MRTC_CHURN_SAMPLE_CYCLES: '10x' }, /MRTC_CHURN_SAMPLE_CYCLES must be/],
        ]) {
            const path = resolve(directory, `${label}.jsonl`);
            const child = spawnSync(process.execPath, [script], { encoding: 'utf8', timeout: 10000,
                env: { ...process.env, MRTC_BUILD_DIR: resolve(directory, 'missing'),
                    MRTC_CHURN_CYCLES: '1000', MRTC_CHURN_REPORT: path,
                    MRTC_CHURN_SAMPLE_CYCLES: '10', MRTC_CHURN_WARMUP_CYCLES: '100',
                    MRTC_CHURN_TIMEOUT_MS: '3600000', MRTC_CHURN_SERVER_GROWTH_MIB: '0',
                    MRTC_CHURN_CLIENT_GROWTH_MIB: '0', ...extra } });
            assert.equal(child.status, 1, child.stderr);
            assert.doesNotMatch(child.stdout, /PASS|verified cycles/);
            const rows = (await readFile(path, 'utf8')).trim().split('\n').map(JSON.parse);
            assert.equal(rows.filter(row => row.type === 'churn-result').length, 1);
            assert.equal(rows.at(-1).ok, false);
            assert.match(rows.at(-1).error, error);
        }
    } finally { await rm(directory, { recursive: true, force: true }); }
});
