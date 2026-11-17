import { Room } from './room';
import './style.css';

function element<T extends Element>(selector: string, type: { new(): T }): T {
  const found = document.querySelector(selector);
  if (!(found instanceof type)) throw Error(`Missing UI control: ${selector}`);
  return found;
}
const name = element('#name', HTMLInputElement);
const meeting = element('#room', HTMLInputElement);
const server = element('#server', HTMLInputElement);
const synthetic = element('#synthetic', HTMLInputElement);
const local = element('#local', HTMLVideoElement);
const remotes = element('#remotes', HTMLDivElement);
const status = element('#status', HTMLParagraphElement);
const error = element('#error', HTMLParagraphElement);
const message = element('#message', HTMLInputElement);
const messages = element('#messages', HTMLOListElement);
const join = element('#join', HTMLButtonElement);
const leave = element('#leave', HTMLButtonElement);
const mic = element('#mic', HTMLButtonElement);
const camera = element('#camera', HTMLButtonElement);
const restart = element('#restart', HTMLButtonElement);
const send = element('#send', HTMLButtonElement);
const tiles = new Map<string, { figure: HTMLElement; video: HTMLVideoElement; caption: HTMLElement }>();
const showError = (reason: unknown): void => { error.textContent = reason instanceof Error ? reason.message : String(reason); };
const room = new Room({
  status: text => { status.textContent = text; },
  error: showError,
  local: stream => { local.srcObject = stream; },
  participant: (id, name) => {
    let tile = tiles.get(id);
    if (!tile) {
      const figure = document.createElement('figure');
      figure.dataset.peerId = id;
      const video = document.createElement('video');
      video.autoplay = true;
      video.playsInline = true;
      const caption = document.createElement('figcaption');
      figure.append(video, caption);
      remotes.append(figure);
      tile = { figure, video, caption };
      tiles.set(id, tile);
    }
    tile.caption.textContent = name;
  },
  remove: id => {
    const tile = tiles.get(id);
    if (tile) { tile.video.srcObject = null; tile.figure.remove(); tiles.delete(id); }
  },
  media: (id, stream) => {
    const tile = tiles.get(id);
    if (!tile) { showError(`Media from unknown participant ${id}`); return; }
    if (tile.video.srcObject === stream) return;
    tile.video.srcObject = stream;
    void tile.video.play().catch(showError);
  },
  message: (id, text) => {
    const item = document.createElement('li');
    item.textContent = `${id === room.id ? 'You' : tiles.get(id)?.caption.textContent ?? id}: ${text}`;
    messages.append(item);
    while (messages.children.length > 256) messages.firstElementChild?.remove();
  },
});
function controls(connected: boolean): void {
  join.disabled = connected;
  for (const button of [leave, mic, camera, restart, send]) button.disabled = !connected;
  for (const input of [name, meeting, server, synthetic]) input.disabled = connected;
}
element('#join-form', HTMLFormElement).addEventListener('submit', event => {
  event.preventDefault();
  error.textContent = '';
  for (const [button, label] of [[mic, 'Microphone'], [camera, 'Camera']] as const) {
    button.setAttribute('aria-pressed', 'true');
    button.textContent = `${label} on`;
  }
  controls(true);
  const defaultURL = new URL('/ws', location.href);
  defaultURL.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  void room.join({ server: server.value || defaultURL.href, meeting: meeting.value,
    name: name.value, synthetic: synthetic.checked }).catch(reason => { showError(reason); controls(false); });
});
leave.addEventListener('click', () => {
  void room.leave().then(() => controls(false)).catch(showError);
});
for (const [button, kind, label] of [[mic, 'audio', 'Microphone'], [camera, 'video', 'Camera']] as const) {
  button.addEventListener('click', () => {
    const enabled = button.getAttribute('aria-pressed') !== 'true';
    button.setAttribute('aria-pressed', String(enabled));
    button.textContent = `${label} ${enabled ? 'on' : 'off'}`;
    room.mute(kind, !enabled);
  });
}
restart.addEventListener('click', () => { void room.renegotiate('sendrecv', true).catch(showError); });
element('#chat-form', HTMLFormElement).addEventListener('submit', event => {
  event.preventDefault();
  try { room.sendChat(message.value); message.value = ''; }
  catch (reason) { showError(reason); }
});
// Diagnostics expose the real controller; the harness still joins and chats through UI controls.
declare global { interface Window { mrtc: Room } }
window.mrtc = room;
window.addEventListener('pagehide', () => { void room.leave(); });
