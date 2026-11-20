import { spawn } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const cache = resolve(here, 'node_modules/.cache');
await mkdir(resolve(cache, 'tmp'), { recursive: true });
const child = spawn(process.execPath, [resolve(here, 'node_modules/playwright/cli.js'),
    'install', 'chromium', '--only-shell'], {
    stdio: 'inherit',
    env: { ...process.env, TMPDIR: resolve(cache, 'tmp'),
        PLAYWRIGHT_BROWSERS_PATH: process.env.PLAYWRIGHT_BROWSERS_PATH || resolve(cache, 'ms-playwright') },
});
child.on('error', error => { console.error(error); process.exitCode = 1; });
child.on('exit', code => { process.exitCode = code ?? 1; });
