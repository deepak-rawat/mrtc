import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { test } from 'node:test';
import { NativeClient, ControlError } from '../support/native-client.mjs';

function controller(t, script) {
    const child = spawn(process.execPath, ['--input-type=module', '-e', script], { stdio: ['pipe', 'pipe', 'pipe'] });
    child.done = new Promise(resolve => {
        child.once('error', error => resolve({ code: -1, error }));
        child.once('close', (code, signal) => resolve({ code, signal }));
    });
    t.after(async () => {
        if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
        await child.done;
    });
    return new NativeClient(null, 'fake-control-child', { child });
}

const reader = `import { createInterface } from 'node:readline';
const input = createInterface({input:process.stdin});
const send = value => process.stdout.write(JSON.stringify({version:1,generation:1,epoch:1,...value})+'\\n');
`;

test('split/coalesced JSONL correlates concurrent commands and retains structured events', async t => {
    const client = controller(t, `${reader}
const requests=[];
input.on('line', line => {
 const request=JSON.parse(line);
 if(request.command==='quit') {send({event:'result',id:request.id,ok:true});input.close();return;}
 requests.push(request);
 if(requests.length===2) {
  const frame=JSON.stringify({version:1,generation:1,epoch:1,event:'message',peer_id:'p',text:'hello'})+'\\n';
  process.stdout.write(frame.slice(0,13));
  setTimeout(()=>{process.stdout.write(frame.slice(13));for(const r of requests.reverse())
   send({event:'result',id:r.id,ok:true,stats:{command:r.command}});},10);
 }
});`);
    const [a, b] = await Promise.all([client.command('first'), client.command('second')]);
    assert.equal(a.stats.command, 'first');
    assert.equal(b.stats.command, 'second');
    assert.equal((await client.event('message')).text, 'hello');
    await client.quit();
});

test('command failure is structured and does not poison later requests', async t => {
    const client = controller(t, `${reader}
input.on('line',line=>{const r=JSON.parse(line);send({event:'result',id:r.id,ok:r.command!=='bad',error:'unsupported'});
if(r.command==='quit')input.close();});`);
    await assert.rejects(client.command('bad'), error => error instanceof ControlError && error.detail.error === 'unsupported');
    await client.command('good');
    await client.quit();
});

for (const [label, output] of [
    ['non-JSON logs', 'native debug log\n'],
    ['unsupported version', '{"version":2,"event":"message"}\n'],
    ['unknown event', '{"version":1,"event":"unexpected","generation":1,"epoch":1}\n'],
    ['unknown response ID', '{"version":1,"event":"result","id":999,"ok":true}\n'],
    ['truncated EOF', '{"version":1'],
    ['oversized frame', 'x'.repeat(1024 * 1024 + 1)],
]) test(`rejects ${label} and immediately fails event waits`, async t => {
    // Generate the large string in the child rather than exceeding argv limits.
    const expression = output.length > 1024 * 1024 ? `'x'.repeat(1024*1024+1)` : JSON.stringify(output);
    const client = controller(t, `process.stdin.resume();setTimeout(()=>{process.stdout.write(${expression});process.stdout.end();},20);`);
    await assert.rejects(client.event('joined', () => true, 0, 1000));
    await assert.rejects(client.command('stats'));
});

test('process exit rejects both pending command and event wait promptly', async t => {
    const client = controller(t, `process.stdin.once('data',()=>process.exit(7));`);
    const results = await Promise.allSettled([client.command('stats', {}, 2000), client.event('joined', () => true, 0, 2000)]);
    assert(results.every(result => result.status === 'rejected'));
    assert.equal(client.pending.size, 0);
});

test('clean stdout EOF fails even a pending quit without a response', async t => {
    const client = controller(t, `process.stdin.once('data',()=>process.stdout.end());`);
    await assert.rejects(client.command('quit', {}, 2000), /control output ended/);
});

test('command deadline poisons session and rejects other waits', async t => {
    const client = controller(t, 'process.stdin.resume();');
    const results = await Promise.allSettled([client.command('stats', {}, 30), client.event('joined', () => true, 0, 2000)]);
    assert(results.every(result => result.status === 'rejected' && /deadline/.test(result.reason.message)));
    assert.equal(client.pending.size, 0);
});

test('event deadlines clean listeners without poisoning session', async t => {
    const client = controller(t, 'process.stdin.resume();');
    await assert.rejects(client.event('joined', () => true, 0, 25), /event deadline/);
    assert.equal(client.changed.listenerCount('change'), 0);
    assert.equal(client.failure, null);
});

test('only explicitly expected asynchronous errors are tolerated', async t => {
    const client = controller(t, `${reader}
input.on('line',line=>{const r=JSON.parse(line);send({event:'error',text:r.command});send({event:'result',id:r.id,ok:true});});`);
    client.expectedError = event => event.text === 'expected-abort';
    await client.command('expected-abort');
    await assert.rejects(client.command('unrelated-error'), /asynchronous error/);
});

test('event history is bounded and cursor survives pruning', async t => {
    const client = controller(t, `${reader}
input.on('line',line=>{const r=JSON.parse(line);for(let n=0;n<2100;n++)send({event:'message',peer_id:'p',text:String(n)});
send({event:'result',id:r.id,ok:true});});`);
    await client.command('flood');
    assert.equal(client.events.length, 2048);
    assert.equal(client.cursor, 2100);
    assert.equal((await client.event('message', event => event.text === '2099', 2099)).sequence, 2100);
});

test('outstanding command storage is bounded', async t => {
    const client = controller(t, 'process.stdin.resume();');
    const pending = Array.from({ length: 128 }, () => client.command('stats'));
    await assert.rejects(client.command('stats'), /too many outstanding/);
    assert.equal(client.pending.size, 128);
    client.fail(new Error('test cleanup'));
    await Promise.allSettled(pending);
    assert.equal(client.pending.size, 0);
});
