import assert from 'node:assert/strict';
import { percentile } from '../support/process.mjs';

export function decodedDelta(before, after, ids) {
    let videoFrames = 0, audioSamples = 0;
    after.forEach((value, i) => {
        assert(value.mode === 'sfu' && value.status === 'active' && value.epoch === before[i].epoch &&
            value.generation === before[i].generation, 'Topology changed during measurement');
        for (const id of ids.filter(id => id !== ids[i])) {
            for (const [label, counter] of [['camera', 'video_frames'], ['mic', 'audio_samples']]) {
                const old = before[i].sources.find(source => source.peer_id === id && source.label === label);
                const now = value.sources.find(source => source.peer_id === id && source.label === label);
                assert(old && now && now.epoch === value.epoch && now.generation === value.generation,
                    `Missing current decoded source ${id}/${label}`);
                const delta = now[counter] - old[counter];
                assert(Number.isFinite(delta) && delta > 0, `No fresh decoded ${id}/${label}`);
                if (label === 'camera') videoFrames += delta;
                else audioSamples += delta;
            }
        }
    });
    return { videoFrames, audioSamples };
}

export function mediaObservations(snapshots) {
    const sources = snapshots.flatMap((snapshot, receiver) => (snapshot.sources || [])
        .filter(source => source.label === 'camera').map(source => {
            const dimensions = Number.isFinite(source.width ?? source.video_width) &&
                Number.isFinite(source.height ?? source.video_height) &&
                (source.width ?? source.video_width) > 0 && (source.height ?? source.video_height) > 0
                ? { width: source.width ?? source.video_width, height: source.height ?? source.video_height } : null;
            const bitrate = source.encoded_bitrate_bps ?? source.measured_encoded_bitrate_bps;
            return { receiver, peerId: source.peer_id, dimensions: dimensions || 'unknown',
                encodedBitrateBps: Number.isFinite(bitrate) ? bitrate : 'unknown',
                configuredTargetBitrateBps: source.target_bitrate_bps ?? 'unknown',
                otherExposedBitrateFields: { video_bitrate_bps: source.video_bitrate_bps ?? 'unknown',
                    bitrate_bps: source.bitrate_bps ?? 'unknown', interpretation: 'unknown; not assumed encoded rate' } };
        }));
    return { sources,
        runtimeResolution: sources.length && sources.every(value => value.dimensions !== 'unknown')
            ? 'exposed-in-stats' : 'unknown',
        runtimeEncodedBitrate: sources.length && sources.every(value => value.encodedBitrateBps !== 'unknown')
            ? 'exposed-in-stats' : 'unknown',
        caveat: 'Unknown fields are not exposed by native stats. Configured target bitrate is not measured encoded bitrate; decoded counters do not establish resolution or bitrate.' };
}

export const metricRules = {
    serverCpuPercent: ['increase', 5], clientsCpuPercent: ['increase', 5],
    totalCpuPercent: ['increase', 5], controllerCpuPercent: ['increase', 5],
    serverPeakRssBytes: ['increase', 10], clientsPeakRssBytes: ['increase', 10],
    totalPeakRssBytes: ['increase', 10],
    decodedVideoFramesPerSecond: ['decrease', 5], decodedAudioSamplesPerSecond: ['decrease', 5],
    applicationSctpRttP50Ms: ['increase', 10], applicationSctpRttP95Ms: ['increase', 10],
    applicationSctpRttMaxMs: ['increase', 10],
};

export const transportMetricRules = {
    transportCpuPercent: ['increase', 5], transportPeakRssBytes: ['increase', 10],
    transportDeliveredPacketsPerSecond: ['decrease', 5], transportPayloadBytesPerSecond: ['decrease', 5],
    transportOneWayP50Ms: ['increase', 10], transportOneWayP95Ms: ['increase', 10],
    transportOneWayMaxMs: ['increase', 10],
};

function rulesFor(metrics, kind = 'native') {
    assert(['native', 'transport'].includes(kind), `Unknown benchmark kind ${kind}`);
    const rules = { ...(kind === 'transport' ? transportMetricRules : metricRules) };
    for (const key of Object.keys(metrics)) {
        if (kind === 'native' && /^process\..+\.cpuPercent$/.test(key)) rules[key] = ['increase', 5];
        else if (kind === 'native' && /^process\..+\.peakRssBytes$/.test(key)) rules[key] = ['increase', 10];
        else assert(key in rules, `Unknown metric ${key}`);
    }
    return rules;
}

export function parseReport(text, label = 'report') {
    let rows;
    try {
        const value = JSON.parse(text);
        rows = Array.isArray(value) ? value : [value];
    } catch {
        try { rows = text.trim().split('\n').map(line => JSON.parse(line)); }
        catch { throw Error(`${label}: invalid JSON/JSONL`); }
    }
    const results = rows.filter(row => row?.type === 'benchmark-result');
    assert(results.length, `${label}: no benchmark results`);
    for (const result of results) {
        assert(result.ok === true, `${label}: failed functional run: ${result.error}`);
        assert(result.schema === 1 && typeof result.runId === 'string' && result.runId &&
            result.provenance && result.host && result.workload,
            `${label}: incomplete provenance/schema`);
        if (result.kind === 'transport') {
            assert(result.fingerprintVerified === true && Number.isSafeInteger(result.packets) &&
                result.packets >= 32 && Number.isFinite(result.elapsedMs) &&
                result.elapsedMs >= result.workload.seconds * 1000,
            `${label}: incomplete verified transport/duration evidence`);
        }
        for (const key of Object.keys(rulesFor(result.metrics ?? {}, result.kind)))
            assert(Number.isFinite(result.metrics?.[key]) && result.metrics[key] > 0,
                `${label}: missing/invalid metric ${key}`);
    }
    return results;
}

function spread(values) {
    const median = percentile(values, 0.5);
    return { median, min: Math.min(...values), max: Math.max(...values),
        spanPercent: (Math.max(...values) - Math.min(...values)) / median * 100,
        madPercent: percentile(values.map(value => Math.abs(value - median)), 0.5) / median * 100 };
}

export function compare(baseline, candidate) {
    assert(baseline.length && candidate.length, 'Both comparison groups are required');
    const runs = [...baseline, ...candidate];
    assert(new Set(runs.map(value => value.runId)).size === runs.length,
        'Duplicate run IDs are not independent repeats');
    const identity = value => JSON.stringify([value.kind ?? 'native', value.workload, value.host.hostname, value.host.platform,
        value.host.arch, value.host.cpu, value.host.logicalCpus, value.host.release, value.host.node]);
    assert(runs.every(value => identity(value) === identity(runs[0])), 'Incompatible workload or host');
    for (const group of [baseline, candidate]) {
        const signature = value => JSON.stringify([value.provenance.sourceCommit,
            value.provenance.sourceDirty, value.provenance.trackedDiffSha256,
            Object.values(value.provenance.binaries).map(binary => binary.sha256),
            value.provenance.transportBuild]);
        assert(group.every(value => signature(value) === signature(group[0])), 'Mixed binary/source identities within a group');
    }
    const insufficient = baseline.length < 3 || candidate.length < 3;
    const rules = rulesFor(runs[0].metrics, runs[0].kind);
    assert(runs.every(value => JSON.stringify(Object.keys(value.metrics).sort()) ===
        JSON.stringify(Object.keys(runs[0].metrics).sort())), 'Incompatible metric sets');
    const metrics = Object.fromEntries(Object.entries(rules).map(([key, [direction, thresholdPercent]]) => {
        const before = spread(baseline.map(value => value.metrics[key]));
        const after = spread(candidate.map(value => value.metrics[key]));
        const deltaPercent = (after.median / before.median - 1) * 100;
        const regressed = direction === 'increase'
            ? after.median > before.median * (1 + thresholdPercent / 100)
            : after.median < before.median * (1 - thresholdPercent / 100);
        const overlappingRanges = before.max >= after.min && after.max >= before.min;
        return [key, { baseline: before, candidate: after, deltaPercent, direction, thresholdPercent,
            regressed, overlappingRanges,
            evidence: insufficient ? 'insufficient-repeats' :
                (overlappingRanges || Math.max(before.madPercent, after.madPercent) > thresholdPercent / 2 ||
                    Math.max(before.spanPercent, after.spanPercent) > thresholdPercent)
                    ? 'noisy-repeat-more' : 'separated-observed-ranges' }];
    }));
    return { schema: 1, type: 'benchmark-comparison',
        baselineProvenance: baseline[0].provenance, candidateProvenance: candidate[0].provenance,
        runs: runs.map(value => ({ runId: value.runId, startedAt: value.startedAt,
            group: baseline.includes(value) ? 'baseline' : 'candidate', elapsedMs: value.elapsedMs,
            loadBefore: value.host.loadAverage, loadAfter: value.hostAfter?.loadAverage })),
        host: runs[0].host, workload: runs[0].workload, baselineRuns: baseline.length,
        candidateRuns: candidate.length, insufficientEvidence: insufficient ||
            Object.values(metrics).some(value => value.evidence === 'noisy-repeat-more'),
        regression: Object.values(metrics).some(value => value.regressed), metrics,
        caveat: 'Observed bounded local runs, not a significance test or capacity/leak certification. Native application SCTP RTT includes the controller. Transport one-way timing includes transport, socket and receive-worker scheduling. Neither measures media capture-to-render latency.' };
}
