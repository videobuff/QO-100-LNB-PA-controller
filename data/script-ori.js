// QO-100 Controller – script.js (compat + rails)
const el = {
  forwardBox:   document.getElementById('forwardBox'),
  reflectedBox: document.getElementById('reflectedBox'),
  swrBox:       document.getElementById('swrBox'),
  tempBox:      document.getElementById('tempBox'),

  mainPaToggle: document.getElementById('mainPaToggle'),
  togglePol:    document.getElementById('togglePol'),
  simToggle:    document.getElementById('simToggle'),

  wsDot:        document.getElementById('wsStatusDot'),

  forwardNeedle:   document.getElementById('needleForward'),
  reflectedNeedle: document.getElementById('needleReflected'),
  gaugeOverlay:    document.getElementById('gaugeOverlay'),

  // settings
  settingsForm:     document.getElementById('settingsForm'),
  forwardMax:       document.getElementById('forwardMax'),
  reflectedMax:     document.getElementById('reflectedMax'),
  needleColor:      document.getElementById('needleColor'),
  needleThickness:  document.getElementById('needleThickness'),
  swrThreshold:     document.getElementById('swrThreshold'),
  v5Scale:          document.getElementById('v5Scale'),
  v12Scale:         document.getElementById('v12Scale'),
  v18Scale:         document.getElementById('v18Scale'),
  v28Scale:         document.getElementById('v28Scale'),
  saveSettings:     document.getElementById('saveSettingsButton'),
  applySettings:    document.getElementById('applySettingsButton'),

  // rails
  railsRow:  document.getElementById('railsRow'),
  rail5Box:  document.getElementById('rail5Box'),
  rail12Box: document.getElementById('rail12Box'),
  rail18Box: document.getElementById('rail18Box'),
  rail28Box: document.getElementById('rail28Box')
};

function alertShow(msg, type='danger', timeoutMs=3500){
  const stack = document.getElementById('alert-stack');
  const box = document.createElement('div');
  box.className = `alert alert-${type}`;
  box.innerHTML = `<button class="alert-close" onclick="this.parentNode.remove()">×</button>${msg}`;
  stack.appendChild(box);
  if (timeoutMs>0) setTimeout(()=>box.remove(), timeoutMs);
}

// --- Fetch helpers ---
async function fetchText(url, opts={}){
  const res = await fetch(url, Object.assign({ method: 'POST', headers: {'Accept':'text/plain'}}, opts));
  const txt = await res.text();
  if (!res.ok) throw new Error(txt || res.statusText);
  return txt;
}
async function fetchJSON(url, opts={}){
  const res = await fetch(url, Object.assign({ headers: {'Accept':'application/json','Content-Type':'application/json'}}, opts));
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch(e) {}
  if (!res.ok) throw new Error((data && (data.error||data.message)) || text || res.statusText);
  return data;
}

// --- UI helpers ---
function setPaState(state){
  const on = String(state).toUpperCase()==='ON';
  el.mainPaToggle.textContent = on ? 'ON' : 'OFF';
  el.mainPaToggle.classList.toggle('bg-danger', on);
  el.mainPaToggle.classList.toggle('bg-success', !on);
}
function setMode(mode){
  const isSSB = String(mode).toUpperCase()==='SSB';
  el.togglePol.textContent = isSSB ? 'SSB' : 'DATV';
  el.togglePol.classList.toggle('bg-success', isSSB);
  el.togglePol.classList.toggle('bg-danger', !isSSB);
}
function setSim(sim){
  el.simToggle.textContent = sim ? 'SIM' : 'REAL';
  el.simToggle.classList.toggle('bg-primary', !!sim);
  el.simToggle.classList.toggle('bg-success', !sim);
}

function setNeedles(forW, refW){
  const overlay = el.gaugeOverlay.getBoundingClientRect();
  const left = 20, right = overlay.width - 20;
  const fx = (val, max) => {
    const clamped = Math.max(0, Math.min(1, max>0 ? (val/max) : 0));
    return Math.round(left + clamped * (right-left));
  };
  const fwdX = fx(forW, parseFloat(el.forwardMax.value)||50);
  const refX = fx(refW, parseFloat(el.reflectedMax.value)||25);
  el.forwardNeedle.setAttribute('x', String(fwdX));
  el.reflectedNeedle.setAttribute('x', String(refX));

  const color = el.needleColor.value || '#000000';
  const thick = parseInt(el.needleThickness.value||2,10);
  el.forwardNeedle.setAttribute('width', String(thick));
  el.reflectedNeedle.setAttribute('width', String(thick));
  el.forwardNeedle.setAttribute('fill', color);
  el.reflectedNeedle.setAttribute('fill', color);
}

function setSWR(swr, threshold){
  const box = el.swrBox;
  box.textContent = `SWR: ${swr.toFixed(2)}`;
  box.classList.remove('swr-ok','swr-warn','swr-bad');
  if (swr < Math.max(1.0, threshold*0.8)) box.classList.add('swr-ok');
  else if (swr < threshold)               box.classList.add('swr-warn');
  else                                    box.classList.add('swr-bad');
}

// Rails
function updateRailsFromMsg(msg){
  if (!msg || !msg.rails){ el.railsRow.style.display='none'; return; }
  const v = msg.rails;
  const f = (x)=> (typeof x==='number' ? x.toFixed(2) : '–.–');
  el.railsRow.style.display = 'flex';
  el.rail5Box.textContent  = `5V: ${f(v.v5)} V`;
  el.rail12Box.textContent = `12V: ${f(v.v12)} V`;
  el.rail18Box.textContent = `18V: ${f(v.v18)} V`;
  el.rail28Box.textContent = `28V: ${f(v.v28)} V`;
}

// --- Actions ---
async function togglePA(){
  try{ const data = await fetchJSON('/api/main_pa_toggle', { method:'POST' }); setPaState(data.state); }
  catch(e){ alertShow('PA toggle failed: '+e.message,'danger'); }
}
async function togglePol(){
  try{ const data = await fetchJSON('/api/toggle', { method:'POST' }); setMode(data.mode); }
  catch(e){ alertShow('Mode toggle failed: '+e.message,'danger'); }
}
async function toggleSim(){
  try{
    const s = await fetchJSON('/api/load_settings');
    const newSim = !s.sim;
    s.sim = newSim;
    await fetchJSON('/api/save_settings', { method:'POST', body: JSON.stringify(s) });
    setSim(newSim);
  }catch(e){ alertShow('SIM toggle failed: '+e.message,'danger'); }
}
async function latchSWR(){
  try{ const txt = await fetchText('/api/latch_swr', { method:'POST' }); alertShow('Latch: '+txt,'success',1400); }
  catch(e){ alertShow('Latch failed: '+e.message,'danger'); }
}
async function resetSWR(){
  try{ const txt = await fetchText('/api/reset_swr', { method:'POST' }); alertShow('Reset: '+txt,'info',1400); }
  catch(e){ alertShow('Reset failed: '+e.message,'danger'); }
}

// --- Settings ---
function toggleSettings(){ el.settingsForm.style.display = (el.settingsForm.style.display==='none' ? 'block' : 'none'); }
function toggleOtaForm(){ const x=document.getElementById('otaForm'); x.style.display = (x.style.display==='none' ? 'block' : 'none'); }

async function loadSettings(){
  try{
    const s = await fetchJSON('/api/load_settings');
    el.forwardMax.value      = s.forwardMax;
    el.reflectedMax.value    = s.reflectedMax;
    el.needleThickness.value = s.needleThickness;
    el.needleColor.value     = s.needleColor || '#000000';
    el.swrThreshold.value    = s.swrThreshold;
    el.v5Scale.value         = s.v5Scale ?? '';
    el.v12Scale.value        = s.v12Scale ?? '';
    el.v18Scale.value        = s.v18Scale ?? '';
    el.v28Scale.value        = s.v28Scale ?? '';
    setSim(!!s.sim);
  }catch(e){ alertShow('Load settings failed: '+e.message,'danger'); }
}
async function saveSettings(){
  try{
    const body = {
      forwardMax: parseFloat(el.forwardMax.value),
      reflectedMax: parseFloat(el.reflectedMax.value),
      needleThickness: parseInt(el.needleThickness.value,10),
      needleColor: el.needleColor.value,
      swrThreshold: parseFloat(el.swrThreshold.value),
      v5Scale: parseFloat(el.v5Scale.value),
      v12Scale: parseFloat(el.v12Scale.value),
      v18Scale: parseFloat(el.v18Scale.value),
      v28Scale: parseFloat(el.v28Scale.value),
      sim: (el.simToggle.textContent==='SIM')
    };
    await fetchJSON('/api/save_settings', { method:'POST', body: JSON.stringify(body) });
    alertShow('Settings saved','success', 1200);
  }catch(e){ alertShow('Save failed: '+e.message,'danger'); }
}
function applySettingsLocal(){ alertShow('Applied locally','info', 900); }

// --- WS Telemetry ---
let ws;
function connectWS(){
  try{
    ws = new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');
    ws.onopen = ()=>{ el.wsDot.style.background='#28a745'; el.wsDot.classList.add('pulse'); };
    ws.onclose= ()=>{ el.wsDot.style.background='#999'; el.wsDot.classList.remove('pulse'); setTimeout(connectWS, 1500); };
    ws.onerror= ()=>{ el.wsDot.style.background='#dc3545'; el.wsDot.classList.remove('pulse'); };

    ws.onmessage = (ev)=>{
      let msg; try{ msg = JSON.parse(ev.data); }catch(_){ return; }
      if (msg.type==='hello'){ setSim(!!msg.sim); }
      else if (msg.type==='data'){
        const fwd = Number(msg.forward||0), ref = Number(msg.reflected||0);
        const swr = Number(msg.swr||1), t = Number(msg.temperature||25);
        el.forwardBox.textContent   = `FOR: ${fwd.toFixed(1)} W`;
        el.reflectedBox.textContent = `REF: ${ref.toFixed(1)} W`;
        el.tempBox.textContent      = `TEMP: ${t.toFixed(1)}°C`;
        setNeedles(fwd, ref);
        setSWR(swr, parseFloat(el.swrThreshold.value)||3);
        setPaState(msg.mainPaState || 'OFF');
        updateRailsFromMsg(msg);
      }
    };
  }catch(e){ console.error('WS error',e); }
}

// --- Clock ---
function tickUTC(){
  const d = new Date();
  const s = d.toUTCString().replace('GMT','UTC');
  document.getElementById('utcTime').textContent = 'UTC: '+s;
}

// --- Wire up ---
function init(){
  el.mainPaToggle.addEventListener('click', togglePA);
  el.togglePol.addEventListener('click', togglePol);
  el.simToggle.addEventListener('click', toggleSim);
  el.saveSettings.addEventListener('click', saveSettings);
  el.applySettings.addEventListener('click', applySettingsLocal);

  loadSettings();
  connectWS();
  setInterval(tickUTC, 1000);
  tickUTC();
}
document.addEventListener('DOMContentLoaded', init);
