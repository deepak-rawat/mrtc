import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { access, mkdir, open, readFile, realpath, stat, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { hash, host, integer, provenance, run } from './benchmark-lib.mjs';
import { splitFlags, parseTransportOutput } from './benchmark-transport-lib.mjs';

const execute = promisify(execFile);
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const harness = resolve(root, 'tests/performance/benchmark-transport.c');
const action = process.argv[2];
assert(['build', 'smoke', 'run'].includes(action) && process.argv.length === 3,
    'Usage: node benchmark-transport.mjs build|smoke|run');
assert(['darwin', 'linux'].includes(process.platform), 'Transport benchmark requires Linux or macOS');
assert(process.env.MRTC_BUILD_DIR && process.env.MRTC_TRANSPORT_BINARY,
    'Set MRTC_BUILD_DIR and MRTC_TRANSPORT_BINARY explicitly');
const build = resolve(root, process.env.MRTC_BUILD_DIR);
const binary = resolve(root, process.env.MRTC_TRANSPORT_BINARY);
const metadataPath = `${binary}.json`;
const seconds = integer(process.env, 'MRTC_TRANSPORT_SECONDS', 15, 1, 60);
const warmupSeconds = integer(process.env, 'MRTC_TRANSPORT_WARMUP_SECONDS', 5, 1, 60);
const packetCap = integer(process.env, 'MRTC_TRANSPORT_PACKET_CAP', 2000000, 1024, 8000000);

async function fileIdentity(path) {
    const resolved = await realpath(path);
    return { path: resolved, sha256: hash(await readFile(resolved)), bytes: (await stat(resolved)).size };
}

async function inputs(identity) {
    const source = identity.source;
    const libraries = await Promise.all(['rtc/libmrtc.a', 'common/libmrtc_common.a']
        .map(name => fileIdentity(join(build, name))));
    const sharedLibraries = await Promise.all([
        identity.buildProfile.OPENSSL_SSL_LIBRARY, identity.buildProfile.OPENSSL_CRYPTO_LIBRARY,
    ].map(path => { assert(path, 'Missing OpenSSL library path'); return fileIdentity(path); }));
    const headers = await Promise.all(['rtc/include/rtc/rtc.h', 'rtc/include/rtc/rtc_transport.h',
        'rtc/include/rtc/rtc_listener.h', 'rtc/include/rtc/rtc_worker.h', 'rtc/include/rtc/rtc_rtp.h',
        'common/include/rtc/rtc_common.h'].map(name => fileIdentity(join(source, name))));
    return { harness: await fileIdentity(harness), libraries, sharedLibraries, headers };
}

async function checkedBuild() {
    const identity = await provenance(build);
    assert.equal(identity.buildProfile.CMAKE_BUILD_TYPE, 'Release', 'Transport comparison requires Release libraries');
    for (const name of ['MRTC_ENABLE_ASAN', 'MRTC_ENABLE_UBSAN', 'MRTC_ENABLE_TSAN'])
        assert(identity.buildProfile[name] !== 'ON', 'Do not compare sanitized libraries as Release');
    return identity;
}

if (action === 'build') {
    for (const path of [binary, metadataPath]) {
        let exists = true;
        try { await access(path); } catch (error) { if (error.code === 'ENOENT') exists = false; else throw error; }
        assert(!exists, `Refusing to overwrite ${path}`);
    }
    const identity = await checkedBuild();
    const input = await inputs(identity);
    const compiler = identity.buildProfile.CMAKE_C_COMPILER;
    assert(compiler, 'Missing CMake C compiler');
    const flags = ['-std=c11', ...splitFlags(identity.buildProfile.CMAKE_C_FLAGS || ''),
        ...splitFlags(identity.buildProfile.CMAKE_C_FLAGS_RELEASE || ''), '-Wall', '-Wextra', '-Wpedantic', '-Werror'];
    for (const name of ['MRTC_ENABLE_TWCC', 'MRTC_ENABLE_RATE_CONTROL'])
        if (identity.buildProfile[name] === 'ON') flags.push(`-D${name}`);
    const args = [...flags, '-I', join(identity.source, 'rtc/include'),
        '-I', join(identity.source, 'common/include'), '-I', join(identity.source, 'common/include/rtc'),
        '-I', identity.buildProfile.OPENSSL_INCLUDE_DIR,
        harness, ...input.libraries.map(value => value.path), ...input.sharedLibraries.map(value => value.path),
        '-pthread', '-lm', '-o', binary];
    await mkdir(dirname(binary), { recursive: true });
    try {
        const output = await execute(compiler, args, { timeout: 60000, maxBuffer: 1024 ** 2 });
        process.stdout.write(output.stdout);
        process.stderr.write(output.stderr);
    } catch (error) {
        if (error.stdout) process.stdout.write(error.stdout);
        if (error.stderr) process.stderr.write(error.stderr);
        throw error;
    }
    assert.deepEqual(await inputs(identity), input, 'Transport build inputs changed during compilation');
    assert.deepEqual(await provenance(build), identity, 'Source/build changed during harness compilation');
    const metadata = { schema: 1, provenance: identity, inputs: input,
        compiler, compilerVersion: await run(compiler, ['--version']),
        compileArguments: args, flags, executable: await fileIdentity(binary),
        linkedLibraries: await run(process.platform === 'darwin' ? 'otool' : 'ldd',
            process.platform === 'darwin' ? ['-L', binary] : [binary]) };
    await writeFile(metadataPath, JSON.stringify(metadata, null, 2) + '\n', { flag: 'wx', mode: 0o600 });
    console.log(`Built transport harness: ${binary}\nProvenance: ${metadataPath}`);
} else {
    let report;
    const runId = randomUUID(), startedAt = new Date().toISOString();
    const machine = host();
    let identity, transportBuild;
    const workload = { kind: 'transport', endpoints: 2, workers: 2, ip: '127.0.0.1', socket: 'UDP',
        payloadBytes: 1000, window: 32, seconds, warmupSeconds, packetCap,
        timing: 'same-process CLOCK_MONOTONIC payload timestamp before RTP build/send to verified receive callback',
        latency: 'one-way transport + UDP socket + receive-worker scheduling, NOT capture-to-render media latency',
        throughput: 'achieved exact delivered payload rate with 32-packet windows and 100us driver drain polling, NOT capacity',
        cpu: 'getrusage process CPU delta, both workers plus driver; 100% is one logical core',
        rss: 'getrusage lifetime peak RSS including setup/warmup, NOT steady-state live allocation' };
    const record = async value => {
        const text = JSON.stringify(value) + '\n';
        if (report) await report.write(text);
        else process.stdout.write(text);
    };
    try {
        if (action === 'run') {
            const path = resolve(root, process.env.MRTC_TRANSPORT_REPORT || `build/transport-${Date.now()}.jsonl`);
            await mkdir(dirname(path), { recursive: true });
            report = await open(path, 'wx', 0o600);
            console.log(`Transport report: ${path}`);
        }
        const metadata = JSON.parse(await readFile(metadataPath, 'utf8'));
        identity = await checkedBuild();
        assert.deepEqual(await inputs(identity), metadata.inputs, 'Transport inputs changed; rebuild the harness');
        assert.equal(hash(await readFile(binary)), metadata.executable.sha256, 'Transport executable hash changed');
        assert.deepEqual(identity, metadata.provenance, 'Build/source differs from harness compile provenance');
        transportBuild = { ...metadata, provenance: undefined };
        const measuredProvenance = { ...identity, transportBuild };
        const args = action === 'smoke' ? ['--smoke'] : [String(seconds), String(warmupSeconds), String(packetCap)];
        const output = await execute(binary, args,
            { timeout: (seconds + warmupSeconds + 20) * 1000, maxBuffer: 1024 ** 2 });
        process.stderr.write(output.stderr);
        const measured = parseTransportOutput(output.stdout, { smoke: action === 'smoke', seconds });
        assert.deepEqual(await inputs(identity), metadata.inputs, 'Transport inputs changed during execution');
        assert.deepEqual(await provenance(build), identity, 'Source/build changed during execution');
        await record({ type: action === 'smoke' ? 'transport-smoke-result' : 'benchmark-result', kind: 'transport',
            schema: 1, runId, startedAt, ok: true, host: machine, hostAfter: host(), workload,
            provenance: measuredProvenance, packets: measured.packets, elapsedMs: measured.elapsedSeconds * 1000,
            fingerprintVerified: true, metrics: measured.metrics });
    } catch (error) {
        if (error.stderr) process.stderr.write(error.stderr);
        await record({ type: action === 'smoke' ? 'transport-smoke-result' : 'benchmark-result', kind: 'transport',
            schema: 1, runId, startedAt, ok: false, host: machine, workload,
            provenance: identity && { ...identity, transportBuild },
            error: error.stack || String(error), harnessStdout: error.stdout });
        process.exitCode = 1;
    } finally { await report?.close(); }
}
