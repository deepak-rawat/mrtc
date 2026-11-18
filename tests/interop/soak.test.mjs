import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const script = fileURLToPath(new URL('./soak.mjs', import.meta.url));

test('soak rejects malformed duration before launching media', () => {
    const result = spawnSync(process.execPath, [script], { encoding: 'utf8', timeout: 10000,
        env: { ...process.env, MRTC_SOAK_SECONDS: '30seconds' } });
    assert.equal(result.status, 1);
    assert.match(result.stderr, /MRTC_SOAK_SECONDS must be/);
});

test('missing server produces one failed soak result, never success', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'mrtc-soak-controller-'));
    try {
        const path = join(directory, 'report.jsonl');
        const result = spawnSync(process.execPath, [script], { encoding: 'utf8', timeout: 10000,
            env: { ...process.env, MRTC_BUILD_DIR: join(directory, 'missing-build'),
                MRTC_SOAK_REPORT: path, MRTC_SOAK_SECONDS: '10', MRTC_SOAK_PEERS: '2',
                MRTC_SOAK_ROOMS: '1', MRTC_SOAK_SAMPLE_MS: '1000',
                MRTC_TEST_ARTIFACTS: join(directory, 'evidence') } });
        assert.equal(result.status, 1);
        const rows = (await readFile(path, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
        const results = rows.filter(row => row.type === 'result');
        assert.equal(results.length, 1);
        assert.equal(results[0].ok, false);
        assert.match(results[0].error, /server exited/);
        assert.doesNotMatch(result.stdout, /PASS/);
    } finally {
        await rm(directory, { recursive: true, force: true });
    }
});
