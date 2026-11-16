import { spawn } from 'node:child_process';
import { resolve } from 'node:path';
import assert from 'node:assert/strict';

const build = resolve(process.env.MRTC_BUILD_DIR || '../../build/sfu-check');
const children = [];
const logs = [];
const cycles = Number(process.env.MRTC_CHURN_CYCLES || 120);
const port = Number(process.env.MRTC_SIGNALING_PORT || 18996);
const url = `ws://127.0.0.1:${port}`;
function run(command, args) {
    const child = spawn(resolve(build, command), args);
    child.log = '';
    child.stdout.on('data', data => child.log += data);
    child.stderr.on('data', data => child.log += data);
    child.done = new Promise((resolve, reject) => {
        child.on('error', reject);
        child.on('exit', (code, signal) => resolve({code, signal}));
    });
    children.push(child);
    logs.push(child);
    return child;
}
async function until(predicate, timeout = 10000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (predicate()) return;
        await new Promise(resolve => setTimeout(resolve, 25));
    }
    throw Error('native SFU fixture readiness timeout');
}
let deadline;
try {
    const server = run('signaling/signaling_server', [String(port)]);
    await until(() => server.log.includes('listening'));
    const observer = run('app/sfu_probe', [url, 'churn', '420']);
    await until(() => observer.log.includes('PROBE_JOINED'));
    const churn = run('app/sfu_probe', [url, 'churn', '10', String(cycles)]);
    const result = await Promise.race([
        churn.done,
        new Promise((resolve, reject) => {
            deadline = setTimeout(() => reject(Error('native SFU churn exceeded 360s')), 360000);
        })
    ]);
    assert.equal(result.code, 0);
    assert(churn.log.includes(`PROBE_CHURN_OK cycles=${cycles}`));
    assert(observer.log.includes('PROBE_MEDIA_OK'));
    console.log(`PASS ${cycles} native conference reconnects with real WebSocket/ICE/DTLS/SRTP audio/video decode and SCTP messages per generation`);
} catch (error) {
    console.error(error);
    for (const child of logs) console.error(child.log);
    process.exitCode = 1;
} finally {
    clearTimeout(deadline);
    for (const child of children) child.kill('SIGTERM');
    await Promise.race([
        Promise.all(children.map(child => child.done)),
        new Promise(resolve => setTimeout(resolve, 3000))
    ]);
    for (const child of children)
        if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
}
