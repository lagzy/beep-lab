/* Minimal Node harness: run the inline studio script with a DOM shim and
   assert on the passive-only / colour / frequency features. */
const fs = require('fs');
const vm = require('vm');

/* ---------- DOM shim ---------- */
function makeCtx(){
  const noop = () => {};
  return {
    fillStyle:'', strokeStyle:'', lineWidth:1, font:'', textAlign:'', textBaseline:'', globalAlpha:1,
    fillRect:noop, clearRect:noop, strokeRect:noop, fillText:noop, strokeText:noop,
    beginPath:noop, moveTo:noop, lineTo:noop, closePath:noop, fill:noop, stroke:noop,
    save:noop, restore:noop, setTransform:noop, transform:noop, translate:noop, scale:noop,
    measureText:() => ({ width: 10 }),
    createLinearGradient:() => ({ addColorStop: noop }),
  };
}
function makeEl(tag){
  const listeners = {};
  const el = {
    tagName:(tag||'div').toUpperCase(), children:[], style:{}, dataset:{},
    _text:'', _html:'', value:'', checked:false, hidden:false, selectedIndex:0,
    options:[], files:[], accept:'', clientWidth:900, clientHeight:507, scrollLeft:0,
    disabled:false, maxLength:0, type:'', title:'', min:0, max:0, step:1,
    classList:{ _s:new Set(), add(c){this._s.add(c);}, remove(c){this._s.delete(c);},
      toggle(c,f){ if(f===undefined) f=!this._s.has(c); f?this._s.add(c):this._s.delete(c); return f; },
      contains(c){ return this._s.has(c); } },
    appendChild(c){ this.children.push(c); c.parentNode=this; return c; },
    removeChild(c){ const i=this.children.indexOf(c); if(i>=0)this.children.splice(i,1); return c; },
    remove(){ if(this.parentNode) this.parentNode.removeChild(this); },
    addEventListener(t,fn){ (listeners[t]=listeners[t]||[]).push(fn); },
    removeEventListener(){},
    querySelector(){ return null; },
    querySelectorAll(){ return []; },
    getBoundingClientRect(){ return { left:0, top:0, right:900, bottom:507, width:900, height:507 }; },
    getContext(){ return (this._ctx = this._ctx || makeCtx()); },
    scrollIntoView(){}, click(){}, focus(){}, select(){},
    get textContent(){ return this._text; }, set textContent(v){ this._text=String(v); },
    get innerHTML(){ return this._html; }, set innerHTML(v){ this._html=String(v); this.children=[]; },
  };
  return el;
}
const registry = {};
function byId(id){
  if (!registry[id]){
    const el = makeEl('div');
    el.id = id;
    if (id === 'posLcd' || id === 'lenLcd') el.firstElementChild = makeEl('b');
    if (id === 'steps') el.options = [16,32,48,64,96,128].map(v => ({ value:String(v) }));
    if (id === 'grid' || id === 'labels') el.tagName = 'CANVAS';
    registry[id] = el;
  }
  return registry[id];
}
const store = new Map();
const globalObj = {
  console, setTimeout, clearTimeout, setInterval, clearInterval, Math, Date, JSON, Object, Array,
  String, Number, Boolean, isFinite, Int8Array, Uint8Array, Int16Array, Uint16Array, Float32Array, DataView, ArrayBuffer,
  Blob: function(){}, URL:{ createObjectURL:()=> 'blob:x', revokeObjectURL(){} },
  document: {
    getElementById: byId,
    createElement: makeEl,
    body: makeEl('body'),
    activeElement: null,
    title: '',
    addEventListener(){}, querySelector(){ return null; }, querySelectorAll(){ return []; },
  },
  localStorage:{ getItem:k=> (store.has(k)?store.get(k):null), setItem:(k,v)=>store.set(k,String(v)), removeItem:k=>store.delete(k) },
  navigator:{},
  requestAnimationFrame: () => 0,
  addEventListener(){}, removeEventListener(){}, dispatchEvent(){},
  confirm: () => true,
  alert(){},
  Event: function(t){ this.type=t; },
  FileReader: function(){ this.readAsText = () => {}; },
  lamejs: undefined,
};
globalObj.window = globalObj;
globalObj.globalThis = globalObj;
vm.createContext(globalObj);
globalObj.window.devicePixelRatio = 1;

/* ---------- load + inject tests ---------- */
const html = fs.readFileSync('index.html', 'utf8');
const si = html.lastIndexOf('<script>');
const js = html.slice(si + 8, html.indexOf('</script>', si));

const TEST = `
;(function(){
  const out = [];
  const ok = (n,c,x) => out.push((c?'PASS ':'FAIL ')+n+(x!==undefined?' :: '+x:''));
  try{
    ok('no S.mode', typeof S.mode === 'undefined');
    // build two stacked same-pitch notes + one normal note
    activeNotes = [];
    S.steps = 32; S.bpm = 150; S.gate = .85; S.duty = .5;
    const a = { id: nextNoteId++, step: 4, len: 1, midi: 60, vol: 1, freq: 0, color: nextPaletteColor() };
    const b = { id: nextNoteId++, step: 4, len: 6, midi: 60, vol: 1, freq: 0, color: nextPaletteColor() };
    activeNotes.push(a, b);
    ok('colours differ', a.color !== b.color, a.color+'/'+b.color);

    // resize right snaps to whole steps
    dragState = { type:'resizeR', noteId:a.id, startStep:a.step, startLen:a.len, startMidi:a.midi, startVol:1, origX:100, origY:0 };
    applyDrag(100 + 2.4*cellW, 0, false);
    ok('resizeR snaps integer len', a.len === 3, a.len);
    // resize left snaps + keeps right edge
    dragState = { type:'resizeL', noteId:a.id, startStep:a.step, startLen:a.len, startMidi:a.midi, startVol:1, origX:100, origY:0 };
    const rightBefore = a.step + a.len;
    applyDrag(100 - 1.6*cellW, 0, false);
    ok('resizeL keeps right edge', Math.abs((a.step + a.len) - rightBefore) < 1e-9 && Number.isInteger(a.step), a.step+'/'+a.len);
    dragState = null;

    // grip detection leaves a body on a 1-step note (cellW forced small)
    cellW = 11;
    const one = { id: nextNoteId++, step: 8, len: 1, midi: 62, vol: 1, freq: 0, color: 0 };
    activeNotes.push(one);
    const bodyHit = noteAt(8*cellW + 5, RULERH + (HIGH-62)*ROWH + 2);
    ok('short note has a body zone', bodyHit && bodyHit.note.id === one.id && bodyHit.handle === 'body', bodyHit && bodyHit.handle);
    const rightHit = noteAt(8*cellW + 10, RULERH + (HIGH-62)*ROWH + 2);
    ok('short note right grip', rightHit && rightHit.handle === 'resizeR', rightHit && rightHit.handle);

    // custom frequency drives renderAudio
    cellW = 22;
    activeNotes = [{ id: nextNoteId++, step: 0, len: 4, midi: 60, vol: 1, freq: 1000, color: 2 }];
    const sr = 8000; const d = renderAudio(sr, 1);
    let zc = 0, prev = d[Math.floor(0.001*sr)];
    for (let i = Math.floor(0.001*sr)+1; i < Math.floor(0.5*sr); i++){ const v = d[i]; if (prev < 0 && v >= 0) zc++; prev = v; }
    // ~1000 Hz gated over ~0.34 s -> roughly 340 rising edges
    ok('custom freq ~1kHz', zc > 300 && zc < 380, zc);

    ok('noteFreq override', Math.abs(noteFreq(activeNotes[0]) - 1000) < 1e-9, noteFreq(activeNotes[0]));

    // codegen: passive only, frequency written
    const ino = makeIno();
    ok('ino has note table', ino.indexOf('N_STEP[NOTE_COUNT]') > -1);
    ok('ino no ACTIVE', ino.toUpperCase().indexOf('ACTIVE') === -1);
    ok('ino writes custom Hz', ino.indexOf('1000') > -1);
    const parsed = parseIno(ino);
    ok('parseIno passive rows', parsed.activeNotes && parsed.activeNotes.length === 1, parsed.activeNotes && parsed.activeNotes.length);
    ok('parseIno keeps freq', parsed.activeNotes && Math.abs(noteFreq(parsed.activeNotes[0]) - 1000) < 2, parsed.activeNotes && noteFreq(parsed.activeNotes[0]));
    ok('parseIno no mode', typeof parsed.mode === 'undefined');

    // JSON round-trip keeps step / freq / colour
    const j = JSON.parse(makeJson());
    applyState({ bpm:j.bpm, steps:j.steps, gate:j.gate, duty:j.duty, pin:j.pin, pattern:j.pattern, activeNotes:j.activeNotes });
    ok('round-trip step exact', activeNotes[0].step === 0, activeNotes[0].step);
    ok('round-trip freq', Math.abs(noteFreq(activeNotes[0]) - 1000) < 0.01, noteFreq(activeNotes[0]));
    ok('round-trip color', activeNotes[0].color === 2, activeNotes[0].color);
    ok('snap has no mode', typeof snapProject().mode === 'undefined');

    // single-note side form follows the selection
    $('noteRail').hidden = false;
    activeNotes = [
      { id: nextNoteId++, step: 0, len: 1, midi: 60, vol: 1, freq: 0, color: 0 },
      { id: nextNoteId++, step: 2, len: 3, midi: 64, vol: 1, freq: 0, color: 1 }
    ];
    selectedNote = null; updateNotePanel();
    ok('form hidden with no selection', $('noteForm').hidden === true && $('noteEmpty').hidden === false);
    selectedNote = activeNotes[1].id; updateNotePanel();
    ok('form shown on select', $('noteForm').hidden === false);
    ok('form shows length', +$('nLen').value === 3, +$('nLen').value);
    ok('form shows sound name', /^E4/.test($('nPitch').textContent), $('nPitch').textContent);
    selectedNote = activeNotes[0].id; $('nFreq').value = 880; applyNoteFreq($('nFreq'), true);
    ok('side freq sets note', activeNotes[0].freq === 880 && Math.abs(noteFreq(activeNotes[0]) - 880) < 1e-9, noteFreq(activeNotes[0]));

    // simple mode: palette + length stepper + mode toggle
    ok('starts in simple mode', simpleMode === true);
    selectedNote = activeNotes[0].id; updateNotePanel();
    ok('palette dot per colour', $('nPalette').children.length === PALETTE.length, $('nPalette').children.length);
    const beforeLen = activeNotes[0].len;
    nudgeLen(1);
    ok('length stepper grows note', activeNotes[0].len === beforeLen + 1, activeNotes[0].len);
    nudgeLen(-1);
    ok('length stepper shrinks note', activeNotes[0].len === beforeLen, activeNotes[0].len);
    simpleMode = false; applyUiMode();
    ok('advanced mode drops simple class', document.body.classList.contains('simple') === false);
    simpleMode = true; applyUiMode();
    ok('simple mode adds simple class', document.body.classList.contains('simple') === true);

    // device serial protocol (Play on device)
    S.steps = 32; S.bpm = 150; S.gate = .85; S.loop = true;
    activeNotes = [
      { id: nextNoteId++, step: 2, len: 1, midi: 60, vol: 1, freq: 0, color: 0 },
      { id: nextNoteId++, step: 4, len: 2, midi: 64, vol: 1, freq: 440, color: 1 },
      { id: nextNoteId++, step: 6, len: 1, midi: 67, vol: 0, freq: 0, color: 2 }
    ];
    const dl = deviceSongLines();
    ok('device protocol header', dl[0] === 'RESET' && dl[1] === 'BPM 150' && dl[2] === 'STEPS 32' && dl[3] === 'GATE 85', dl.slice(0, 4).join('|'));
    ok('device protocol skips silent notes', dl.filter(l => /^N /.test(l)).length === 2, dl.filter(l => /^N /.test(l)).length);
    ok('device protocol keeps custom freq', dl.indexOf('N 4 2 440') > -1, dl.join('|'));
    ok('device protocol play line', dl[dl.length - 1] === 'PLAY 1', dl[dl.length - 1]);

    // rotate never overflows
    S.steps = 4;
    activeNotes = [{ id: nextNoteId++, step: 3, len: 3, midi: 60, vol: 1, freq: 0, color: 0 }];
    rotate(1);
    ok('rotate clamps tail', activeNotes[0].step + activeNotes[0].len <= S.steps, activeNotes[0].step+'/'+activeNotes[0].len);

    // legacy ACTIVE ino still imports (as passive note rows)
    const NL = String.fromCharCode(10);
    const legacyA = ['#define BUZZER_PIN 4','#define TEMPO_BPM 120','#define SONG_STEPS 8','#define GATE_PCT 50','/* generated (ACTIVE mode) */','const uint8_t SONG[SONG_STEPS] = {','  1, 0, 1, 1, 0, 0, 1, 0','};'].join(NL);
    const pa = parseIno(legacyA);
    ok('legacy active parse no mode', typeof pa.mode === 'undefined');
    ok('legacy active parse pattern', pa.pattern && pa.pattern.filter(v => v === 72).length === 4, pa.pattern && pa.pattern.filter(v=>v===72).length);
  } catch(e){
    out.push('THREW :: ' + (e && e.stack ? e.stack : e));
  }
  globalThis.__RESULT = out;
})();
`;

const full = js.replace(/\}\)\(\);\s*$/, TEST + '\n})();');
try {
  vm.runInContext(full, globalObj, { filename: 'studio.js' });
} catch (e) {
  console.log('LOAD THREW:', e && e.stack ? e.stack : e);
}
const res = globalObj.__RESULT || ['(no result)'];
const openStyle = (html.match(/<style\b/gi) || []).length;
const closeStyle = (html.match(/<\/style>/gi) || []).length;
res.unshift((openStyle === closeStyle && openStyle > 0 ? 'PASS' : 'FAIL') + ' html style tag balanced :: ' + openStyle + '/' + closeStyle);
const failed = res.filter(x => /^(FAIL|THREW)/.test(x));
res.forEach(r => console.log(r));
console.log('\n' + (failed.length ? failed.length + ' FAILING' : 'ALL PASS') + ' (' + res.length + ' checks)');
