import assert from 'node:assert/strict';
import { fixture, until } from './fixture.mjs';

const cycles = Number(process.env.MRTC_CHURN_CYCLES || 120);
let f;
let deadline;
try {
    f = await fixture('sfu', [], { withBrowser: false });
    f.run('observer', 'app/sfu_probe', [f.url, 'churn', '420']);
    await until(() => f.logs.get('observer').includes('PROBE_JOINED'), 'native observer join');
    const churn = f.run('churn', 'app/sfu_probe', [f.url, 'churn', '10', String(cycles)]);
    const result = await Promise.race([
        churn.done,
        new Promise((resolve, reject) => {
            deadline = setTimeout(() => reject(Error('native SFU churn exceeded 360s')), 360000);
        }),
    ]);
    assert.equal(result.code, 0);
    assert(f.logs.get('churn').includes(`PROBE_CHURN_OK cycles=${cycles}`));
    assert(f.logs.get('observer').includes('PROBE_MEDIA_OK'));
    console.log(`PASS ${cycles} native conference reconnects with real WebSocket/ICE/DTLS/SRTP decode and SCTP`);
} catch (error) {
    console.error(error);
    if (f) for (const [name, text] of f.logs) console.error(`--- ${name} ---\n${text}`);
    process.exitCode = 1;
} finally {
    clearTimeout(deadline);
    await f?.close();
}
