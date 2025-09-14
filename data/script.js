/* QO-100 Controller – script.js v016 */

let wsConnected = false;
let swrLatched  = false;
let mainPaOn    = false;
let simMode     = true;

const byId = (id) => document.getElementById(id);
const authHdr = () => ({ "Authorization": "Basic " + btoa("admin:password") });

const gaugeConfig = {
  forward:   { min:0, max:50, startX:38/1100, endX:1050/1100, y:25,  h:80 },
  reflected: { min:0, max:25, startX:38/1100, endX:1050/1100, y:165, h:80 }
};

// ---------- Alerts (fixed top, schuift layout NIET) ----------
function showAlert(message, type="danger", timeoutMs=2200){
  const stack = byId("alert-stack");
  const div = document.createElement("div");
  div.className = `alert alert-${type}`;
  div.innerHTML = `<span>${message}</span><button class="alert-close">×</button>`;
  stack.appendChild(div);
  div.querySelector(".alert-close").onclick = () => div.remove();
  if (timeoutMs>0) setTimeout(()=>div.remove(), timeoutMs);
}

// ---------- UTC ----------
function updateUTCTime(){
  const el = byId("utcTime"); if(!el) return;
  const now=new Date();
  const dd=String(now.getUTCDate()).padStart(2,"0");
  const mths=["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
  const mm=mths[now.getUTCMonth()];
  const yy=now.getUTCFullYear();
  const h =String(now.getUTCHours()).padStart(2,"0");
  const m =String(now.getUTCMinutes()).padStart(2,"0");
  el.textContent = `UTC: ${dd}-${mm}-${yy} ${h}:${m}`;
  setTimeout(updateUTCTime,1000);
}

// ---------- Gauge ----------
function updateGaugeDimensions(){
  const img=byId("gaugeImage"), svg=byId("gaugeOverlay");
  if(!img||!svg) return 0;
  const w=img.clientWidth, h=img.clientHeight;
  svg.setAttribute("width", w);
  svg.setAttribute("height", h);
  const sy=h/300;
  byId("needleForward")?.setAttribute("y",  gaugeConfig.forward.y*sy);
  byId("needleForward")?.setAttribute("height",gaugeConfig.forward.h*sy);
  byId("needleReflected")?.setAttribute("y",  gaugeConfig.reflected.y*sy);
  byId("needleReflected")?.setAttribute("height",gaugeConfig.reflected.h*sy);
  return w;
}

function updateNeedle(cfg, value, needleId, labelId, labelText){
  const w = updateGaugeDimensions(); if(w===0) return;
  const x0=cfg.startX*w, x1=cfg.endX*w;
  const v = Math.max(cfg.min, Math.min(value, cfg.max));
  const f = (v - cfg.min)/(cfg.max-cfg.min);
  const x = x0 + f*(x1-x0);

  const needle=byId(needleId), label=byId(labelId);
  if(!needle||!label) return;

  const thick = parseInt(byId("needleThickness")?.value||"2",10);
  const color = byId("needleColor")?.value || "#000000";

  needle.style.transition="x .25s ease";
  needle.setAttribute("x", x);
  needle.setAttribute("width", thick);
  needle.setAttribute("fill", color);

  label.textContent = `${labelText}: ${value.toFixed(1)} W`;
}

// ---------- Paint helpers ----------
function paintModeButton(isSSB){
  const b=byId("togglePol"); if(!b) return;
  b.textContent = isSSB ? "SSB" : "DATV";
  b.classList.remove("bg-success","bg-danger");
  b.classList.add(isSSB ? "bg-success" : "bg-danger");
}
function paintPaButton(isOn){
  const b=byId("mainPaToggle"); if(!b) return;
  b.textContent = isOn ? "ON" : "OFF";
  b.classList.remove("bg-success","bg-danger");
  // Off = groen, On = rood (zoals je vroeg)
  b.classList.add(isOn ? "bg-danger" : "bg-success");
}
function paintSimButton(isSim){
  const b=byId("simToggle"); if(!b) return;
  simMode = isSim;
  b.textContent = isSim ? "SIM" : "REAL";
  b.classList.remove("bg-primary","bg-success","bg-secondary");
  // REAL = groen, SIM = blauw
  b.classList.add(isSim ? "bg-primary" : "bg-success");
}
function setSwrClass(name){
  const el=byId("swrBox"); if(!el) return;
  el.className = `meter-box swr-box ${name}`;
}

// ---------- WS ----------
function updateWsStatus(ok){
  const d=byId("wsStatusDot"); if(!d) return;
  d.style.backgroundColor = ok ? "#28a745" : "orange";
  d.classList.toggle("pulse", ok);  // laat weer knipperen
}
function connectWebSocket(){
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = ()=>{ wsConnected=true; updateWsStatus(true); };
  ws.onclose= ()=>{ wsConnected=false; updateWsStatus(false); setTimeout(connectWebSocket,1200); };
  ws.onerror= ()=> ws.close();
  ws.onmessage = (ev)=>{
    try{
      const msg = JSON.parse(ev.data);
      if(msg.type==="hello"){
        if(msg.version) byId("firmwareVersion").textContent = `Firmware ${msg.version} — PA0ESH`;
        if(typeof msg.sim === "boolean") paintSimButton(msg.sim);
        return;
      }
      if(msg.type==="data"){
        updateNeedle(gaugeConfig.forward,   msg.forward??0,   "needleForward","forwardBox","FOR");
        updateNeedle(gaugeConfig.reflected, msg.reflected??0, "needleReflected","reflectedBox","REF");

        const swr=Number(msg.swr||1.0), thr=Number(msg.swrThreshold||3.0);
        if(swr<=2.0){ setSwrClass("swr-ok"); swrLatched=false; }
        else if(swr<=thr){ setSwrClass("swr-warn"); swrLatched=false; }
        else {
          setSwrClass("swr-bad");
          if(!swrLatched){
            swrLatched=true; mainPaOn=false; paintPaButton(false);
            fetch("/api/latch_swr",{method:"POST",headers:authHdr()}).catch(()=>{});
          }
        }
        const t=byId("tempBox"); if(t) t.textContent=`TEMP: ${(msg.temperature??25).toFixed(1)}°C`;
        if(typeof msg.mainPaState==="string" && !swrLatched){
          mainPaOn = (msg.mainPaState==="ON");
          paintPaButton(mainPaOn);
        }
        if(typeof msg.sim==="boolean") paintSimButton(msg.sim);
      }
    }catch(e){}
  };
}

// ---------- Listeners ----------
function setupTogglePolListener(){
  const btn=byId("togglePol"); if(!btn) return;
  paintModeButton(true);
  btn.onclick = async ()=>{
    try{
      const r = await fetch("/api/toggle",{method:"POST",headers:authHdr()});
      const data = await r.json();
      paintModeButton((data.mode||"SSB").toUpperCase()==="SSB");
    }catch(e){ showAlert("Error toggling mode: "+e.message,"danger"); }
  };
}
function setupMainPaToggleListener(){
  const btn=byId("mainPaToggle"); if(!btn) return;
  paintPaButton(false);
  btn.onclick = async ()=>{
    if(swrLatched){ showAlert("Cannot turn PA on while SWR is latched.","warning"); return; }
    try{
      const r = await fetch("/api/main_pa_toggle",{method:"POST",headers:authHdr()});
      const data = await r.json();
      mainPaOn = data.state==="ON";
      paintPaButton(mainPaOn);
    }catch(e){ showAlert("Toggle PA failed: "+e.message,"danger"); }
  };
}
function setupSwrResetListener(){
  const swrBox=byId("swrBox"); if(!swrBox) return;
  swrBox.onclick = async ()=>{
    try{
      const r = await fetch("/api/reset_swr",{method:"POST",headers:authHdr()});
      const t = await r.text();
      if(t==="SWR_RESET"){ swrLatched=false; setSwrClass("swr-ok"); showAlert("SWR reset.","success"); }
      else showAlert("Please wait before resetting SWR","warning");
    }catch(e){ showAlert("Error resetting SWR: "+e.message,"danger"); }
  };
}

// SIM toggle – slaat sim=… op en forceert meteen een visuele “beweging” als feedback
function setupSimToggle(){
  const btn=byId("simToggle"); if(!btn) return;
  paintSimButton(simMode);
  btn.onclick = async ()=>{
    try{
      const r1 = await fetch("/api/load_settings",{headers:authHdr()});
      if(!r1.ok) throw new Error(`${r1.status} ${r1.statusText}`);
      const cur = await r1.json();
      const nextSim = !Boolean(cur.sim ?? simMode);

      const payload = {
        forwardMax: Number(cur.forwardMax ?? 50),
        reflectedMax: Number(cur.reflectedMax ?? 25),
        needleThickness: Number(cur.needleThickness ?? 2),
        needleColor: String(cur.needleColor ?? "#000000"),
        swrThreshold: Number(cur.swrThreshold ?? 3.0),
        v5Scale:  Number(cur.v5Scale ?? 1.0),
        v12Scale: Number(cur.v12Scale ?? 1.0),
        v18Scale: Number(cur.v18Scale ?? 1.0),
        v28Scale: Number(cur.v28Scale ?? 1.0),
        sim: nextSim
      };

      const r2 = await fetch("/api/save_settings",{
        method:"POST",
        headers:{...authHdr(),"Content-Type":"application/json"},
        body: JSON.stringify(payload)
      });
      if(!r2.ok) throw new Error(`${r2.status} ${r2.statusText}`);

      paintSimButton(nextSim);
      showAlert(`Simulator ${nextSim?"aan":"uit"}`,"info");

      // direct een klein visueel “tikje” zodat je meteen beweging ziet in SIM
      if(nextSim){
        updateNeedle(gaugeConfig.forward,   Math.random()* (payload.forwardMax||50), "needleForward","forwardBox","FOR");
        updateNeedle(gaugeConfig.reflected, Math.random()* (payload.reflectedMax||25), "needleReflected","reflectedBox","REF");
      }
    }catch(e){ showAlert("Sim toggle faalde: "+e.message,"danger"); }
  };
}

// settings
function applyLoadedSettings(d){
  if(d.forwardMax!=null)      byId("forwardMax").value = Number(d.forwardMax);
  if(d.reflectedMax!=null)    byId("reflectedMax").value = Number(d.reflectedMax);
  if(d.needleThickness!=null) byId("needleThickness").value = parseInt(d.needleThickness,10);
  if(d.needleColor)           byId("needleColor").value = String(d.needleColor);
  if(d.swrThreshold!=null)    byId("swrThreshold").value = Number(d.swrThreshold);

  if(d.v5Scale!=null)  byId("v5Scale").value  = Number(d.v5Scale);
  if(d.v12Scale!=null) byId("v12Scale").value = Number(d.v12Scale);
  if(d.v18Scale!=null) byId("v18Scale").value = Number(d.v18Scale);
  if(d.v28Scale!=null) byId("v28Scale").value = Number(d.v28Scale);

  if(typeof d.sim==="boolean") paintSimButton(d.sim);

  // direct naalden stylen
  const thick=parseInt(byId("needleThickness").value,10);
  const color=byId("needleColor").value;
  ["needleForward","needleReflected"].forEach(id=>{
    const n=byId(id); if(n){ n.setAttribute("width",thick); n.setAttribute("fill",color); }
  });

  if(d.forwardMax!=null)   gaugeConfig.forward.max   = Number(d.forwardMax);
  if(d.reflectedMax!=null) gaugeConfig.reflected.max = Number(d.reflectedMax);
}
function collectSettings(){
  return {
    forwardMax:      parseFloat(byId("forwardMax").value),
    reflectedMax:    parseFloat(byId("reflectedMax").value),
    needleThickness: parseInt(byId("needleThickness").value,10),
    needleColor:     byId("needleColor").value,
    swrThreshold:    parseFloat(byId("swrThreshold").value),
    v5Scale:  parseFloat(byId("v5Scale").value),
    v12Scale: parseFloat(byId("v12Scale").value),
    v18Scale: parseFloat(byId("v18Scale").value),
    v28Scale: parseFloat(byId("v28Scale").value),
    sim: simMode
  };
}
async function saveSettings(){
  const settings = collectSettings();
  try{
    const r = await fetch("/api/save_settings",{
      method:"POST",
      headers:{...authHdr(),"Content-Type":"application/json"},
      body: JSON.stringify(settings)
    });
    if(!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    const rl = await fetch("/api/load_settings",{headers:authHdr()});
    const d = await rl.json();
    applyLoadedSettings(d);
    showAlert("Settings opgeslagen.","success");
  }catch(e){ showAlert("Error save settings: "+e.message,"danger"); }
}
function applySettingsLocal(){
  const s=collectSettings();
  gaugeConfig.forward.max   = s.forwardMax;
  gaugeConfig.reflected.max = s.reflectedMax;
  ["needleForward","needleReflected"].forEach(id=>{
    const n=byId(id); if(n){ n.setAttribute("width", s.needleThickness); n.setAttribute("fill", s.needleColor); }
  });
  showAlert("Settings toegepast (lokaal).","info",1600);
}

// OTA
async function checkForUpdate(){
  try{
    const r = await fetch("/api/check_update",{headers:authHdr()});
    const d = await r.json();
    if(d.updateAvailable){
      byId("otaUrl").value = d.updateUrl||"";
      showAlert(`Update ${d.latestVersion} beschikbaar (nu ${d.currentVersion})`,"info");
    } else {
      showAlert("No update available","info");
    }
  }catch(e){ showAlert("Error checking for update: "+e.message,"danger"); }
}
async function performUpdate(){
  const url = byId("otaUrl").value.trim();
  if(!url){ showAlert("Please enter OTA URL","warning"); return; }
  const box=byId("otaProgress"), fill=byId("otaProgressBarFill");
  box.style.display="block"; fill.style.width="0%";
  try{
    const r = await fetch(`/api/update?updateUrl=${encodeURIComponent(url)}`,{method:"POST",headers:authHdr()});
    const txt = await r.text();
    if(r.ok){ fill.style.width="100%"; showAlert("Update request OK","success"); }
    else showAlert("Update failed: "+txt,"danger");
  }catch(e){ showAlert("Error performing update: "+e.message,"danger"); }
}
function hideOtaProgress(){ byId("otaProgress").style.display="none"; }

// sections
function toggleSettings(){ const el=byId("settingsForm"); el.style.display=(el.style.display==="block")?"none":"block"; }
function toggleOtaForm(){ const el=byId("otaForm"); el.style.display=(el.style.display==="block")?"none":"block"; }

// init
document.addEventListener("DOMContentLoaded", async ()=>{
  updateUTCTime();

  setupTogglePolListener();
  setupMainPaToggleListener();
  setupSwrResetListener();
  setupSimToggle();

  byId("saveSettingsButton")?.addEventListener("click", saveSettings);
  byId("applySettingsButton")?.addEventListener("click", applySettingsLocal);

  // settings laden
  try{
    const r = await fetch("/api/load_settings",{headers:authHdr()});
    applyLoadedSettings(await r.json());
  }catch(e){ showAlert("Settings load faalde: "+e.message,"warning"); }

  paintModeButton(true);
  paintPaButton(false);
  setSwrClass("swr-ok");

  connectWebSocket();
  window.addEventListener("resize", updateGaugeDimensions);
  updateGaugeDimensions();
});
