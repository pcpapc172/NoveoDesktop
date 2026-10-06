// Exercise the shipped page against a controlled RTC room, without live credentials.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../Telegram/Resources/noveo/call.html'), 'utf8');
const script = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)][1][1];
const flush = () => new Promise(resolve => setImmediate(resolve));

function fixture(denyMic = false) {
  const events = [], calls = [], timers = new Map(), published = [];
  let room, resolvePublish, nextTimer = 0;
  class Room {
    constructor() {
      room = this;
      this.handlers = new Map();
      this.remoteParticipants = new Map();
      this.localParticipant = {
        setMicrophoneEnabled: async enabled => {
          calls.push(['microphone', enabled]);
          if (denyMic && enabled) throw new Error('Microphone permission denied');
        },
        publishTrack: (track, options) => {
          published.push({ track, options });
          return new Promise(resolve => { resolvePublish = resolve; });
        },
        unpublishTrack: async track => calls.push(['unpublish', track]),
      };
    }
    on(name, callback) { this.handlers.set(name, callback); }
    async connect(url, token) { calls.push(['connect', url, token]); }
    async switchActiveDevice(kind, id) { calls.push(['device', kind, id]); }
    async disconnect() { calls.push(['disconnect']); this.handlers.get('Disconnected')(); }
  }
  const canvasTrack = { stopped: false, stop() { this.stopped = true; } };
  const element = () => ({ remove() {}, muted: false, videoWidth: 1920, videoHeight: 1080 });
  const context = {
    window: { LivekitClient: { Room, RoomEvent: new Proxy({}, { get: (_, key) => key }),
      Track: { Kind: { Audio: 'audio', Video: 'video' }, Source: { ScreenShare: 'screen_share', Camera: 'camera' } } },
      external: { invoke: text => events.push(JSON.parse(text)) } },
    navigator: { mediaDevices: { enumerateDevices: async () => [
      { kind: 'audioinput', label: 'USB mic', deviceId: 'browser-mic-id' },
      { kind: 'audiooutput', label: 'Headphones', deviceId: 'browser-speaker-id' },
    ] } },
    Image: class { set src(_) { this.onload(); } },
    document: {
      body: { appendChild() {} }, getElementById: () => element(),
      createElement: () => ({ ...element(), getContext: () => ({ drawImage() {} }),
        captureStream: () => ({ getVideoTracks: () => [canvasTrack] }),
        toDataURL: () => 'data:image/jpeg;base64,ZmFrZQ==' }),
    },
    setInterval: callback => { timers.set(++nextTimer, callback); return nextTimer; },
    clearInterval: id => timers.delete(id),
  };
  vm.runInNewContext(script, context, { filename: 'noveo/call.html' });
  return { api: context.window.NoveoCall, events, calls, timers, published, canvasTrack,
    room: () => room, resolvePublish: () => resolvePublish() };
}

(async () => {
  const f = fixture();
  assert.equal(f.events[0].e, 'ready');
  await f.api.setDevices('USB mic', 'Headphones');
  await f.api.connect('wss://voice.noveo.ir', 'room-only-token', false);
  assert.deepEqual(f.calls.find(call => call[0] === 'connect'), ['connect', 'wss://voice.noveo.ir', 'room-only-token']);
  assert.ok(f.calls.some(call => call[1] === 'audioinput' && call[2] === 'browser-mic-id'));
  assert.ok(f.calls.some(call => call[1] === 'audiooutput' && call[2] === 'browser-speaker-id'));
  assert.equal(f.events.find(event => event.e === 'connected').muted, false);
  await f.api.setMuted(true);
  assert.equal(f.events.at(-1).muted, true);
  f.api.videoFrame('jpeg', 640, 360, true);
  assert.equal(f.published[0].options.source, 'screen_share');
  await f.api.stopScreen();
  f.resolvePublish(); await flush();
  assert.equal(f.canvasTrack.stopped, true); // Late publish is cleaned up after stop.
  assert.ok(f.calls.some(call => call[0] === 'unpublish'));
  assert.equal(f.events.filter(event => event.e === 'screenPublished').length, 0);
  assert.equal(f.events.filter(event => event.e === 'screenStopped').length, 1);
  const track = { kind: 'video', source: 'screen_share', attach: () => ({
    remove() {}, videoWidth: 1920, videoHeight: 1080,
  }), detach() {} };
  f.room().handlers.get('TrackSubscribed')(track, { source: 'screen_share' }, { identity: 'peer' });
  assert.equal(f.timers.size, 1);
  [...f.timers.values()][0]();
  assert.equal(f.events.at(-1).e, 'videoFrame');
  f.room().handlers.get('TrackUnsubscribed')(track, { source: 'screen_share' });
  assert.equal(f.timers.size, 0);
  await f.api.disconnect();
  assert.equal(f.events.at(-1).e, 'disconnected');

  const denied = fixture(true);
  await denied.api.connect('wss://voice.noveo.ir', 'room-only-token', false);
  const connected = denied.events.find(event => event.e === 'connected');
  assert.equal(connected.muted, true);
  assert.match(connected.micError, /permission denied/);
  assert.equal(denied.events.some(event => event.e === 'error'), false);
  denied.api.videoFrame('jpeg', 640, 360, false);
  assert.equal(denied.published[0].options.source, 'camera');
  denied.resolvePublish(); await flush();
  assert.equal(denied.events.at(-1).screen, false);
  await denied.api.disconnect();
  assert.equal(denied.canvasTrack.stopped, true);
  console.log('PASS bundled media bridge: room connection, microphone denial/mute, native device label mapping, camera/screen routing, remote frames, pending publication cancellation and media cleanup');
})().catch(error => { console.error(error); process.exitCode = 1; });
