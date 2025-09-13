// =========================
// QO-100 Controller v018.6
// =========================

// ---- AUTH (pas aan als je login wijzigde in firmware) ----
const AUTH = { user: "admin", pass: "password" };
const JSON_HEADERS = () => ({
  "Content-Type": "application/json",
  "Authorization": "Basic " + btoa(`${AUTH.user}:${AUTH.pass}`)
});
const AUTH_HEADERS = () => ({
  "Authorization": "Basic " + btoa(`${AUTH.user}:${AUTH.pass}`)
});

// ---- Global state ----
let swrLatched = false;
let wsConnected = false;
let mainPaState = false; // OFF
let mode = "SSB";        // "SSB" of "DATV"
let simMode = false;
let isSaving = false;
let lastSettings = {};   // UI-bron van waarheid

// Gauge X-config (Y/H via updateGaugeDimensions)
const gaugeConfig = {
  forward:   { min: 0, max: 50, startX: 38/1100,  endX: 1050/1100 },
  reflected: { min: 0, max: 25, startX: 38/1100,  endX: 1050/1100 }
};

// ---- DOM helpers ----
const $ = id => document.getElementById(id);

// ---- Modal helpers ----
function modalShow(msg, title = "Melding") {
  $("modalTitle").textContent = title || "Melding";
  $("modalBody").textContent  = msg || "";
  $("modal").classList.remove("hidden");
}
function modalHide() {
  $("modal").classList.add("hidden");
}
function showAlert(message, type="Info"){ modalShow(message, type); }

// ---- Clock ----
function updateUTCTime(){
  const el = $("utcTime");
  const n = new Date();
  const dd = String(n.getUTCDate()).padStart(2,'0');
  const mm = String(n.getUTCMonth()+1).padStart(2,'0');
  const yy = n.getUTCFullYear();
  const hh = String(n.getUTCHours()).padStart(2,'0');
  const mi = String(n.getUTCMinutes()).padStart(2,'0');
  el.textContent = `UTC: ${dd}-${mm}-${yy} ${hh}:${mi}`;
  setTimeout(updateUTCTime, 1000);
}

// ---- Gauge layout (Y/H) — beide naalden 10px omlaag ----
function updateGaugeDimensions(){
  const img = $("gaugeImage");
  const overlay = $("gaugeOverlay");
  if (!img || !overlay) return;

  const w = img.clientWidth;
  const h = img.clientHeight || (img.naturalHeight * (w / img.naturalWidth));
  overlay.setAttribute("width", w);
  overlay.setAttribute("height", h);

  const scaleY = h / 300;
  const shift = 10 * scaleY;

  const nf = $("needleForward");
  const nr = $("needleReflected");

  nf.setAttribute("y", (35 * scaleY) + shift);
  nf.setAttribute("height", (80 * scaleY));

  nr.setAttribute("y", (175 * scaleY) + shift);
  nr.setAttribute("height", (80 * scaleY));
}

// ---- Needles: X, dikte, kleur (inline style wint van CSS) ----
function updateNeedle(config, value, needleId, labelId, labelText){
  const img = $("gaugeImage");
  const needle = $(needleId);
  const label = $(labelId);
  if (!img || !needle || !label) return;

  const w = img.clientWidth;
  const x0 = config.startX * w;
  const x1 = config.endX * w;

  const clamped = Math.max(config.min, Math.min(value, config.max));
  const frac = (clamped - config.min) / (config.max - config.min);
  const x = x0 + frac * (x1 - x0);

  const thick = parseInt($("needleThickness")?.value ?? lastSettings.needleThickness ?? 6);
  const color = $("needleColor")?.value ?? lastSettings.needleColor ?? "#ff0000";

  needle.setAttribute("x", x);
  needle.setAttribute("width", thick);
  needle.style.fill = color;

  label.textContent = `${labelText}: ${value.toFixed(1)} W`;
}

// ---- Status UI ----
function updateWsStatus(ok){
  const dot = $("wsStatusDot");
  dot.style.backgroundColor = ok ? "#2fa84f" : "orange";
  dot.classList.toggle("pulse", ok);
}
function updateMainPaDisplay(){
  const btn = $("mainPaToggle");
  btn.textContent = mainPaState ? "ON" : "OFF";
  btn.classList.toggle("success", mainPaState);  // ON = groen
  btn.classList.toggle("danger", !mainPaState);  // OFF = rood
}
function updateModeDisplay(isSSB){
  mode = isSSB ? "SSB" : "DATV";
  const btn = $("togglePol");
  btn.textContent = mode;
  btn.classList.toggle("success", isSSB);  // SSB = groen
  btn.classList.toggle("danger", !isSSB);  // DATV = rood
}
function setSimButton(simOn){
  simMode = !!simOn;
  const b = $("simToggle");
  b.textContent = simMode ? "SIM ON" : "REAL";
  b.classList.toggle("success", simMode);
  b.classList.toggle("danger", !simMode);
}

// ---- WebSocket ----
function connectWS(){
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen  = ()=>{ wsConnected=true; updateWsStatus(true); };
  ws.onclose = ()=>{ wsConnected=false; updateWsStatus(false); setTimeout(connectWS, 2000); };
  ws.onerror = ()=> ws.close();

  ws.onmessage = (ev)=>{
    const d = JSON.parse(ev.data);
    if (d.type !== "data") return;

    updateNeedle(gaugeConfig.forward,   d.forward,   "needleForward",   "forwardBox",   "FOR");
    updateNeedle(gaugeConfig.reflected, d.reflected, "needleReflected", "reflectedBox", "REF");

    const swrBox = $("swrBox");
    swrBox.textContent = `SWR: ${d.swr.toFixed(2)}`;
    if (d.swr <= 2.0) swrBox.className = "meter-box swr-green";
    else if (d.swr <= (d.swrThreshold ?? (lastSettings.swrThreshold ?? 3.5))) swrBox.className = "meter-box swr-orange";
    else swrBox.className = "meter-box swr-red";

    $("tempBox").textContent = `TEMP: ${d.temperature.toFixed(1)}°C`;

    if (typeof d.v5  !== "undefined") $("v5Box").textContent  = `5V: ${d.v5.toFixed(2)} V`;
    if (typeof d.v12 !== "undefined") $("v12Box").textContent = `12V: ${d.v12.toFixed(2)} V`;
    if (typeof d.v18 !== "undefined") $("v18Box").textContent = `18V: ${d.v18.toFixed(2)} V`;
    if (typeof d.v28 !== "undefined") $("v28Box").textContent = `28V: ${d.v28.toFixed(2)} V`;

    if (d.version) $("firmwareVersion").textContent = `Firmware ${d.version} — PA0ESH`;

    if (typeof d.swrLatched === "boolean") swrLatched = d.swrLatched;
    if (typeof d.mainPaState !== "undefined"){
      mainPaState = d.mainPaState === "ON";
      updateMainPaDisplay();
    }
    if (typeof d.mode !== "undefined"){
      updateModeDisplay(d.mode === "SSB");
    }
    if (typeof d.simMode !== "undefined"){
      setSimButton(!!d.simMode);
    }
  };
}

// ---- Controls ----
function setupControls(){
  $("mainPaToggle").onclick = ()=>{
    if (swrLatched){ modalShow("SWR is gelatched. Reset eerst.","Info"); return; }
    fetch("/api/main_pa_toggle", { method:"POST", headers: AUTH_HEADERS() })
      .then(r=>r.json().catch(()=>({ok:false,error:"Bad JSON"})))
      .then(j=>{
        if (!j.ok){ modalShow("Toggle PA faalde: "+(j.error||"unknown"),"Fout"); return; }
        mainPaState = j.state === "ON";
        updateMainPaDisplay();
      })
      .catch(e=> modalShow("Error toggling PA: "+e,"Fout"));
  };

  $("togglePol").onclick = ()=>{
    fetch("/api/toggle", { method:"POST", headers: AUTH_HEADERS() })
      .then(r=>r.text())
      .then(state=>{
        // Server geeft "VERTICAL" of iets anders; VERTICAL => SSB
        const isSSB = state.includes("VERTICAL");
        updateModeDisplay(isSSB);
      })
      .catch(e=> modalShow("Error toggling mode: "+e,"Fout"));
  };

  $("swrBox").onclick = ()=>{
    fetch("/api/reset_swr", { method:"POST", headers: AUTH_HEADERS() })
      .then(r=>r.json().catch(()=>({})))
      .then(j=>{
        if (j && j.changed) modalShow("SWR reset uitgevoerd","OK");
        else modalShow("Geen latch actief","Info");
      })
      .catch(e=> modalShow("Error resetting SWR: "+e,"Fout"));
  };

  $("settingsToggle").onclick = ()=> $("settingsPanel").classList.toggle("hidden");
  $("otaToggle").onclick      = ()=> $("otaPanel").classList.toggle("hidden");

  $("simToggle").onclick = ()=>{
    // Probeer server-sim te toggelen; zo niet, fallback lokaal
    fetch("/api/sim_toggle", { method:"POST", headers: AUTH_HEADERS() })
      .then(r=>r.json())
      .then(j=> setSimButton(!!j.simMode))
      .catch(_=>{
        setSimButton(!simMode);
      });
  };

  // Modal sluiters
  $("modalClose").onclick = modalHide;
  $("modalOk").onclick    = modalHide;
  $("modal").addEventListener("click", (e)=>{ if (e.target === $("modal")) modalHide(); });
  document.addEventListener("keydown", (e)=>{ if (e.key === "Escape") modalHide(); });

  // Init: modal dicht
  modalHide();
}

// ---- Settings helpers ----
function readNum(id, key, fb){
  const raw = parseFloat($(id).value);
  if (Number.isFinite(raw)) return raw;
  if (typeof lastSettings[key] !== "undefined") return Number(lastSettings[key]);
  return fb;
}
function readInt(id, key, fb){
  const raw = parseInt($(id).value);
  if (Number.isInteger(raw)) return raw;
  if (typeof lastSettings[key] !== "undefined") return parseInt(lastSettings[key])||fb;
  return fb;
}
function readStr(id, key, fb){
  const raw = $(id).value;
  if (raw !== "" && raw != null) return raw;
  if (typeof lastSettings[key] !== "undefined") return String(lastSettings[key]);
  return fb;
}

// Alleen visueel toepassen
function applySettingsLocal(){
  lastSettings.forwardMax      = readNum("forwardMax","forwardMax",50);
  lastSettings.reflectedMax    = readNum("reflectedMax","reflectedMax",25);
  lastSettings.needleThickness = readInt("needleThickness","needleThickness",6);
  lastSettings.needleColor     = readStr("needleColor","needleColor","#ff0000");
  lastSettings.swrThreshold    = readNum("swrThreshold","swrThreshold",3.5);

  lastSettings.v5Scale  = readNum("v5Scale","v5Scale",1.6818);
  lastSettings.v12Scale = readNum("v12Scale","v12Scale",4.0909);
  lastSettings.v18Scale = readNum("v18Scale","v18Scale",6.4545);
  lastSettings.v28Scale = readNum("v28Scale","v28Scale",10.0909);

  gaugeConfig.forward.max   = lastSettings.forwardMax;
  gaugeConfig.reflected.max = lastSettings.reflectedMax;

  $("needleForward").setAttribute("width", lastSettings.needleThickness);
  $("needleReflected").setAttribute("width", lastSettings.needleThickness);
  $("needleForward").style.fill = lastSettings.needleColor;
  $("needleReflected").style.fill = lastSettings.needleColor;
}

// Laden (met Auth) en merge met UI-waarden
function loadSettings(){
  return fetch("/api/load_settings", { headers: AUTH_HEADERS() })
    .then(r=>{
      if(!r.ok) throw new Error(`${r.status} ${r.statusText}`);
      return r.json();
    })
    .then(d=>{
      lastSettings = { ...lastSettings, ...(d||{}) };

      if (typeof lastSettings.forwardMax      !== "undefined") $("forwardMax").value = lastSettings.forwardMax;
      if (typeof lastSettings.reflectedMax    !== "undefined") $("reflectedMax").value = lastSettings.reflectedMax;
      if (typeof lastSettings.needleThickness !== "undefined") $("needleThickness").value = lastSettings.needleThickness;
      if (typeof lastSettings.needleColor     !== "undefined") $("needleColor").value = lastSettings.needleColor;
      if (typeof lastSettings.swrThreshold    !== "undefined") $("swrThreshold").value = lastSettings.swrThreshold;

      if (typeof lastSettings.v5Scale  !== "undefined") $("v5Scale").value  = lastSettings.v5Scale;
      if (typeof lastSettings.v12Scale !== "undefined") $("v12Scale").value = lastSettings.v12Scale;
      if (typeof lastSettings.v18Scale !== "undefined") $("v18Scale").value = lastSettings.v18Scale;
      if (typeof lastSettings.v28Scale !== "undefined") $("v28Scale").value = lastSettings.v28Scale;

      if (typeof lastSettings.mainPaState !== "undefined"){
        mainPaState = lastSettings.mainPaState === "ON";
        updateMainPaDisplay();
      }
      if (typeof lastSettings.mode !== "undefined"){
        updateModeDisplay(lastSettings.mode === "SSB");
      }

      applySettingsLocal();
    })
    .catch(e=> modalShow("Error load settings: "+e, "Fout"));
}

// Opslaan (met Auth) → merge → herladen
function saveSettings(){
  if (isSaving) return;
  isSaving = true; $("saveSettings").disabled = true;

  const payload = {
    forwardMax:      readNum("forwardMax","forwardMax",50),
    reflectedMax:    readNum("reflectedMax","reflectedMax",25),
    needleThickness: readInt("needleThickness","needleThickness",6),
    needleColor:     readStr("needleColor","needleColor","#ff0000"),
    swrThreshold:    readNum("swrThreshold","swrThreshold",3.5),

    v5Scale:  readNum("v5Scale","v5Scale",1.6818),
    v12Scale: readNum("v12Scale","v12Scale",4.0909),
    v18Scale: readNum("v18Scale","v18Scale",6.4545),
    v28Scale: readNum("v28Scale","v28Scale",10.0909)
  };

  fetch("/api/save_settings", {
    method:"POST",
    headers: JSON_HEADERS(),
    body: JSON.stringify(payload)
  })
  .then(r=>{
    if(!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    return r.json().catch(()=> ({}));
  })
  .then(resp=>{
    // Als server leeg {} terugstuurt, hou dan payload als waarheid
    lastSettings = { ...lastSettings, ...payload, ...(resp||{}) };
    applySettingsLocal();
    return loadSettings();
  })
  .then(()=> modalShow("Instellingen opgeslagen.","OK"))
  .catch(e=> modalShow("Error save settings: "+e,"Fout"))
  .finally(()=>{ isSaving=false; $("saveSettings").disabled=false; });
}

// ---- OTA ----
function setProgress(pct){
  const bar = $("otaProgress");
  const fill= $("otaProgressFill");
  bar.classList.remove("hidden");
  fill.style.width = `${pct}%`;
  if (pct>=100) setTimeout(()=> bar.classList.add("hidden"), 1200);
}
function checkForUpdate(){
  fetch("/api/check_update", { headers: AUTH_HEADERS() })
    .then(r=>r.json())
    .then(d=>{
      if (d.updateAvailable) modalShow(`Update beschikbaar: ${d.latestVersion}`,"Info");
      else modalShow(`Geen update beschikbaar (huidig: ${d.currentVersion||"?"})`,"Info");
    })
    .catch(e=>modalShow("Error check update: "+e,"Fout"));
}
function doUpdateFile(){
  const f = $("otaFile").files?.[0];
  if (!f){ modalShow("Kies eerst een .bin bestand","Info"); return; }
  const fd = new FormData();
  fd.append("update", f, f.name);
  setProgress(2);
  fetch("/api/update", { method:"POST", headers: AUTH_HEADERS(), body: fd })
    .then(r=>r.json().catch(()=>({ok:false})))
    .then(j=>{
      if (j.ok){ setProgress(100); modalShow("OTA succesvol — reboot kan even duren","OK"); }
      else { setProgress(0); modalShow("OTA mislukt","Fout"); }
    })
    .catch(e=>{ setProgress(0); modalShow("OTA error: "+e,"Fout"); });
}

// ---- INIT ----
function init(){
  // Init knoppen-kleuren meteen correct (fallback) vóór serverdata
  updateModeDisplay(true);   // SSB = groen
  updateMainPaDisplay();     // OFF = rood
  setSimButton(false);       // REAL

  updateUTCTime();
  updateGaugeDimensions();
  const img = $("gaugeImage");
  if (!img.complete) img.onload = updateGaugeDimensions;

  connectWS();
  setupControls();

  // Settings knoppen
  $("applySettings").onclick = ()=> applySettingsLocal();
  $("saveSettings").onclick  = ()=> saveSettings();
  $("reloadSettings").onclick= ()=> loadSettings();

  // OTA knoppen
  $("checkUpdate").onclick = ()=> checkForUpdate();
  $("doUpdate").onclick    = ()=> doUpdateFile();

  // Laad settings (met auth) → zet UI
  loadSettings();

  // Responsive
  window.addEventListener("resize", ()=> updateGaugeDimensions());
}

document.addEventListener("DOMContentLoaded", init);
