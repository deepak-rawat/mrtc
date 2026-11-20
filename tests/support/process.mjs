import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFile, readdir, stat } from 'node:fs/promises';
import { cpus, hostname, loadavg, release, totalmem } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';

const execute = promisify(execFile);
export const run = async (command, args, cwd) =>
    (await execute(command, args, { cwd, timeout: 10000, maxBuffer: 4 * 1024 ** 2 })).stdout.trim();
export const hash = data => createHash('sha256').update(data).digest('hex');
export const host = () => ({ hostname: hostname(), platform: process.platform, arch: process.arch,
    release: release(), cpu: cpus()[0]?.model, logicalCpus: cpus().length,
    memoryBytes: totalmem(), loadAverage: loadavg(), node: process.version });
export const normalizeLinkedLibraries = text => text.replace(/\s+\(0x[0-9a-f]+\)/gi, '');

export function integer(env, name, fallback, min, max) {
    const text = env[name] ?? String(fallback);
    const value = Number(text);
    assert(/^\d+$/.test(text) && Number.isSafeInteger(value) && value >= min && value <= max,
        `${name} must be ${min}..${max}`);
    return value;
}

export function cpuSeconds(text) {
    const match = /^(?:(\d+)-)?(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)$/.exec(text);
    assert(match && Number(match[3]) < 60 && Number(match[4]) < 60, `Invalid process CPU time: ${text}`);
    return Number(match[1] || 0) * 86400 + Number(match[2] || 0) * 3600 +
        Number(match[3]) * 60 + Number(match[4]);
}

export async function resources(processes) {
    const pids = [...processes.values()].map(child => {
        assert(Number.isSafeInteger(child.pid) && child.pid > 0 &&
            child.exitCode === null && child.signalCode === null, 'Owned process is not running');
        return child.pid;
    });
    const output = await run('ps', ['-o', 'pid=,time=,rss=', '-p', pids.join(',')]);
    const rows = new Map(output.split('\n').map(line => {
        const fields = line.trim().split(/\s+/);
        assert(fields.length === 3 && /^\d+$/.test(fields[0]) && /^\d+$/.test(fields[2]),
            `Invalid process resource row: ${line}`);
        return [Number(fields[0]), { cpuSeconds: cpuSeconds(fields[1]), rssBytes: Number(fields[2]) * 1024 }];
    }));
    return Object.fromEntries([...processes].map(([name, child]) => {
        assert(rows.has(child.pid), `Missing owned PID ${child.pid}`);
        return [name, rows.get(child.pid)];
    }));
}

export async function provenance(build) {
    const cache = await readFile(join(build, 'CMakeCache.txt'), 'utf8');
    const entries = Object.fromEntries(cache.split('\n').filter(line => /^[^/#][^=]*=/.test(line))
        .map(line => { const at = line.indexOf('='); return [line.slice(0, at).split(':')[0], line.slice(at + 1)]; }));
    const source = entries.CMAKE_HOME_DIRECTORY;
    assert(source, 'CMakeCache lacks CMAKE_HOME_DIRECTORY');
    const commit = await run('git', ['rev-parse', 'HEAD'], source);
    const status = await run('git', ['status', '--porcelain', '--untracked-files=all'], source);
    const diff = await run('git', ['diff', 'HEAD', '--binary'], source);
    const compilers = [];
    for (const entry of await readdir(join(build, 'CMakeFiles'), { withFileTypes: true })) {
        if (!entry.isDirectory() || !/^\d+\./.test(entry.name)) continue;
        const text = await readFile(join(build, 'CMakeFiles', entry.name, 'CMakeCCompiler.cmake'), 'utf8');
        compilers.push({ cmakeVersion: entry.name, configuration: text });
    }
    assert(compilers.length, 'Missing generated CMake compiler identity');
    const binaries = {};
    for (const name of ['app/server/mrtc_server', 'app/cli/mrtc_cli']) {
        const path = join(build, name);
        binaries[name] = { path, sha256: hash(await readFile(path)), modified: (await stat(path)).mtime.toISOString(),
            linkedLibraries: normalizeLinkedLibraries(await run(process.platform === 'darwin' ? 'otool' : 'ldd',
                process.platform === 'darwin' ? ['-L', path] : [path])) };
    }
    return { build, source, sourceCommit: commit, sourceDirty: Boolean(status), sourceStatus: status,
        trackedDiffSha256: hash(diff), cacheSha256: hash(cache), compilers, binaries,
        buildProfile: Object.fromEntries(Object.entries(entries).filter(([key]) =>
            /CMAKE_(BUILD_TYPE|C_COMPILER|C_FLAGS|EXE_LINKER_FLAGS)|MRTC_|OPENSSL|OPUS|VPX|SRTP|SCTP|WEBSOCKET|LWS|CJSON|LIBYUV|ZLIB/i.test(key))),
        caveat: 'Source identity is read from the CMake source worktree at run time, not an embedded build-commit attestation. Binary hashes identify the measured executables; rebuild after source changes.' };
}

export function percentile(values, fraction) {
    assert(values.length && values.every(Number.isFinite), 'Missing/nonfinite measurements');
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.ceil(fraction * sorted.length) - 1];
}
