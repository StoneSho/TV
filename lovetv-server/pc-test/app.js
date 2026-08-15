// LoveTV PC 测试台：MQTT + 语音（IMA ADPCM 16kHz/30ms，与单片机一致）

const $ = id => document.getElementById(id);

const DEV_SAMPLE_RATE = 16000;
const FRAME_SIZE = 480;          // 30ms @ 16kHz
const ADPCM_FRAME_BYTES = 240;   // 480 samples -> 240 bytes

let mqttClient = null;
let wsVoice = null;
let peerOnline = false;
let audioCtx = null;
let micStream = null;
let processor = null;
let pcmBuf = new Float32Array(0);
let recording = false;
let nextPlayTime = 0;
let sentBytes = 0;
let recvBytes = 0;
let recvPkts = 0;
let mailMode = false;          // 当前录音是否作为留言
let mailFrames = [];           // 留言 ADPCM 帧
let mailStreamActive = false;  // 正在播放服务器下发的留言
let mailStreamDec = { predictor: 0, stepIndex: 0 };
let mailPlayingId = null;

// ---------- IMA ADPCM ----------

const kIndexTable = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];
const kStepTable = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
  34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
  157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
  724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
  3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];

function adpcmEncode(pcmInt16, outBytes, state) {
  for (let i = 0; i < ADPCM_FRAME_BYTES; i++) {
    let byte = 0;
    for (let s = 0; s < 2; s++) {
      const sample = pcmInt16[i * 2 + s];
      let diff = sample - state.predictor;
      let code = 0;
      let step = kStepTable[state.stepIndex];
      if (diff < 0) { code = 8; diff = -diff; }
      if (diff >= step) { code |= 4; diff -= step; }
      step >>= 1;
      if (diff >= step) { code |= 2; diff -= step; }
      step >>= 1;
      if (diff >= step) { code |= 1; }
      const delta = ((2 * (code & 7) + 1) * kStepTable[state.stepIndex]) >> 3;
      let pred = state.predictor + ((code & 8) ? -delta : delta);
      pred = Math.max(-32768, Math.min(32767, pred));
      state.predictor = pred;
      state.stepIndex += kIndexTable[code & 15];
      if (state.stepIndex < 0) state.stepIndex = 0;
      else if (state.stepIndex > 88) state.stepIndex = 88;
      if (s === 0) byte = code << 4; else byte |= code;
    }
    outBytes[i] = byte;
  }
}

function adpcmDecode(inBytes, pcmInt16, state) {
  for (let i = 0; i < inBytes.length; i++) {
    const byte = inBytes[i];
    for (let s = 0; s < 2; s++) {
      const code = s === 0 ? (byte >> 4) & 15 : byte & 15;
      let step = kStepTable[state.stepIndex];
      let diff = step >> 3;
      if (code & 4) diff += step;
      if (code & 2) diff += step >> 1;
      if (code & 1) diff += step >> 2;
      let pred = state.predictor + ((code & 8) ? -diff : diff);
      pred = Math.max(-32768, Math.min(32767, pred));
      state.predictor = pred;
      state.stepIndex += kIndexTable[code & 15];
      if (state.stepIndex < 0) state.stepIndex = 0;
      else if (state.stepIndex > 88) state.stepIndex = 88;
      pcmInt16[i * 2 + s] = pred;
    }
  }
}

const encState = { predictor: 0, stepIndex: 0 };
const decState = { predictor: 0, stepIndex: 0 };

// ---------- 工具 ----------

function now() {
  return new Date().toLocaleTimeString('zh-CN', { hour12: false });
}

function log(cls, text) {
  const box = $('log');
  const line = document.createElement('div');
  line.className = 'log-line ' + cls;
  line.textContent = `[${now()}] ${text}`;
  box.appendChild(line);
  while (box.children.length > 300) box.firstElementChild.remove();
  box.scrollTop = box.scrollHeight;
}

function setStatus(el, text, on) {
  el.textContent = text;
  el.className = 'status ' + (on ? 'on' : 'off');
}

function pushPcm(data) {
  const n = pcmBuf.length + data.length;
  const next = new Float32Array(n);
  next.set(pcmBuf);
  next.set(data, pcmBuf.length);
  pcmBuf = next;
}

function takePcm(n) {
  const out = pcmBuf.slice(0, n);
  pcmBuf = pcmBuf.slice(n);
  return out;
}

function resample(input, inRate, outRate) {
  if (inRate === outRate) return input;
  const ratio = inRate / outRate;
  const out = new Float32Array(Math.ceil(input.length / ratio));
  for (let i = 0; i < out.length; i++) {
    const pos = i * ratio;
    const i0 = Math.floor(pos);
    const i1 = Math.min(i0 + 1, input.length - 1);
    const frac = pos - i0;
    out[i] = input[i0] * (1 - frac) + input[i1] * frac;
  }
  return out;
}

// ---------- MQTT ----------

function connectMqtt() {
  const url = $('mqttUrl').value.trim();
  if (!url) { log('t-sys', '请填写 MQTT 地址'); return; }
  if (mqttClient) { try { mqttClient.end(true); } catch {} }
  mqttClient = mqtt.connect(url, {
    clientId: 'pc-test-' + Math.random().toString(16).slice(2),
    clean: true,
    connectTimeout: 8000,
    reconnectPeriod: 3000,
  });
  mqttClient.on('connect', () => {
    const pcId = $('pcId').value.trim();
    setStatus($('mqttStatus'), 'MQTT 已连接', true);
    log('t-sys', `MQTT 已连接，订阅 lovetv/${pcId}/text`);
    mqttClient.subscribe('lovetv/' + pcId + '/text', { qos: 0 });
  });
  mqttClient.on('message', (topic, payload) => {
    log('t-mqtt', `收到 ${topic}: ${payload.toString()}`);
  });
  mqttClient.on('error', (e) => log('t-sys', 'MQTT 错误: ' + e.message));
  mqttClient.on('close', () => setStatus($('mqttStatus'), 'MQTT 未连接', false));
}

function sendMqtt() {
  if (!mqttClient || !mqttClient.connected) {
    log('t-sys', 'MQTT 未连接，发送失败');
    return;
  }
  const devId = $('devId').value.trim();
  const pcId = $('pcId').value.trim();
  const type = $('msgType').value;
  const note = $('msgNote').value.trim();
  const payload = { from: pcId, type, ts: Math.floor(Date.now() / 1000) };
  if (note) payload.msg = note;
  const topic = 'lovetv/' + devId + '/text';
  mqttClient.publish(topic, JSON.stringify(payload), { qos: 0 });
  log('t-mqtt', `发送 → ${topic}: ${JSON.stringify(payload)}`);
}

// ---------- 语音 ----------

function connectVoice() {
  const base = $('voiceUrl').value.trim().replace(/\/+$/, '');
  const devId = $('devId').value.trim();
  const pcId = $('pcId').value.trim();
  if (!base) { log('t-sys', '请填写语音地址'); return; }
  if (wsVoice) { try { wsVoice.close(); } catch {} }
  const url = `${base}?id=${encodeURIComponent(pcId)}&peer=${encodeURIComponent(devId)}`;
  wsVoice = new WebSocket(url);
  wsVoice.binaryType = 'arraybuffer';
  wsVoice.onopen = () => {
    setStatus($('voiceStatus'), '语音已连接', true);
    $('ptt').disabled = false;
    log('t-sys', `语音已连接：${url}`);
  };
  wsVoice.onclose = (e) => {
    setStatus($('voiceStatus'), '语音未连接', false);
    $('ptt').disabled = true;
    log('t-sys', '语音连接已断开 (' + e.code + ')');
  };
  wsVoice.onerror = () => log('t-sys', '语音连接错误');
  wsVoice.onmessage = (ev) => {
    if (!(ev.data instanceof ArrayBuffer)) {
      let obj = null;
      try { obj = JSON.parse(ev.data); } catch {}
      if (!obj || !obj.type) {
        log('t-voice', '收到未知文本消息（忽略）');
        return;
      }
      if (obj.type === 'peer_status') {
        peerOnline = !!obj.online;
        log('t-sys', `对方设备${peerOnline ? '在线' : '离线'}${peerOnline ? '' : '（语音将自动转留言）'}`);
      } else if (obj.type === 'mail_list') {
        renderMailList(obj.items || []);
      } else if (obj.type === 'mail_notify') {
        log('t-sys', `未读留言 ${obj.count} 条`);
      } else if (obj.type === 'mail_send_ack') {
        log('t-voice', `留言上传开始：${obj.id}（${obj.folder}）`);
      } else if (obj.type === 'mail_send_done') {
        log('t-voice', `留言已保存：${obj.id}`);
        refreshMail();
      } else if (obj.type === 'mail_get_start') {
        mailStreamActive = true;
        mailPlayingId = obj.id || null;
        mailStreamDec.predictor = 0;
        mailStreamDec.stepIndex = 0;
        log('t-voice', `开始播放留言：${obj.id}（${obj.duration}ms）`);
      } else if (obj.type === 'mail_get_end') {
        mailStreamActive = false;
        log('t-voice', `留言播放结束：${obj.id}`);
        if (obj.id) sendVoiceControl({ type: 'mail_read', id: obj.id });
      }
      return;
    }
    if (mailStreamActive) {
      const bytes = new Uint8Array(ev.data);
      if (bytes.length % 2 !== 0 || bytes.length === 0) return;
      const pcm = new Int16Array(bytes.length * 2);
      adpcmDecode(bytes, pcm, mailStreamDec);
      const f32 = new Float32Array(pcm.length);
      for (let i = 0; i < pcm.length; i++) f32[i] = pcm[i] / 32768;
      playFloat(f32);
      $('playState').textContent = `留言播放中...`;
      return;
    }
    recvBytes += ev.data.byteLength;
    recvPkts++;
    $('recvBytes').textContent = recvBytes;
    $('recvPkts').textContent = recvPkts;
    const bytes = new Uint8Array(ev.data);
    if (bytes.length === 0 || bytes.length % 2 !== 0) {
      log('t-voice', `异常包 ${bytes.length} 字节，丢弃`);
      return;
    }
    const pcm = new Int16Array(bytes.length * 2);
    adpcmDecode(bytes, pcm, decState);
    const f32 = new Float32Array(pcm.length);
    for (let i = 0; i < pcm.length; i++) f32[i] = pcm[i] / 32768;
    playFloat(f32);
    $('playState').textContent = `播放中 ${f32.length} 样本 / ${recvPkts} 包`;
  };
}

function sendVoiceControl(obj) {
  if (!wsVoice || wsVoice.readyState !== WebSocket.OPEN) {
    log('t-sys', '语音未连接，无法发送控制消息');
    return false;
  }
  wsVoice.send(JSON.stringify(obj));
  return true;
}

function playFloat(data) {
  if (!audioCtx || data.length === 0) return;
  const buf = audioCtx.createBuffer(1, data.length, DEV_SAMPLE_RATE);
  buf.copyToChannel(data, 0);
  const src = audioCtx.createBufferSource();
  src.buffer = buf;
  src.connect(audioCtx.destination);
  const t = audioCtx.currentTime;
  if (nextPlayTime < t + 0.02 || nextPlayTime - t > 0.5) {
    nextPlayTime = t + 0.02;
  }
  src.start(nextPlayTime);
  nextPlayTime += data.length / DEV_SAMPLE_RATE;
}

async function startMic() {
  if (audioCtx) { if (audioCtx.state === 'suspended') await audioCtx.resume(); return; }
  audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  micStream = await navigator.mediaDevices.getUserMedia({
    audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
  });
  const source = audioCtx.createMediaStreamSource(micStream);
  processor = audioCtx.createScriptProcessor(4096, 1, 1);
  const mute = audioCtx.createGain();
  mute.gain.value = 0;
  source.connect(processor);
  processor.connect(mute);
  mute.connect(audioCtx.destination);
  processor.onaudioprocess = (e) => {
    if (!recording || !wsVoice || wsVoice.readyState !== WebSocket.OPEN) return;
    const ch = e.inputBuffer.getChannelData(0);
    const pcm16 = audioCtx.sampleRate === DEV_SAMPLE_RATE
      ? ch : resample(ch, audioCtx.sampleRate, DEV_SAMPLE_RATE);
    pushPcm(pcm16);
    while (pcmBuf.length >= FRAME_SIZE) {
      const frame = takePcm(FRAME_SIZE);
      const pcmInt = new Int16Array(FRAME_SIZE);
      for (let i = 0; i < FRAME_SIZE; i++) {
        let v = Math.round(frame[i] * 32767);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        pcmInt[i] = v;
      }
      const packet = new Uint8Array(ADPCM_FRAME_BYTES);
      adpcmEncode(pcmInt, packet, encState);
      if (mailMode || !peerOnline) {
        mailFrames.push(packet.slice());
        $('mailRecInfo').textContent = `已录 ${(mailFrames.length * 30 / 1000).toFixed(1)}s（${mailFrames.length * 240} 字节）`;
      } else {
        wsVoice.send(packet);
        sentBytes += packet.length;
        $('sentBytes').textContent = sentBytes;
      }
    }
  };
  log('t-sys', `麦克风就绪，采样率 ${audioCtx.sampleRate}Hz`);
}

async function startPtt() {
  if (!wsVoice || wsVoice.readyState !== WebSocket.OPEN) {
    log('t-sys', '语音未连接，先点“连接全部”');
    return;
  }
  try {
    await startMic();
    recording = true;
    encState.predictor = 0;
    encState.stepIndex = 0;
    $('animBox').classList.add('talking');
    $('ptt').classList.add('active');
    $('ptt').textContent = '正在说话...松开结束';
    log('t-voice', '开始讲话');
  } catch (err) {
    log('t-voice', '启动麦克风失败: ' + err.message);
  }
}

function stopPtt() {
  if (!recording) return;
  recording = false;
  pcmBuf = new Float32Array(0);
  $('animBox').classList.remove('talking');
  $('ptt').classList.remove('active');
  $('ptt').textContent = '按住说话（先连接）';
  if (!peerOnline && mailFrames.length > 0) {
    uploadMailFrames();
    log('t-voice', '对方离线，这段语音已自动转为留言');
  } else {
    log('t-voice', '讲话结束，已发送 ' + sentBytes + ' 字节');
  }
}

// ---------- 语音留言 ----------

function uploadMailFrames() {
  if (mailFrames.length === 0) return;
  const pcId = $('pcId').value.trim();
  const devId = $('devId').value.trim();
  sendVoiceControl({ type: 'mail_send_start', from: pcId, to: devId });
  for (const f of mailFrames) wsVoice.send(f);
  sendVoiceControl({ type: 'mail_send_end' });
  log('t-voice', `留言已上传：${mailFrames.length} 帧 / ${(mailFrames.length * 30 / 1000).toFixed(1)}s`);
  mailFrames = [];
}

async function startMailRec() {
  if (!wsVoice || wsVoice.readyState !== WebSocket.OPEN) {
    log('t-sys', '语音未连接，先点“连接全部”');
    alert('语音未连接，请先点“连接全部”');
    return;
  }
  try {
    await startMic();
    recording = true;
    mailMode = true;
    mailFrames = [];
    encState.predictor = 0;
    encState.stepIndex = 0;
    const btn = $('btnMailRec');
    btn.textContent = '停止并发送留言';
    btn.classList.add('active');
    log('t-voice', '开始录制留言（对方离线也能收）');
  } catch (err) {
    log('t-voice', '启动麦克风失败: ' + err.message);
    alert('启动麦克风失败：' + err.message);
  }
}

function stopMailRec() {
  if (!recording) return;
  recording = false;
  mailMode = false;
  pcmBuf = new Float32Array(0);
  const btn = $('btnMailRec');
  btn.textContent = '录制留言';
  btn.classList.remove('active');
  if (mailFrames.length === 0) {
    log('t-voice', '留言太短，未发送');
    return;
  }
  uploadMailFrames();
}

function refreshMail() {
  sendVoiceControl({ type: 'mail_list' });
}

function playMail(id) {
  sendVoiceControl({ type: 'mail_get', id });
}

function readMail(id) {
  sendVoiceControl({ type: 'mail_read', id });
  setTimeout(refreshMail, 300);
}

function deleteMail(id) {
  sendVoiceControl({ type: 'mail_delete', id });
  setTimeout(refreshMail, 300);
}

function renderMailList(items) {
  const box = $('mailList');
  box.innerHTML = '';
  if (!items || items.length === 0) {
    box.textContent = '暂无留言';
    return;
  }
  items.forEach((it) => {
    const row = document.createElement('div');
    row.className = 'mail-row';
    const info = document.createElement('span');
    const t = new Date(it.ts).toLocaleString('zh-CN', { hour12: false });
    info.textContent = `${it.status === 'new' ? '●' : '○'} ${it.from} ${t}（${(it.duration_ms / 1000).toFixed(1)}s）`;
    const play = document.createElement('button');
    play.textContent = '播放';
    play.onclick = () => playMail(it.id);
    const del = document.createElement('button');
    del.textContent = '删除';
    del.className = 'sec';
    del.onclick = () => deleteMail(it.id);
    row.append(info, play, del);
    box.appendChild(row);
  });
}

function disconnectAll() {
  if (mqttClient) { try { mqttClient.end(true); } catch {} mqttClient = null; }
  if (wsVoice) { try { wsVoice.close(); } catch {} wsVoice = null; }
  if (processor) { try { processor.disconnect(); } catch {} processor = null; }
  if (micStream) { micStream.getTracks().forEach(t => t.stop()); micStream = null; }
  if (audioCtx) { try { audioCtx.close(); } catch {} audioCtx = null; }
  recording = false;
  peerOnline = false;
  mailMode = false;
  mailFrames = [];
  mailStreamActive = false;
  $('animBox').classList.remove('talking');
  setStatus($('mqttStatus'), 'MQTT 未连接', false);
  setStatus($('voiceStatus'), '语音未连接', false);
  $('ptt').disabled = true;
  log('t-sys', '已断开全部连接');
}

// ---------- 事件绑定 ----------

$('connectBtn').onclick = () => { connectMqtt(); connectVoice(); };
$('disconnectBtn').onclick = disconnectAll;
$('sendMqtt').onclick = sendMqtt;
$('btnMailRec').onclick = () => { if (recording) stopMailRec(); else startMailRec(); };
$('btnMailRefresh').onclick = refreshMail;
$('msgType').onchange = () => {
  $('msgNote').placeholder = $('msgType').value === 'custom'
    ? '自定义消息内容（必填）' : '比如：想你了';
};

const ptt = $('ptt');
ptt.addEventListener('mousedown', startPtt);
ptt.addEventListener('mouseup', stopPtt);
ptt.addEventListener('mouseleave', stopPtt);
ptt.addEventListener('touchstart', (e) => { e.preventDefault(); startPtt(); }, { passive: false });
ptt.addEventListener('touchend', (e) => { e.preventDefault(); stopPtt(); }, { passive: false });

// ---------- LoveTV 动画（移植自原版） ----------

let animFrames = [];
let animIdx = 0;
let animTimer = null;

async function initAnim() {
  try {
    const manifest = await (await fetch('assets/manifest.json')).json();
    const sel = $('animSet');
    manifest.forEach((s) => {
      const o = document.createElement('option');
      o.value = s.name;
      o.textContent = `${s.name}（${s.frames.length}帧 ${s.w}×${s.h}）`;
      sel.appendChild(o);
    });

    function loadSet(name) {
      const s = manifest.find(x => x.name === name);
      if (!s) return;
      animFrames = s.frames;
      animFrames.forEach(f => { const im = new Image(); im.src = f; });
      animIdx = 0;
      $('animImg').src = animFrames[0];
      $('animInfo').textContent = `${s.w}×${s.h}，${s.frames.length} 帧，10fps`;
    }

    sel.onchange = () => loadSet(sel.value);
    $('animRand').onclick = () => {
      const s = manifest[Math.floor(Math.random() * manifest.length)];
      sel.value = s.name;
      loadSet(s.name);
    };
    loadSet(manifest.some(s => s.name === 'hutao') ? 'hutao' : manifest[0].name);

    if (animTimer) clearInterval(animTimer);
    animTimer = setInterval(() => {
      if (animFrames.length) {
        animIdx = (animIdx + 1) % animFrames.length;
        $('animImg').src = animFrames[animIdx];
      }
    }, 100);  // 与原版一致的 10fps
  } catch (e) {
    log('t-sys', '加载动画失败: ' + e.message);
  }
}

initAnim();
