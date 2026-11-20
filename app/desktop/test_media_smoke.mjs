import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

process.env.MRTC_BUILD_DIR ||= 'build';
const { fixture, until, build } = await import('../../tests/e2e/fixture.mjs');
const { NativeClient } = await import('../../tests/e2e/native-client.mjs');
const destination = resolve(build, 'desktop-media-smoke', `${Date.now()}-${process.pid}`);
await mkdir(destination, { recursive: true });
const statsFile = resolve(destination, 'desktop.json');
const room = `desktop-${randomUUID().slice(0, 8)}`;
const f = await fixture('mesh', [], { withBrowser: false });
let desktop, cli;
let report;
try {
    cli = new NativeClient(f, 'desktop-smoke-cli');
    await cli.join(f.url, room, 'CLI smoke', { stun: '' });
    const oldVideo = process.env.SDL_VIDEO_DRIVER;
    const oldAudio = process.env.SDL_AUDIO_DRIVER;
    process.env.SDL_VIDEO_DRIVER = 'dummy';
    process.env.SDL_AUDIO_DRIVER = 'dummy';
    try {
        const executable = process.platform === 'darwin'
            ? 'app/desktop/mrtc_desktop.app/Contents/MacOS/mrtc_desktop'
            : 'app/desktop/mrtc_desktop';
        desktop = f.run('desktop', executable, ['--meeting', room, '--server', f.url,
            '--name', 'Desktop smoke', '--stun', '', '--synthetic', '--tone',
            '--duration', '15', '--stats-file', statsFile]);
    } finally {
        if (oldVideo === undefined) delete process.env.SDL_VIDEO_DRIVER;
        else process.env.SDL_VIDEO_DRIVER = oldVideo;
        if (oldAudio === undefined) delete process.env.SDL_AUDIO_DRIVER;
        else process.env.SDL_AUDIO_DRIVER = oldAudio;
    }
    let firstCLI, firstDesktop;
    async function desktopStats() {
        if (desktop.spawnFailure || desktop.exitCode !== null)
            throw Error(`Desktop exited before media check: ${f.logs.get('desktop')}`);
        try { return JSON.parse(await readFile(statsFile, 'utf8')); }
        catch (error) {
            if (error.code === 'ENOENT' || error instanceof SyntaxError) return null;
            throw error;
        }
    }
    await until(async () => {
        const cliStats = await cli.snapshot();
        const peer = cliStats.peers.find(peer => peer.name === 'Desktop smoke');
        const uiStats = await desktopStats();
        if (peer?.video_frames > 3 && peer.audio_frames > 3 &&
            uiStats?.joined && uiStats.video_frames > 3 && uiStats.audio_frames > 3 &&
            uiStats.video_uploads > 3) {
            firstCLI = peer;
            firstDesktop = uiStats;
            return true;
        }
        return false;
    }, 'bidirectional real desktop/CLI decoded media and SDL texture uploads', 12000);
    await until(async () => {
        const peer = (await cli.snapshot()).peers.find(peer => peer.id === firstCLI.id);
        const uiStats = await desktopStats();
        if (peer?.video_frames > firstCLI.video_frames + 2 &&
            peer.audio_samples > firstCLI.audio_samples &&
            uiStats?.video_frames > firstDesktop.video_frames + 2 &&
            uiStats.audio_samples > firstDesktop.audio_samples &&
            uiStats.video_uploads > firstDesktop.video_uploads + 2) {
            report = { room, commands: { desktop: desktop.spawnargs, cli: cli.child.spawnargs },
                cli: { before: firstCLI, after: peer },
                desktop: { before: firstDesktop, after: uiStats } };
            return true;
        }
        return false;
    }, 'fresh decoded audio/video and fresh desktop rendering after baseline', 5000);
    await until(() => desktop.exitCode !== null || desktop.signalCode !== null,
        'normal bounded desktop exit', 18000);
    const desktopResult = await desktop.done;
    assert.equal(desktopResult.code, 0, f.logs.get('desktop'));
    await cli.quit();
    assert.equal(cli.child.exitCode, 0);
    report.exits = { desktop: { pid: desktop.pid, code: desktopResult.code },
        cli: { pid: cli.child.pid, code: cli.child.exitCode } };
    await writeFile(resolve(destination, 'result.json'), JSON.stringify(report, null, 2));
    console.log(JSON.stringify({ passed: true, destination, ...report }, null, 2));
} catch (error) {
    await f.artifacts(error);
    throw error;
} finally {
    await f.close();
    assert(desktop === undefined || desktop.exitCode !== null || desktop.signalCode !== null);
    assert(cli === undefined || cli.child.exitCode !== null || cli.child.signalCode !== null);
}
