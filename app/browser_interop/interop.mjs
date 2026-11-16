import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const build = resolve(process.env.MRTC_BUILD_DIR || '../../build/sfu-check');
const children = [];
let browser;
const fixture = await readFile(resolve(here, 'index.html'));
const http = createServer((request, response) => {
    response.writeHead(200, { 'Content-Type': 'text/html' });
    response.end(fixture);
});
const outputs = new Map();
function run(name, command, args) {
    const process = spawn(command, args, { stdio: ['pipe', 'pipe', 'pipe'] });
    children.push(process);
    outputs.set(name, '');
    for (const stream of [process.stdout, process.stderr])
        stream.on('data', data => outputs.set(name, outputs.get(name) + data));
    process.on('error', error => outputs.set(name, outputs.get(name) + error));
    return process;
}
async function until(predicate, label, timeout = 30000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (await predicate()) return;
        await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw Error(`timeout waiting for ${label}`);
}
async function exerciseChat(prefix, url) {
    const args = ['--server', url, '--meeting', prefix, '--stun', ''];
    const first = run(`${prefix}-a`, resolve(build, 'app/chat/chat'), args);
    const second = run(`${prefix}-b`, resolve(build, 'app/chat/chat'), args);
    const firstOutput = () => outputs.get(`${prefix}-a`);
    const secondOutput = () => outputs.get(`${prefix}-b`);
    await until(() => firstOutput().includes('[joined as') &&
        secondOutput().includes('[joined as'), `${prefix} clients joined`);
    await until(() => {
        assert.equal(first.exitCode, null, `${prefix} first client exited early`);
        if (secondOutput().includes(`${prefix}-check-a`)) return true;
        first.stdin.write(`${prefix}-check-a\n`);
        return false;
    }, `${prefix} first message delivered`);
    await until(() => {
        assert.equal(second.exitCode, null, `${prefix} second client exited early`);
        if (firstOutput().includes(`${prefix}-check-b`)) return true;
        second.stdin.write(`${prefix}-check-b\n`);
        return false;
    }, `${prefix} reply delivered`);
    first.stdin.write(`${prefix}-final-a\nquit\n`);
    second.stdin.write(`${prefix}-final-b\nquit\n`);
    await until(() => first.exitCode !== null && second.exitCode !== null, `${prefix} cleanup`);
    assert.equal(first.exitCode, 0);
    assert.equal(second.exitCode, 0);
}
try {
    await new Promise(resolve => http.listen(0, '127.0.0.1', resolve));
    const port = Number(process.env.MRTC_SIGNALING_PORT || 18995);
    const url = `ws://127.0.0.1:${port}`;
    run('server', resolve(build, 'signaling/signaling_server'), [String(port)]);
    await until(() => outputs.get('server').includes('listening'), 'local SFU start');
    browser = await chromium.launch({
        headless: true,
        timeout: 30000,
        ...(process.env.MRTC_CHROME_PATH ? { executablePath: process.env.MRTC_CHROME_PATH } : {}),
        args: ['--autoplay-policy=no-user-gesture-required', '--no-sandbox',
            '--disable-features=WebRtcHideLocalIpsWithMdns', '--mute-audio']
    });
    const context = await browser.newContext({permissions: ['microphone']});
    const first = await context.newPage();
    await first.goto(`http://127.0.0.1:${http.address().port}`);
    const probe = run('native', resolve(build, 'app/sfu_probe'), [url, 'interop', '3600']);
    await until(() => outputs.get('native').includes('PROBE_JOINED'), 'native join');
    await first.evaluate(({ url }) => startConference(url, 'interop'), { url });
    await until(async () => {
        const stats = await first.evaluate(() => conference.stats());
        assert.deepEqual(stats.errors, []);
        return stats.video > 2 && stats.audio > 0 &&
            stats.messages.includes('browser-datachannel') &&
            stats.messages.includes('native-datachannel');
    }, 'decoded Chromium audio/video and standard SCTP/DCEP exchange');
    const second = await context.newPage();
    await second.goto(`http://127.0.0.1:${http.address().port}`);
    await second.evaluate(({ url }) => startConference(url, 'interop'), { url });
    await until(async () => (await first.evaluate(() => conference.stats())).tiles === 2,
        'second publisher routing');
    const firstId = await first.evaluate(() => conference.id);
    const videoFrames = page => page.evaluate(async id => {
        const pc = conference.pcs.get(id);
        if (!pc) return 0;
        return [...(await pc.getStats()).values()].filter(report =>
            report.type === 'inbound-rtp' && report.kind === 'video')
            .reduce((sum, report) => sum + (report.framesDecoded || 0), 0);
    }, firstId);
    await until(async () => (await videoFrames(second)) > 2, 'forwarded first publisher video');
    await second.evaluate(id => { conference.originalSubscription = conference.pcs.get(id); }, firstId);
    let answers = await first.evaluate(() => conference.publisherAnswers);
    await first.evaluate(() => conference.renegotiate('inactive'));
    await until(async () => first.evaluate(previous => conference.publisherAnswers > previous, answers),
        'publisher inactive answer');
    await new Promise(resolve => setTimeout(resolve, 300));
    const paused = await videoFrames(second);
    await new Promise(resolve => setTimeout(resolve, 300));
    assert.equal(await videoFrames(second), paused);
    answers = await first.evaluate(() => conference.publisherAnswers);
    await first.evaluate(() => conference.renegotiate('sendrecv'));
    await until(async () => first.evaluate(previous => conference.publisherAnswers > previous, answers),
        'publisher resumed answer');
    await until(async () => (await videoFrames(second)) > paused + 2, 'resumed publisher forwarding');
    assert(await second.evaluate(id =>
        conference.pcs.get(id) === conference.originalSubscription, firstId));
    await first.evaluate(() => { conference.originalChannel = conference.dc; });
    answers = await first.evaluate(() => conference.publisherAnswers);
    await first.evaluate(() => conference.renegotiate('sendrecv', true));
    await until(async () => first.evaluate(previous => conference.publisherAnswers > previous, answers),
        'live publisher ICE restart answer');
    assert(await first.evaluate(() => conference.dc === conference.originalChannel &&
        conference.dc.readyState === 'open'));
    await first.evaluate(() => conference.dc.send('restart-datachannel'));
    await until(async () => first.evaluate(() =>
        conference.messages.includes('restart-datachannel')), 'same SCTP channel after ICE restart');
    const secondId = await second.evaluate(() => conference.id);
    await second.evaluate(() => conference.close());
    await until(async () => first.evaluate(id =>
        !conference.pcs.has(id) && !document.getElementById(`peer-${id}`), secondId),
    'departed publisher and stale tile removal');
    await second.reload();
    await second.evaluate(({ url }) => startConference(url, 'interop'), { url });
    await until(async () => (await first.evaluate(() => conference.stats())).tiles === 2,
        'reconnected publisher routing');
    await until(() => outputs.get('native').includes('PROBE_MEDIA_OK'), 'native decoded media');
    await exerciseChat('sfu-chat', url);
    const reservation = createServer();
    await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
    const meshPort = reservation.address().port;
    await new Promise(resolve => reservation.close(resolve));
    run('mesh-server', resolve(build, 'signaling/signaling_server'), [String(meshPort), '--mesh']);
    await until(() => outputs.get('mesh-server').includes('listening'), 'mesh server start');
    await exerciseChat('mesh-chat', `ws://127.0.0.1:${meshPort}`);
    assert.deepEqual((await first.evaluate(() => conference.stats())).errors, []);
    assert.deepEqual((await second.evaluate(() => conference.stats())).errors, []);
    console.log('PASS Chromium decoded SFU audio/video, standard SCTP/DCEP browser/server/native messages, live publisher direction updates/ICE restart, retained subscriptions, join/leave/reconnect, stale tile removal and SFU/mesh CLI chat');
    console.log(JSON.stringify(await first.evaluate(() => conference.stats())));
    await first.evaluate(() => conference.close());
    await second.evaluate(() => conference.close());
    probe.kill('SIGTERM');
} catch (error) {
    console.error(error);
    for (const [name, output] of outputs) console.error(`--- ${name} ---\n${output}`);
    if (browser) {
        for (const page of browser.contexts().flatMap(context => context.pages()))
            console.error(JSON.stringify(await page.evaluate(() => conference?.stats()).catch(String), null, 2));
    }
    process.exitCode = 1;
} finally {
    if (browser) await browser.close();
    await new Promise(resolve => http.close(resolve));
    for (const process of children) process.kill('SIGTERM');
    await new Promise(resolve => setTimeout(resolve, 300));
    for (const process of children)
        if (process.exitCode === null && process.signalCode === null) process.kill('SIGKILL');
}
