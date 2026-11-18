export interface Capture {
  stream: MediaStream;
  close(): Promise<void>;
}
export async function capture(synthetic: boolean): Promise<Capture> {
  if (!synthetic) {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: { width: 640, height: 360 } });
    return { stream, close: async () => { stream.getTracks().forEach(track => track.stop()); } };
  }
  const canvas = document.createElement('canvas');
  canvas.width = 320;
  canvas.height = 180;
  const paint = canvas.getContext('2d');
  if (!paint) throw Error('Canvas capture is unavailable');
  let frame = 0;
  const draw = (): void => {
    paint.fillStyle = `hsl(${frame++ % 360} 70% 40%)`;
    paint.fillRect(0, 0, canvas.width, canvas.height);
    paint.fillStyle = 'white';
    paint.font = '24px sans-serif';
    paint.fillText(`mrtc ${frame}`, 20, 90);
  };
  draw();
  // Synthetic media must not depend on a working speaker device.
  const options: AudioContextOptions & { sinkId?: { type: 'none' } } = { sampleRate: 48000 };
  if ('setSinkId' in AudioContext.prototype) options.sinkId = { type: 'none' };
  const audio = new AudioContext(options);
  const oscillator = audio.createOscillator();
  const gain = audio.createGain();
  const output = audio.createMediaStreamDestination();
  oscillator.frequency.value = 440;
  gain.gain.value = 0.08;
  oscillator.connect(gain).connect(output);
  oscillator.start();
  const timer = window.setInterval(draw, 50);
  try { await audio.resume(); }
  catch (error) { window.clearInterval(timer); oscillator.stop(); await audio.close(); throw error; }
  const stream = new MediaStream([...canvas.captureStream(15).getVideoTracks(), ...output.stream.getAudioTracks()]);
  return {
    stream,
    close: async () => {
      window.clearInterval(timer);
      stream.getTracks().forEach(track => track.stop());
      oscillator.stop();
      await audio.close();
    },
  };
}
