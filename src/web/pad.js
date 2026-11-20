'use strict';

// ---------------------------------------------------------------------------
// Renderer: a line-for-line port of src/inkrenderer.cpp, so ink looks the
// same here and on the computer. Change them together.
// ---------------------------------------------------------------------------

const REFERENCE_HEIGHT = 1080;
const LASER_FADE_MS = 1400;

function bodyWidth(s) {
  switch (s.tool) {
    case 'calligraphy': return s.size * 2.6;
    case 'marker': return s.size * 1.8;
    case 'highlighter': return s.size * 5;
    case 'laser': return s.size * 1.3;
    case 'eraser': return 0;
    default: return s.size;
  }
}

function toCanvas(s, W, H) {
  const out = [];
  for (const p of s.pts) {
    const x = p[0] * W, y = p[1] * H, pressure = p.length > 2 ? p[2] : 0.5;
    if (out.length) {
      const last = out[out.length - 1];
      if (Math.hypot(last.x - x, last.y - y) < 0.05) {
        last.p = Math.max(last.p, pressure);
        continue;
      }
    }
    out.push({ x, y, p: pressure });
  }
  return out;
}

function smooth(a, step) {
  if (a.length < 3) return a;
  const out = [];
  for (let i = 0; i < a.length - 1; i++) {
    const p0 = a[Math.max(0, i - 1)], p1 = a[i], p2 = a[i + 1], p3 = a[Math.min(a.length - 1, i + 2)];
    const steps = Math.min(12, Math.max(1, Math.ceil(Math.hypot(p2.x - p1.x, p2.y - p1.y) / step)));
    for (let s = 0; s < steps; s++) {
      const t = s / steps, t2 = t * t, t3 = t2 * t;
      const c = (v0, v1, v2, v3) =>
        0.5 * (2 * v1 + (v2 - v0) * t + (2 * v0 - 5 * v1 + 4 * v2 - v3) * t2 + (3 * v1 - v0 - 3 * v2 + v3) * t3);
      out.push({ x: c(p0.x, p1.x, p2.x, p3.x), y: c(p0.y, p1.y, p2.y, p3.y), p: p1.p + (p2.p - p1.p) * t });
    }
  }
  out.push(a[a.length - 1]);
  return out;
}

// Every piece is wound the same way and filled with the nonzero rule, so the
// pieces merge into one shape with no seams.
function addWound(path, poly) {
  let area = 0;
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length];
    area += a[0] * b[1] - b[0] * a[1];
  }
  if (area < 0) poly = poly.slice().reverse();
  path.moveTo(poly[0][0], poly[0][1]);
  for (let i = 1; i < poly.length; i++) path.lineTo(poly[i][0], poly[i][1]);
  path.closePath();
}

function disc(cx, cy, r) {
  const sides = r < 2 ? 8 : r < 8 ? 16 : 28;
  const poly = [];
  for (let i = 0; i < sides; i++) {
    const a = 2 * Math.PI * i / sides;
    poly.push([cx + Math.cos(a) * r, cy + Math.sin(a) * r]);
  }
  return poly;
}

function addRoundBody(path, samples, radius) {
  for (let i = 0; i < samples.length; i++) {
    const r = radius(i);
    addWound(path, disc(samples[i].x, samples[i].y, r));
    if (i === 0) continue;
    const a = samples[i - 1], b = samples[i];
    const dx = b.x - a.x, dy = b.y - a.y, length = Math.hypot(dx, dy);
    if (length < 1e-6) continue;
    const nx = -dy / length, ny = dx / length, ra = radius(i - 1), rb = r;
    addWound(path, [[a.x + nx * ra, a.y + ny * ra], [b.x + nx * rb, b.y + ny * rb],
                    [b.x - nx * rb, b.y - ny * rb], [a.x - nx * ra, a.y - ny * ra]]);
  }
}

function penOutline(s, W, H) {
  const width = bodyWidth(s) * H / REFERENCE_HEIGHT;
  const samples = smooth(toCanvas(s, W, H), Math.max(1.5, width * 0.6));
  const path = new Path2D();
  addRoundBody(path, samples, i => Math.max(0.4, width * (0.3 + 0.7 * samples[i].p) / 2));
  return path;
}

function calligraphyOutline(s, W, H) {
  const nib = bodyWidth(s) * H / REFERENCE_HEIGHT;
  const samples = smooth(toCanvas(s, W, H), Math.max(1, nib * 0.25));
  const angle = s.angle * Math.PI / 180;
  const ax = Math.cos(angle), ay = -Math.sin(angle);
  const hairline = Math.max(0.45, nib * 0.03);

  const fromStart = [0];
  for (let i = 1; i < samples.length; i++)
    fromStart.push(fromStart[i - 1] + Math.hypot(samples[i].x - samples[i - 1].x, samples[i].y - samples[i - 1].y));
  const total = fromStart[fromStart.length - 1];
  const taper = nib * 1.2;
  const half = i => {
    const ends = Math.min(1, 0.3 + 0.7 * Math.min(fromStart[i], total - fromStart[i]) / taper);
    return nib / 2 * (0.45 + 0.55 * samples[i].p) * ends;
  };

  const path = new Path2D();
  for (let i = 1; i < samples.length; i++) {
    const a = samples[i - 1], b = samples[i], ha = half(i - 1), hb = half(i);
    addWound(path, [[a.x + ax * ha, a.y + ay * ha], [b.x + ax * hb, b.y + ay * hb],
                    [b.x - ax * hb, b.y - ay * hb], [a.x - ax * ha, a.y - ay * ha]]);
  }
  if (samples.length === 1) {
    const c = samples[0], h = half(0), nx = -ay * hairline, ny = ax * hairline;
    addWound(path, [[c.x + ax * h + nx, c.y + ay * h + ny], [c.x - ax * h + nx, c.y - ay * h + ny],
                    [c.x - ax * h - nx, c.y - ay * h - ny], [c.x + ax * h - nx, c.y + ay * h - ny]]);
  }
  addRoundBody(path, samples, () => hairline);
  return path;
}

function linePath(samples) {
  const path = new Path2D();
  if (!samples.length) return path;
  path.moveTo(samples[0].x, samples[0].y);
  for (let i = 1; i < samples.length; i++) path.lineTo(samples[i].x, samples[i].y);
  if (samples.length === 1) path.lineTo(samples[0].x + 0.01, samples[0].y);
  return path;
}

function shapeOutlines(s, W, H) {
  const samples = toCanvas(s, W, H);
  if (!samples.length) return [];
  const a = samples[0], b = samples[samples.length - 1];
  const left = Math.min(a.x, b.x), top = Math.min(a.y, b.y), right = Math.max(a.x, b.x), bottom = Math.max(a.y, b.y);
  if (s.tool === 'rect')
    return [[[left, top], [right, top], [right, bottom], [left, bottom], [left, top]]];
  if (s.tool === 'ellipse') {
    const cx = (left + right) / 2, cy = (top + bottom) / 2, poly = [];
    for (let i = 0; i <= 64; i++) {
      const t = 2 * Math.PI * i / 64;
      poly.push([cx + Math.cos(t) * (right - left) / 2, cy + Math.sin(t) * (bottom - top) / 2]);
    }
    return [poly];
  }
  const scale = H / REFERENCE_HEIGHT, width = bodyWidth(s) * scale;
  const length = Math.hypot(b.x - a.x, b.y - a.y);
  const head = Math.min(length * 0.6, Math.max(width * 4.5, 14 * scale));
  const angle = Math.atan2(b.y - a.y, b.x - a.x);
  const wing = side => [b.x - Math.cos(angle + side) * head, b.y - Math.sin(angle + side) * head];
  return [[[a.x, a.y], [b.x, b.y]], [wing(0.5), [b.x, b.y], wing(-0.5)]];
}

function laserOpacity(s, now) {
  if (!s.endedAt) return 1;
  return Math.min(1, Math.max(0, 1 - (now - s.endedAt) / LASER_FADE_MS));
}

function lighter(hex, factor) {
  // Qt's QColor::lighter: scale the HSV value, spilling into saturation.
  let r = parseInt(hex.slice(1, 3), 16) / 255, g = parseInt(hex.slice(3, 5), 16) / 255, b = parseInt(hex.slice(5, 7), 16) / 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b);
  let h = 0, sat = max ? (max - min) / max : 0, v = max * factor / 100;
  if (max !== min) {
    const d = max - min;
    h = max === r ? (g - b) / d + (g < b ? 6 : 0) : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    h /= 6;
  }
  if (v > 1) { sat = Math.max(0, sat - (v - 1)); v = 1; }
  const i = Math.floor(h * 6), f = h * 6 - i, p = v * (1 - sat), q = v * (1 - f * sat), t = v * (1 - (1 - f) * sat);
  [r, g, b] = [[v, t, p], [q, v, p], [p, v, t], [p, q, v], [t, p, v], [v, p, q]][i % 6];
  return [r, g, b].map(c => Math.round(c * 255));
}

function rgba(hex, alpha) {
  return `rgba(${parseInt(hex.slice(1, 3), 16)},${parseInt(hex.slice(3, 5), 16)},${parseInt(hex.slice(5, 7), 16)},${alpha})`;
}

function paintStroke(ctx, s, W, H, now) {
  if (!s.pts.length || s.tool === 'eraser') return;
  ctx.save();
  const width = bodyWidth(s) * H / REFERENCE_HEIGHT;
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  if (s.tool === 'pen' || s.tool === 'calligraphy') {
    ctx.fillStyle = s.color;
    ctx.fill(s.tool === 'pen' ? penOutline(s, W, H) : calligraphyOutline(s, W, H), 'nonzero');
  } else if (s.tool === 'marker' || s.tool === 'highlighter') {
    ctx.strokeStyle = s.tool === 'highlighter' ? rgba(s.color, 0.38) : s.color;
    ctx.lineWidth = width;
    ctx.stroke(linePath(smooth(toCanvas(s, W, H), Math.max(2, width * 0.5))));
  } else if (s.tool === 'laser') {
    const opacity = laserOpacity(s, now);
    const path = linePath(smooth(toCanvas(s, W, H), 3));
    ctx.strokeStyle = rgba(s.color, 0.28 * opacity);
    ctx.lineWidth = width * 3.2;
    ctx.stroke(path);
    const [r, g, b] = lighter(s.color, 170);
    ctx.strokeStyle = `rgba(${r},${g},${b},${opacity})`;
    ctx.lineWidth = width;
    ctx.stroke(path);
  } else {
    ctx.strokeStyle = s.color;
    ctx.lineWidth = width;
    for (const line of shapeOutlines(s, W, H)) {
      ctx.beginPath();
      ctx.moveTo(line[0][0], line[0][1]);
      for (let i = 1; i < line.length; i++) ctx.lineTo(line[i][0], line[i][1]);
      ctx.stroke();
    }
  }
  ctx.restore();
}

// ---------------------------------------------------------------------------
// The pad
// ---------------------------------------------------------------------------

const PALETTE = ['#ff3b30', '#ff9f0a', '#ffd60a', '#30d158', '#0a84ff', '#bf5af2', '#ffffff', '#1c1c1e'];
const SIZES = [2, 3.5, 6, 10];
const NIB_ANGLES = [15, 30, 45, 0];
const SHAPES = ['arrow', 'rect', 'ellipse'];
const ERASER_SIZE = 16;

const $ = id => document.getElementById(id);
const stage = $('stage'), sheet = $('sheet'), screenImage = $('screen');
const inkCanvas = $('ink'), liveCanvas = $('live');
const inkContext = inkCanvas.getContext('2d'), liveContext = liveCanvas.getContext('2d');

function load(name, fallback) {
  try { const v = localStorage.getItem('omaink.' + name); return v === null ? fallback : JSON.parse(v); }
  catch { return fallback; }
}
function save(name, value) {
  try { localStorage.setItem('omaink.' + name, JSON.stringify(value)); } catch { /* private mode */ }
}

const settings = {
  tool: load('tool', 'pen'),
  color: load('color', PALETTE[0]),
  size: load('size', 1),
  nib: load('nib', 15),
  mirror: load('mirror', true),
};

const key = new URLSearchParams(location.search).get('k') || '';
let socket = null;
let you = '';
let aspect = 16 / 9;
let board = '';
let committed = [];
let mirrorAvailable = true;
let retryDelay = 500;
let sequence = 0;
let current = null;           // the stroke under the pen right now
let pendingPoints = [];       // its points not yet sent
let predicted = [];           // where the pen is about to be, drawn but never sent
const awaiting = new Map();   // finished strokes until the computer echoes them back
const lasers = [];            // own lasers fading out
let eraserAt = null;
let penSeen = false;
const touches = new Map();
let gesture = null;
let inkDirty = true;
let lastFrameUrl = null;

// --- toolbar ---------------------------------------------------------------

function buildToolbar() {
  const colors = $('colors');
  PALETTE.forEach((color, i) => {
    const b = document.createElement('button');
    b.className = 'swatch';
    b.style.background = color;
    b.title = `Color ${i + 1}`;
    b.style.width = b.style.height = '24px';
    b.addEventListener('click', () => { settings.color = color; save('color', color); refreshToolbar(); });
    b.dataset.color = color;
    colors.appendChild(b);
  });
  const sizes = $('sizes');
  SIZES.forEach((size, i) => {
    const b = document.createElement('button');
    b.className = 'size';
    b.title = `Size ${i + 1}`;
    const dot = document.createElement('i');
    dot.style.width = dot.style.height = `${4 + size * 1.3}px`;
    b.appendChild(dot);
    b.addEventListener('click', () => { settings.size = i; save('size', i); refreshToolbar(); });
    sizes.appendChild(b);
  });
  for (const b of document.querySelectorAll('[data-tool]'))
    b.addEventListener('click', () => { settings.tool = b.dataset.tool; save('tool', settings.tool); refreshToolbar(); });
  $('nib').addEventListener('click', () => {
    settings.nib = NIB_ANGLES[(NIB_ANGLES.indexOf(settings.nib) + 1) % NIB_ANGLES.length];
    save('nib', settings.nib);
    refreshToolbar();
  });
  $('undo').addEventListener('click', () => send({ t: 'undo' }));
  $('redo').addEventListener('click', () => send({ t: 'redo' }));
  $('clear').addEventListener('click', () => send({ t: 'clear' }));
  $('board').addEventListener('click', () => send({ t: 'board' }));
  $('mirror').addEventListener('click', () => {
    settings.mirror = !settings.mirror;
    save('mirror', settings.mirror);
    send({ t: 'mirror', on: settings.mirror });
    refreshToolbar();
  });
}

function refreshToolbar() {
  for (const b of document.querySelectorAll('[data-tool]')) b.classList.toggle('on', b.dataset.tool === settings.tool);
  for (const b of document.querySelectorAll('.swatch')) b.classList.toggle('on', b.dataset.color === settings.color);
  document.querySelectorAll('.size').forEach((b, i) => b.classList.toggle('on', i === settings.size));
  $('nib').textContent = `${settings.nib}°`;
  $('nib').parentElement.hidden = settings.tool !== 'calligraphy';
  $('mirror').classList.toggle('on', settings.mirror && mirrorAvailable);
  $('mirror').disabled = !mirrorAvailable;
  $('board').classList.toggle('on', board !== '');
  screenImage.hidden = !settings.mirror || !mirrorAvailable || board !== '';
  sheet.className = board ? `board-${board}` : '';
}

let noticeTimer = 0;
function notice(text, sticky) {
  const n = $('notice');
  n.textContent = text;
  n.hidden = !text;
  clearTimeout(noticeTimer);
  if (text && !sticky) noticeTimer = setTimeout(() => { n.hidden = true; }, 3500);
}

// --- layout ----------------------------------------------------------------

function layout() {
  const box = stage.getBoundingClientRect();
  let width = box.width, height = width / aspect;
  if (height > box.height) { height = box.height; width = height * aspect; }
  sheet.style.width = `${Math.floor(width)}px`;
  sheet.style.height = `${Math.floor(height)}px`;
  const ratio = window.devicePixelRatio || 1;
  for (const canvas of [inkCanvas, liveCanvas]) {
    canvas.width = Math.round(Math.floor(width) * ratio);
    canvas.height = Math.round(Math.floor(height) * ratio);
  }
  inkDirty = true;
}

// --- connection ------------------------------------------------------------

function connect() {
  if (!key) {
    notice('This address is missing its key. Run "omaink pad" on the computer and open the address it shows.', true);
    return;
  }
  socket = new WebSocket(`ws://${location.host}/ws?k=${encodeURIComponent(key)}`);
  socket.binaryType = 'blob';
  socket.onopen = () => {
    retryDelay = 500;
    $('status').classList.add('live');
    notice('');
    send({ t: 'mirror', on: settings.mirror });
  };
  socket.onclose = () => {
    $('status').classList.remove('live');
    if (current) current = null;
    notice('Reconnecting to the computer…', true);
    setTimeout(connect, retryDelay);
    retryDelay = Math.min(retryDelay * 2, 5000);
  };
  socket.onmessage = event => {
    if (typeof event.data !== 'string') { showFrame(event.data); return; }
    receive(JSON.parse(event.data));
  };
}

function send(message) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message));
}

function receive(m) {
  switch (m.t) {
    case 'state':
      you = m.you;
      committed = m.strokes;
      board = m.board || '';
      mirrorAvailable = m.mirror;
      if (m.aspect && Math.abs(m.aspect - aspect) > 1e-3) { aspect = m.aspect; layout(); }
      if (m.mirrorError) notice(m.mirrorError);
      for (const s of committed) awaiting.delete(s.id);
      inkDirty = true;
      refreshToolbar();
      break;
    case 'add':
      committed.push(m.stroke);
      awaiting.delete(m.stroke.id);
      inkDirty = true;
      break;
    case 'remove': {
      const gone = new Set(m.ids);
      committed = committed.filter(s => !gone.has(s.id));
      inkDirty = true;
      break;
    }
    case 'board':
      board = m.board || '';
      refreshToolbar();
      inkDirty = true;
      break;
    case 'aspect':
      aspect = m.aspect;
      layout();
      break;
    case 'mirror-error':
      notice(m.message);
      break;
    case 'error':
      notice(m.message);
      break;
  }
}

function showFrame(blob) {
  if (!settings.mirror) return;
  const url = URL.createObjectURL(blob);
  screenImage.onload = () => { if (lastFrameUrl) URL.revokeObjectURL(lastFrameUrl); lastFrameUrl = url; };
  screenImage.src = url;
}

// --- drawing ---------------------------------------------------------------

function pointFrom(event) {
  const box = sheet.getBoundingClientRect();
  const pressure = event.pointerType === 'pen' ? Math.max(0.05, event.pressure || 0) : 0.75;
  return [(event.clientX - box.left) / box.width, (event.clientY - box.top) / box.height, pressure];
}

function startStroke(event) {
  const id = String(++sequence);
  const tool = settings.tool;
  current = {
    id, pointerId: event.pointerId, tool, color: settings.color,
    size: tool === 'eraser' ? ERASER_SIZE : SIZES[settings.size], angle: settings.nib, pts: [pointFrom(event)],
  };
  send({ t: 'begin', stroke: { id, tool, color: current.color, size: current.size, angle: current.angle, pts: current.pts } });
  if (tool === 'eraser') eraserAt = current.pts[0];
}

function extendStroke(event) {
  const events = event.getCoalescedEvents ? event.getCoalescedEvents() : [event];
  for (const e of events.length ? events : [event]) {
    const point = pointFrom(e);
    if (SHAPES.includes(current.tool)) current.pts = [current.pts[0], point];
    else current.pts.push(point);
    pendingPoints.push(point);
  }
  predicted = event.getPredictedEvents ? event.getPredictedEvents().map(pointFrom) : [];
  if (current.tool === 'eraser') eraserAt = current.pts[current.pts.length - 1];
}

function flush() {
  if (current && pendingPoints.length) {
    send({ t: 'move', id: current.id, pts: pendingPoints });
    pendingPoints = [];
  }
}

function finishStroke(cancelled) {
  if (!current) return;
  flush();
  send({ t: cancelled ? 'cancel' : 'end', id: current.id });
  if (!cancelled && current.tool === 'laser') {
    current.endedAt = Date.now();
    lasers.push(current);
  } else if (!cancelled && current.tool !== 'eraser') {
    current.sentAt = Date.now();
    awaiting.set(you + current.id, current);
  }
  current = null;
  predicted = [];
  eraserAt = null;
}

stage.addEventListener('pointerdown', event => {
  if (event.pointerType === 'pen') penSeen = true;
  if (event.pointerType === 'touch') {
    touches.set(event.pointerId, { x: event.clientX, y: event.clientY });
    if (touches.size >= 2) {
      // A second finger means a gesture, not a stroke.
      if (current && current.pointerId !== undefined && touches.has(current.pointerId)) finishStroke(true);
      gesture = gesture || { fingers: 0, start: Date.now(), moved: false };
      gesture.fingers = Math.max(gesture.fingers, touches.size);
      return;
    }
    // Once the Pencil has been used, fingers only make gestures, so a
    // resting palm never draws.
    if (penSeen) return;
  }
  if (current) return;
  event.preventDefault();
  stage.setPointerCapture(event.pointerId);
  startStroke(event);
});

stage.addEventListener('pointermove', event => {
  if (event.pointerType === 'touch' && touches.has(event.pointerId)) {
    const t = touches.get(event.pointerId);
    if (gesture && Math.hypot(event.clientX - t.x, event.clientY - t.y) > 24) gesture.moved = true;
  }
  if (current && event.pointerId === current.pointerId) extendStroke(event);
});

function pointerEnd(event) {
  if (event.pointerType === 'touch') {
    touches.delete(event.pointerId);
    if (gesture && touches.size === 0) {
      if (!gesture.moved && Date.now() - gesture.start < 400) {
        send({ t: gesture.fingers >= 3 ? 'redo' : 'undo' });
        notice(gesture.fingers >= 3 ? 'Redo' : 'Undo');
      }
      gesture = null;
    }
  }
  if (current && event.pointerId === current.pointerId) {
    if (event.type === 'pointerup') extendStroke(event);
    finishStroke(event.type === 'pointercancel');
  }
}
stage.addEventListener('pointerup', pointerEnd);
stage.addEventListener('pointercancel', pointerEnd);

// Safari scrolls, zooms and selects on touch unless told not to.
document.addEventListener('touchmove', e => { if (stage.contains(e.target)) e.preventDefault(); }, { passive: false });
document.addEventListener('gesturestart', e => e.preventDefault());
document.addEventListener('dblclick', e => e.preventDefault());

// --- painting --------------------------------------------------------------

function frame() {
  flush();
  const now = Date.now();
  const ratio = window.devicePixelRatio || 1;
  const W = inkCanvas.width / ratio, H = inkCanvas.height / ratio;

  if (inkDirty) {
    inkContext.setTransform(ratio, 0, 0, ratio, 0, 0);
    inkContext.clearRect(0, 0, W, H);
    for (const s of committed) paintStroke(inkContext, s, W, H, now);
    inkDirty = false;
  }

  liveContext.setTransform(ratio, 0, 0, ratio, 0, 0);
  liveContext.clearRect(0, 0, W, H);
  for (const [id, s] of awaiting) {
    // If the computer never echoes a stroke back, it didn't take it.
    if (now - s.sentAt > 4000) awaiting.delete(id);
    else paintStroke(liveContext, s, W, H, now);
  }
  for (let i = lasers.length - 1; i >= 0; i--) {
    if (laserOpacity(lasers[i], now) <= 0) lasers.splice(i, 1);
    else paintStroke(liveContext, lasers[i], W, H, now);
  }
  if (current && current.tool !== 'eraser') {
    const shown = predicted.length && !SHAPES.includes(current.tool)
      ? { ...current, pts: current.pts.concat(predicted) } : current;
    paintStroke(liveContext, shown, W, H, now);
  }
  if (eraserAt) {
    const r = ERASER_SIZE * H / REFERENCE_HEIGHT;
    liveContext.strokeStyle = 'rgba(255,255,255,0.85)';
    liveContext.lineWidth = 1.5;
    liveContext.beginPath();
    liveContext.arc(eraserAt[0] * W, eraserAt[1] * H, r, 0, Math.PI * 2);
    liveContext.stroke();
  }
  requestAnimationFrame(frame);
}

buildToolbar();
refreshToolbar();
layout();
window.addEventListener('resize', layout);
connect();
requestAnimationFrame(frame);
