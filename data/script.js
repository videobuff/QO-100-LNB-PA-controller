/*
 * Project: QO-100 LNB/PA Controller
 * Author: PA0ESH (Erik Schott)
 * Year: 2025
 * License: MIT
 * Version: 015
 */

console.log("[UI] v015 booting…");

let swrLatched = false;
let wsConnected = false;
let mainPaState = false;
let isSaving = false;

const gaugeConfig = {
  forward:   { min: 0, max: 50, startX: 48 / 1100, endX: 1050 / 1100 },
  reflected: { min: 0, max: 25, startX: 48 / 1100, endX: 1050 / 1100 }
};

// Netjes instelbare naald-offsets
const needleLayout = {
  baseImageHeight: 300,
  forY: 35,    forYOffset: 20,
  refY: 175,   refYOffset: 16
};

function fetchNoCache(url, options={}) {
  const headers = Object.assign({ 'Cache-Control': 'no-store' }, options.headers || {});
  return fetch(url, Object.assign({}, options, { headers })).then(async res=>{
    const ct = res.headers.get('content-type')||"";
    const body = ct.includes('application/json') ? await res.clone().json().catch(()=>null)
                                                 : await res.clone().text().catch(()=>null);
    if (!res.ok) throw new Error(`HTTP ${res.status} on ${url}`);
    return {res, body};
  });
}

function setAlertOffset() {
  const header = document.getElementById('pageHeader');
  const h = header ? header.getBoundingClientRect().height : 64;
  document.documentElement.style.setProperty('--alertTop', (h + 12) + 'px');
}
function showAlert(msg, type="danger") {
  setAlertOffset();
  const anchor = document.getElementById('alertAnchor') || document.body;
  const div = document.createElement("div");
  div.className = `alert alert-${type}`;
  div.innerHTML = `<span>${msg}</span><button class="alert-close" aria-label="Close">×</button>`;
  anchor.appendChild(div);
  div.querySelector(".alert-close").addEventListener("click", () => div.remove());
  setTimeout(()=>div.remove(), 4000);
}

function updateUTCTime() {
  const el = document.getElementById("utcTime");
  if (!el) return;
  const n = new Date();
  const M = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
  el.textContent = `UTC: ${n.getUTCDate()}-${M[n.getUTCMonth()]}-${n.getUTCFullYear()} ${String(n.getUTCHours()).padStart(2,'0')}:${String(n.getUTCMinutes()).padStart(2,'0')}`;
  setTimeout(updateUTCTime, 1000);
}

function updateGaugeDimensions() {
  const img = document.getElementById("gaugeImage");
  const svg = document.getElementById("gaugeOverlay");
  const nf  = document.getElementById("needleForward");
  const nr  = document.getElementById("needleReflected");
  if (!img || !svg || !nf || !nr) return 0;

  const w = img.clientWidth, h = img.clientHeight;
  svg.setAttribute("width", w); svg.setAttribute("height", h);

  const scaleY = h / needleLayout.baseImageHeight;
  nf.setAttribute("y", (needleLayout.forY * scaleY) + needleLayout.forYOffset);
  nf.setAttribute("height", 80 * scaleY);
  nr.setAttribute("y", (needleLayout.refY * scaleY) + needleLayout.refYOffset);
  nr.setAttribute("height", 80 * scaleY);

  return w;
}

function updateNeedle(cfg, value, needleId, labelId, labelText) {
  const w = updateGaugeDimensions(); if (w === 0) return;

  const start = cfg.startX * w, end = cfg.endX * w;
  const clamped = Math.max(cfg.min, Math.min(value, cfg.max));
  const frac = (clamped - cfg.min) / (cfg.max - cfg.min);
  const x = start + frac * (end - start);

  const needle = document.getElementById(needleId);
  if (needle) {
    const thickness = parseInt(document.getElementById("needleThickness")?.value || 2, 10);
    const color = document.getElementById("needleColor")?.value || "#000000";
    needle.style.transition = 'x 0.3s ease';
    requestAnimationFrame(() => {
      needle.setAttribute("x", x);
      needle.setAttribute("width", thickness);
      needle.setAttribute("fill", color);
    });
  }

  const label = document.getElementById(labelId);
  if (label) label.textContent = `${labelText}: ${Number(value).toFixed(1)} W`;
}

function updateFirmwareVersion(v) {
  const el = document.getElementById("firmwareVersion");
  if (el) el.textContent = `Firmware ${v} — PA0ESH`;
}
function updateWsStatus(on) {
  const dot = document.getElementById("wsStatusDot");
  if (!dot) return;
  dot.style.backgroundColor = on ? "#22c55e" : "orange";
  dot.classList.toggle("pulse", on);
}

// WebSocket met auto-reconnect
function connectWebSocket() {
  const url = `ws://${window.location.host}/ws`;
  console.log("[WS] connecting", url);
  const ws = new WebSocket(url);
  let backoff = 1000;

  ws.onopen = () => { console.log("[WS] open"); wsConnected = true; updateWsStatus(true); };

  ws.onmessage = (ev) => {
    let data; try { data = JSON.parse(ev.data); } catch(e){ return; }
    if (data.type === "data") {
      updateNeedle(gaugeConfig.forward,   data.forward,   "needleForward",   "forwardBox",   "FOR");
      updateNeedle(gaugeConfig.reflected, data.reflected, "needleReflected", "reflectedBox", "REF");

      const swrBox = document.getElementById("swrBox");
      const tempBox= document.getElementById("tempBox");
      if (swrBox) swrBox.textContent = `SWR: ${Number(data.swr).toFixed(2)}`;
      if (tempBox) tempBox.textContent = `TEMP: ${Number(data.temperature).toFixed(1)}°C`;
      if (data.version) updateFirmwareVersion(data.version);

      // DC rails labels
      const setTxt = (id, txt) => { const el=document.getElementById(id); if (el) el.textContent = txt; };
      if (typeof data.v5  === "number")  setTxt("v5Box",  `5V: ${data.v5.toFixed(2)} V`);
      if (typeof data.v12 === "number")  setTxt("v12Box", `12V: ${data.v12.toFixed(2)} V`);
      if (typeof data.v18 === "number")  setTxt("v18Box", `18V: ${data.v18.toFixed(2)} V`);
      if (typeof data.v28 === "number")  setTxt("v28Box", `28V: ${data.v28.toFixed(2)} V`);

      // SIM/REAL label
      const simBtn = document.getElementById("simToggle");
      if (simBtn) simBtn.textContent = data.simMode ? "SIM" : "REAL";

      // SWR gedrag
      if (data.swr <= 2) {
        swrBox.className = "meter-box bg-dark text-light swr-color-box swr-green";
        if (swrLatched) swrLatched=false;
      } else if (data.swr <= data.swrThreshold) {
        swrBox.className = "meter-box bg-dark text-light swr-color-box swr-orange";
        if (swrLatched) swrLatched=false;
      } else {
        swrBox.className = "meter-box bg-dark text-light swr-color-box swr-red";
        if (!swrLatched && mainPaState) {
          fetchNoCache("/api/latch_swr", { method: "POST" })
            .then(({body})=>{
              mainPaState=false; updateMainPaDisplay();
              swrLatched=true; showAlert(`SWR exceeded threshold of ${body?.swrThreshold ?? 3.0}! PA turned off.`, "danger");
            })
            .catch(err => showAlert("Error latching SWR: " + err.message, "danger"));
        }
      }

      if (data.mainPaState !== undefined && !swrLatched) {
        mainPaState = data.mainPaState === "ON";
        updateMainPaDisplay();
      }
    }
  };

  ws.onclose = () => {
    console.log("[WS] close; reconnecting…");
    wsConnected = false;
    updateWsStatus(false);
    setTimeout(connectWebSocket, backoff);
    backoff = Math.min(backoff * 2, 16000);
  };
  ws.onerror = () => ws.close();
}

// UI handlers
function setupTogglePolListener() {
  const btn = document.getElementById("togglePol"); if (!btn) return;
  btn.addEventListener("click", async ()=>{
    btn.disabled = true;
    try {
      const {body} = await fetchNoCache("/api/toggle", { method:"POST" });
      const vertical = body?.mode === "VERTICAL";
      btn.textContent = vertical ? "SSB" : "DATV";
      btn.classList.toggle("bg-success", vertical);
      btn.classList.toggle("bg-danger", !vertical);
    } finally { btn.disabled = false; }
  });
}
function setupSwrResetListener() {
  const swrBox = document.getElementById("swrBox"); if (!swrBox) return;
  swrBox.addEventListener("click", async ()=>{
    try {
      const {body} = await fetchNoCache("/api/reset_swr", { method:"POST" });
      swrLatched=false;
      showAlert(body?.changed ? "SWR reset successfully, PA remains off until toggled" : "SWR was not latched", body?.changed ? "success" : "info");
    } catch(e){ showAlert("Error resetting SWR: "+e.message,"danger"); }
  });
}
function setupMainPaToggleListener() {
  const btn = document.getElementById("mainPaToggle"); if (!btn) return;
  btn.addEventListener("click", async ()=>{
    if (swrLatched) { showAlert("Cannot turn PA on while SWR is latched. Reset SWR first.","warning"); return; }
    const prev = mainPaState;
    mainPaState = !mainPaState;
    updateMainPaDisplay();
    try {
      const {body} = await fetchNoCache("/api/main_pa_toggle", { method:"POST" });
      mainPaState = (body?.state === "ON");
      updateMainPaDisplay();
    } catch(e){
      mainPaState = prev; updateMainPaDisplay();
      showAlert("Error toggling main PA: "+e.message,"danger");
    }
  });
}
function setupSimToggle(){
  const btn = document.getElementById("simToggle"); if (!btn) return;
  btn.addEventListener("click", async ()=>{
    btn.disabled = true;
    try {
      const {body} = await fetchNoCache("/api/sim_toggle", { method:"POST" });
      btn.textContent = body?.simMode ? "SIM" : "REAL";
      showAlert(`Mode: ${body?.simMode ? "Simulator" : "Real ADC"}`, "info");
    } catch(e){ showAlert("Error toggling sim/real: "+e.message,"danger"); }
    finally { btn.disabled = false; }
  });
}
function toggleOtaForm(){ const f=document.getElementById("otaForm"); if (f) f.style.display = (f.style.display==="block"?"none":"block"); }
function toggleSettings(){ const f=document.getElementById("settingsForm"); if (f) f.style.display = (f.style.display==="block"?"none":"block"); }

function updateMainPaDisplay() {
  const result = document.getElementById("mainPaToggle");
  if (!result) return;
  result.textContent = mainPaState ? "ON" : "OFF";
  result.classList.toggle("bg-success", mainPaState);
  result.classList.toggle("bg-danger", !mainPaState);
}

// Settings laden/toepassen
function applyLoadedSettings(data) {
  const fwd  = Number(data.forwardMax ?? 50);
  const ref  = Number(data.reflectedMax ?? 25);
  const thick= Number.isFinite(Number(data.needleThickness)) ? Number(data.needleThickness) : 2;
  const color= (typeof data.needleColor === "string" && data.needleColor.length) ? data.needleColor : "#000000";
  const thr  = Number(data.swrThreshold ?? 3);

  document.getElementById("forwardMax").value     = fwd;
  document.getElementById("reflectedMax").value   = ref;
  document.getElementById("needleThickness").value= thick;
  document.getElementById("needleColor").value    = color;
  document.getElementById("swrThreshold").value   = thr;

  // NIEUW: scale factors UI sync
  if (data.v5Scale  != null) document.getElementById("v5Scale").value  = Number(data.v5Scale).toFixed(4);
  if (data.v12Scale != null) document.getElementById("v12Scale").value = Number(data.v12Scale).toFixed(4);
  if (data.v18Scale != null) document.getElementById("v18Scale").value = Number(data.v18Scale).toFixed(4);
  if (data.v28Scale != null) document.getElementById("v28Scale").value = Number(data.v28Scale).toFixed(4);

  gaugeConfig.forward.max   = fwd;
  gaugeConfig.reflected.max = ref;
  document.getElementById("needleForward").setAttribute("width", thick);
  document.getElementById("needleReflected").setAttribute("width", thick);
  document.getElementById("needleForward").setAttribute("fill", color);
  document.getElementById("needleReflected").setAttribute("fill", color);
}

async function syncStatus() {
  try {
    const {body} = await fetchNoCache("/api/status");
    mainPaState = body?.mainPaState === "ON";
    updateMainPaDisplay();
    const polBtn = document.getElementById("togglePol");
    const vertical = body?.pol === "VERTICAL";
    if (polBtn) {
      polBtn.textContent = vertical ? "SSB" : "DATV";
      polBtn.classList.toggle("bg-success", vertical);
      polBtn.classList.toggle("bg-danger", !vertical);
    }
    const simBtn = document.getElementById("simToggle");
    if (simBtn) simBtn.textContent = body?.simMode ? "SIM" : "REAL";
    if (body?.version) updateFirmwareVersion(body.version);

    // init rails
    if (typeof body.v5 === "number")  document.getElementById("v5Box").textContent   = `5V: ${body.v5.toFixed(2)} V`;
    if (typeof body.v12 === "number") document.getElementById("v12Box").textContent  = `12V: ${body.v12.toFixed(2)} V`;
    if (typeof body.v18 === "number") document.getElementById("v18Box").textContent  = `18V: ${body.v18.toFixed(2)} V`;
    if (typeof body.v28 === "number") document.getElementById("v28Box").textContent  = `28V: ${body.v28.toFixed(2)} V`;
  } catch(e){
    console.warn("[status] failed:", e.message);
  }
}

function applySettings() {
  const forwardMax   = parseFloat(document.getElementById("forwardMax").value);
  const reflectedMax = parseFloat(document.getElementById("reflectedMax").value);
  const thickness    = parseInt(document.getElementById("needleThickness").value, 10);
  const color        = document.getElementById("needleColor").value;
  const swrThreshold = parseFloat(document.getElementById("swrThreshold").value);

  // NIEUW: scale factors uit UI
  const v5Scale      = parseFloat(document.getElementById("v5Scale").value);
  const v12Scale     = parseFloat(document.getElementById("v12Scale").value);
  const v18Scale     = parseFloat(document.getElementById("v18Scale").value);
  const v28Scale     = parseFloat(document.getElementById("v28Scale").value);

  gaugeConfig.forward.max   = forwardMax;
  gaugeConfig.reflected.max = reflectedMax;
  document.getElementById("needleForward").setAttribute("width", thickness);
  document.getElementById("needleReflected").setAttribute("width", thickness);
  document.getElementById("needleForward").setAttribute("fill", color);
  document.getElementById("needleReflected").setAttribute("fill", color);

  saveSettingsRobust({
      forwardMax, reflectedMax, needleThickness: thickness, needleColor: color, swrThreshold,
      v5Scale, v12Scale, v18Scale, v28Scale
    })
    .then(() => fetchNoCache("/api/load_settings"))
    .then(({body}) => { applyLoadedSettings(body); showAlert("Settings applied and saved successfully","success"); })
    .catch(err => showAlert("Error saving settings: " + err.message, "danger"));
}

async function saveSettingsRobust(settings){
  try {
    const r = await fetch('/api/save_settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
      body: JSON.stringify(settings)
    });
    if (!r.ok) throw new Error('POST failed '+r.status);
    const after = await fetch('/api/load_settings', { headers:{'Cache-Control':'no-store'} }).then(x=>x.json());
    const applied =
      Number(after.forwardMax)      === Number(settings.forwardMax) &&
      Number(after.reflectedMax)    === Number(settings.reflectedMax) &&
      Number(after.needleThickness) === Number(settings.needleThickness) &&
      String(after.needleColor).toLowerCase() === String(settings.needleColor).toLowerCase() &&
      Number(after.swrThreshold)    === Number(settings.swrThreshold) &&
      Math.abs(Number(after.v5Scale)  - Number(settings.v5Scale))  < 1e-4 &&
      Math.abs(Number(after.v12Scale) - Number(settings.v12Scale)) < 1e-4 &&
      Math.abs(Number(after.v18Scale) - Number(settings.v18Scale)) < 1e-4 &&
      Math.abs(Number(after.v28Scale) - Number(settings.v28Scale)) < 1e-4;
    if (applied) return;
  } catch(e){ /* fallback hieronder */ }
  const q = new URLSearchParams({
    forwardMax:       String(settings.forwardMax),
    reflectedMax:     String(settings.reflectedMax),
    needleThickness:  String(settings.needleThickness),
    needleColor:      String(settings.needleColor),
    swrThreshold:     String(settings.swrThreshold),
    v5Scale:          String(settings.v5Scale),
    v12Scale:         String(settings.v12Scale),
    v18Scale:         String(settings.v18Scale),
    v28Scale:         String(settings.v28Scale)
  }).toString();
  const url = `/api/save_settings?${q}`;
  const r2 = await fetch(url, { headers: { 'Cache-Control': 'no-store' } });
  if (!r2.ok) throw new Error('GET fallback failed '+r2.status);
}

// Init
document.addEventListener('DOMContentLoaded', async ()=>{
  setAlertOffset();
  updateUTCTime();
  await syncStatus();
  fetchNoCache("/api/load_settings").then(({body})=>applyLoadedSettings(body)).catch(()=>{});

  document.getElementById('saveSettingsButton')?.addEventListener('click', ()=>{
    if (isSaving) return; isSaving=true;
    const settings = {
      forwardMax: parseFloat(document.getElementById('forwardMax').value) || 50,
      reflectedMax: parseFloat(document.getElementById('reflectedMax').value) || 25,
      needleThickness: parseInt(document.getElementById('needleThickness').value,10) || 2,
      needleColor: document.getElementById('needleColor').value || '#000000',
      swrThreshold: parseFloat(document.getElementById('swrThreshold').value) || 3,
      v5Scale:  parseFloat(document.getElementById('v5Scale').value)  || 1.6818,
      v12Scale: parseFloat(document.getElementById('v12Scale').value) || 4.0909,
      v18Scale: parseFloat(document.getElementById('v18Scale').value) || 6.4545,
      v28Scale: parseFloat(document.getElementById('v28Scale').value) || 10.0909
    };
    saveSettingsRobust(settings)
      .then(()=>fetchNoCache('/api/load_settings'))
      .then(({body})=>{ applyLoadedSettings(body); showAlert("Settings applied and saved successfully","success"); })
      .catch(e=>showAlert("Error saving settings: "+e.message,"danger"))
      .finally(()=>{ isSaving=false; });
  });

  document.getElementById("togglePol")    && setupTogglePolListener();
  document.getElementById("swrBox")       && setupSwrResetListener();
  document.getElementById("mainPaToggle") && setupMainPaToggleListener();
  document.getElementById("simToggle")    && setupSimToggle();

  connectWebSocket();
  window.addEventListener("resize", ()=>{ updateGaugeDimensions(); setAlertOffset(); });
});
