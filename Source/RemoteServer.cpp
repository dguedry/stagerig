#include "RemoteServer.h"
#include "PanelParams.h"
#include "MidiEditor.h"
#include "GmMixer.h"
#include "GmSynth.h"
#include "MappingSuggestions.h"
#include "PluginView.h"
#include "Charts.h"
#include "MidiFx.h"
#include <thread>
#include <fcntl.h>
#include "BinaryData.h"
#include <juce_graphics/juce_graphics.h>

#if JUCE_LINUX || JUCE_MAC
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
#endif

using namespace juce;

namespace perf
{

//==============================================================================
// The page. Kept in one string so the app is a single binary with nothing to install.
// Big touch targets, current program first: this is read at arm's length on a stand.
static const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#15161c">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<link rel="manifest" href="/manifest.webmanifest">
<title>StageRig</title>
<style>
  :root { --bg:#15161c; --panel:#1e1f24; --row:#272931; --accent:#5aa9ff; --dim:#9aa0ab; }
  * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
  body { margin:0; background:var(--bg); color:#fff; font:16px/1.4 system-ui,sans-serif;
         padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left); }
  header { display:flex; align-items:center; gap:12px; padding:12px 16px; background:var(--panel); position:sticky; top:0; z-index:2; }
  header h1 { font-size:15px; margin:0; flex:1; color:var(--dim); font-weight:600; letter-spacing:.06em; }
  #panic { background:#a3282d; color:#fff; border:0; border-radius:8px; padding:10px 16px; font-weight:700; font-size:14px; }
  .input { margin:14px 12px 22px; }
  .now { background:var(--panel); border-radius:12px; padding:14px 16px; margin-bottom:10px; }
  .now .name { font-size:12px; color:var(--dim); letter-spacing:.08em; font-weight:700; }
  .now .prog { display:flex; align-items:baseline; gap:12px; margin-top:4px; }
  .now .num { font-size:34px; font-weight:800; color:var(--accent); font-variant-numeric:tabular-nums; }
  .now .title { font-size:22px; font-weight:700; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
                flex:1; min-width:0; }
  /* The plugin's interface, reachable from the header rather than from the slot
     rows further down: this is the one you want mid-set. Pushed to the right so
     it never crowds the program name, which is what the eye comes here for. */
  .now .gui { margin-left:auto; flex:none; background:var(--row); color:#fff; border:0;
              border-radius:10px; padding:10px 16px; font-size:13px; font-weight:800;
              letter-spacing:.04em; align-self:center; }
  .now .gui:active { background:var(--accent); color:#06121f; }
  .now .gui:disabled { opacity:.6; }
  /* An audio input's meter: two bars on a dB scale, and a clip mark that stays
     lit a moment, so a hot microphone is seen from the stand. */
  .meter { display:flex; align-items:center; gap:6px; margin-top:10px; }
  .meter .bars { flex:1; display:flex; flex-direction:column; gap:3px; }
  .meter i { display:block; height:6px; background:#1a1d24; border-radius:3px; overflow:hidden; }
  .meter i span { display:block; height:100%; width:0; border-radius:3px;
                  background:linear-gradient(to right, #4fb86a 0 80%, #d8a657 80% 95%, #e04a4a 95%); background-size:100% 100%; }
  .meter b { flex:none; width:10px; height:15px; border-radius:3px; background:#2a2e38; }
  .meter.clip b { background:#ff3b3b; }

  /* Choosing between the instruments of a split. A sheet, not a dropdown: this
     is hit with a thumb on a stand. */
  .sheet { position:fixed; inset:0; background:#000a; display:flex; align-items:flex-end;
           justify-content:center; z-index:10; padding:16px;
           padding-bottom:calc(16px + env(safe-area-inset-bottom)); }
  .sheet .card { background:var(--panel); border-radius:16px; padding:14px; width:100%;
                 max-width:420px; display:flex; flex-direction:column; gap:8px; }
  .sheet .sheet-title { color:var(--dim); font-size:12px; font-weight:700;
                        letter-spacing:.08em; text-transform:uppercase; padding:4px 4px 6px; }
  .sheet button { background:var(--row); color:#fff; border:0; border-radius:12px;
                  padding:16px; font-size:16px; font-weight:700; text-align:left; }
  .sheet button.cancel { background:none; color:var(--dim); text-align:center; font-size:15px; }
  .sheet .part { display:flex; align-items:center; gap:8px; background:var(--row); border-radius:12px; padding:10px 12px; }
  .sheet .part .who { flex:1; min-width:0; }
  .sheet .part .who b { display:block; font-size:16px; }
  .sheet .part .who span { display:block; color:var(--dim); font-size:13px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .sheet .part button { flex:none; padding:10px 14px; font-size:14px; background:var(--bg); }
  .sheet .part button.on { background:#a3282d; }
  .sheet .part.muted .who { opacity:.45; }
  .transport button.parts { background:var(--row); color:#fff; font-weight:700; }
  .transport button.click.on { background:var(--accent); color:#06121f; }
  .loading { color:#d8a657; font-size:14px; }
  .grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(150px,1fr)); gap:8px; }
  button.p { background:var(--row); color:#fff; border:0; border-radius:10px; padding:14px 12px; text-align:left;
             font-size:15px; display:flex; gap:10px; align-items:baseline; min-height:56px; }
  button.p .n { color:var(--dim); font-variant-numeric:tabular-nums; font-weight:700; font-size:13px; }
  button.p.on { background:var(--accent); color:#06121f; }
  button.p.on .n { color:#06121f; }
  .knobs { padding:4px 12px 10px; }
  .kslot { margin-top:8px; }
  .kname { color:var(--dim); font-size:11px; font-weight:700; letter-spacing:.06em; text-transform:uppercase; margin:0 0 4px; }
  .knob { display:grid; grid-template-columns:96px 1fr 64px; align-items:center; gap:10px; padding:4px 0; }
  .knob .kn { color:#fff; font-size:14px; font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .knob .kv { color:var(--dim); font-size:13px; text-align:right; font-variant-numeric:tabular-nums; }
  .knob input[type=range] { width:100%; height:34px; margin:0; accent-color:var(--accent); touch-action:pan-y; }
  /* Group filters. Only drawn when the setup actually uses groups, so someone
     who has not touched the feature sees exactly what they saw before. */
  /* Tempo lives in the header beside PANIC: it is worth a glance and a tap, not
     a band across the top of the screen. */
  .hdr { display:flex; align-items:center; gap:8px; }
  .hdr .bpm { font-size:15px; font-weight:800; color:var(--accent);
              font-variant-numeric:tabular-nums; white-space:nowrap; }
  .hdr .bpm i { font-size:10px; color:var(--dim); font-style:normal; font-weight:600; margin-left:2px; }
  #tap { background:var(--row); color:#fff; border:0; border-radius:8px;
         padding:9px 14px; font-size:13px; font-weight:800; letter-spacing:.04em; }
  #tap:active { background:var(--accent); color:#06121f; }

  /* Tabs: the set list is what you look at during a gig, the programs are
     what you look at while building one. */
  .tabs { display:flex; gap:8px; padding:10px 12px 0; }
  .tabs button { flex:1; background:var(--panel); color:var(--dim); border:0; border-radius:10px 10px 0 0;
                 padding:12px; font-size:14px; font-weight:800; letter-spacing:.04em; }
  .tabs button.on { background:var(--row); color:#fff; }

  /* The set list. The current song is the thing you must be able to read at a
     glance from a stand, so it is the one that is big. */
  .setwrap { margin:10px 12px 22px; }
  /* The backing track of the song in play: one big button, and where it is. */
  .transport { display:flex; align-items:center; gap:12px; background:var(--panel); border-radius:12px;
               padding:10px 12px; margin-bottom:12px; }
  .transport button { flex:none; border:0; border-radius:10px; padding:12px 20px; font-size:16px; font-weight:800;
                      background:var(--accent); color:#06121f; min-width:96px; }
  .transport button.running { background:#a3282d; color:#fff; }
  .sections { display:flex; flex-wrap:wrap; gap:8px; margin:-4px 0 12px; }
  .sections button { background:var(--row); color:#fff; border:2px solid transparent; border-radius:10px;
                     padding:10px 14px; font-size:15px; font-weight:700; }
  .sections button.on { background:var(--accent); color:#06121f; }
  .sections button.queued { border-color:var(--accent); }
  .sections button.go { background:#8a5a12; }
  .transport .tt small { display:block; font-size:10px; letter-spacing:.08em; font-weight:700; color:var(--dim); opacity:.8; }
  .transport .tt { flex:1; min-width:0; font-size:15px; color:var(--dim); font-variant-numeric:tabular-nums;
                   overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .setpick { display:flex; flex-wrap:wrap; gap:8px; margin-bottom:12px; }
  .setpick button { background:var(--row); color:var(--dim); border:0; border-radius:999px;
                    padding:10px 18px; font-size:13px; font-weight:700; }
  .setpick button.on { background:var(--accent); color:#06121f; }

  .song { display:flex; align-items:baseline; gap:12px; background:var(--panel);
          border-radius:12px; padding:14px 16px; margin-bottom:8px; width:100%;
          border:0; text-align:left; color:#fff; }
  .song .n { color:var(--dim); font-variant-numeric:tabular-nums; font-weight:700;
             font-size:14px; min-width:24px; }
  .song .t { font-size:18px; font-weight:700; flex:1; overflow:hidden;
             text-overflow:ellipsis; white-space:nowrap; }
  .song .bpm { color:var(--dim); font-size:13px; font-weight:700; }

  /* The one you are on. */
  .song.on { background:var(--accent); color:#06121f; padding:18px 16px; }
  .song.on .n, .song.on .bpm { color:#06121f; }
  .song.on .t { font-size:26px; font-weight:800; }
  .song.on .parts { color:#06121f; }
  .find { width:100%; box-sizing:border-box; margin:0 0 12px; padding:14px 16px;
          font-size:16px; border-radius:12px; border:1px solid #3a3d47;
          background:var(--panel); color:#fff; }
  .find:focus { outline:none; border-color:var(--accent); }
  .findrow { display:flex; gap:8px; align-items:stretch; margin-bottom:12px; }
  .findrow .find { margin:0; flex:1; }
  .findrow .editmode { flex:0 0 auto; background:var(--row); color:var(--dim); border:1px solid #3a3d47;
                       border-radius:12px; padding:0 18px; font-size:15px; font-weight:700; }
  .findrow .editmode.on { background:var(--accent); color:#06121f; border-color:var(--accent); }
  .setpick button.new { border:1px dashed #3a3d47; background:none; }
  .editbar { display:flex; gap:8px; margin-bottom:12px; }
  .editbar button { flex:1; background:var(--row); color:#fff; border:0; border-radius:12px; padding:12px; font-size:15px; font-weight:700; }
  .song .ctl { display:flex; gap:6px; flex:0 0 auto; align-self:center; }
  .song .ctl span { padding:10px 14px; border-radius:10px; background:#2b2d35; color:#d7dbe2; font-size:15px; font-weight:700; }
  .song .ctl span.off { opacity:.3; }
  .song.on .ctl span { background:#06121f; color:#fff; }
  .otherhead { color:var(--dim); font-size:11px; font-weight:800; letter-spacing:.1em;
               margin:18px 0 8px; }
  .parts { font-size:13px; color:var(--dim); margin-top:4px; }
  .notes { font-size:14px; color:#d8a657; margin-top:4px; font-style:italic; }
  .notes .key { font-style:normal; font-weight:700; padding:3px 9px; border-radius:8px; background:#d8a65722;
                border:1px solid #d8a65766; cursor:pointer; }
  .song.on .notes .key { background:#06121f1a; border-color:#06121f66; }
  .sheet .keys { display:grid; grid-template-columns:repeat(7, 1fr); gap:6px; }
  .sheet .scales { display:grid; grid-template-columns:repeat(2, 1fr); gap:6px; margin-top:6px; }
  .sheet .keys button, .sheet .scales button { padding:12px 6px; text-align:center; font-size:15px; }
  .sheet .keys button.none { grid-column: span 2; }
  .sheet .keys button.on, .sheet .scales button.on { background:var(--accent); color:#06121f; }
  .song .chart { margin-left:10px; padding:8px 14px; border-radius:10px; background:#2b2d35; color:#d7dbe2;
                 font-size:14px; font-weight:600; flex:0 0 auto; }
  .song.on .chart { background:#06121f; color:#fff; }
  .song.on .notes { color:#06121f; }
  .songbody { flex:1; min-width:0; }

  /* Step buttons: big, far apart, hard to hit the wrong one. */
  .step { display:flex; gap:10px; margin-top:14px; }
  .step button { flex:1; background:var(--row); color:#fff; border:0; border-radius:12px;
                 padding:20px; font-size:17px; font-weight:800; letter-spacing:.04em; }
  .step button:active { background:var(--accent); color:#06121f; }
  .step button:disabled { opacity:.35; }

  .empty { color:var(--dim); font-size:15px; padding:20px 4px; line-height:1.5; }

  .groups { display:flex; flex-wrap:wrap; gap:8px; margin:0 0 10px; }
  .groups button { background:var(--row); color:var(--dim); border:0; border-radius:999px;
                   padding:9px 16px; font-size:13px; font-weight:700; letter-spacing:.02em; }
  .groups button.on { background:var(--accent); color:#06121f; }

  #err { display:none; background:#a3282d; padding:10px 16px; font-size:14px; }
  #gate { display:none; align-items:center; justify-content:center; min-height:70vh; padding:20px; }
  #gate form { text-align:center; max-width:320px; width:100%; }
  #gate p { color:var(--dim); font-size:15px; margin:0 0 14px; }
  #gate .hint { font-size:13px; color:#d8a657; margin-top:14px; min-height:18px; }
  #code { width:100%; font-size:34px; text-align:center; letter-spacing:.25em; text-transform:uppercase;
          padding:14px; border-radius:12px; border:2px solid #3a3d47; background:var(--panel); color:#fff; }
  #code:focus { outline:none; border-color:var(--accent); }
  #gate button { margin-top:14px; width:100%; background:var(--accent); color:#06121f; border:0;
                 border-radius:12px; padding:16px; font-size:17px; font-weight:700; }
</style></head>
<body>
<header>
  <h1>STAGERIG</h1>
  <div class="hdr">
    <span class="bpm" id="bpm">…<i>bpm</i></span>
    <button id="tap">TAP TEMPO</button>
    <button id="panic">PANIC</button>
  </div>
</header>
<div id="err"></div>
<div id="gate">
  <form id="codeform">
    <p>Enter the 6-character code from the <b>Tablet control</b> window on the computer</p>
    <input id="code" inputmode="latin" autocapitalize="characters" autocomplete="off" spellcheck="false" maxlength="6" placeholder="6 characters">
    <button type="submit">Connect to StageRig</button>
    <p class="hint" id="gatemsg"></p>
  </form>
</div>
<div class="tabs">
  <button id="tabSongs" class="on">Set list</button>
  <button id="tabProgs">Programs</button>
</div>
<div id="setview"></div>
<div id="inputs"></div>
</div>
<script>
/* Tapping is timed on the server, where the tempo lives: a few milliseconds of
   network on a local wifi is far below the precision of a finger, and doing the
   averaging in two places would let them disagree. */
document.getElementById("tap").onclick = async () => {
  try {
    const r = await api("/api/tap", { method: "POST" });
    if (r && r.tempo) showTempo(r.tempo);
  } catch (e) { if (!e.gate) show(e.message); }
};

function showTempo(bpm) {
  document.getElementById("bpm").innerHTML =
    (Math.round(bpm * 10) / 10).toFixed(1) + '<i>bpm</i>';
}


let token = new URLSearchParams(location.search).get("t") || localStorage.getItem("t") || "";
if (token) localStorage.setItem("t", token);
let rev = -1;

// Asking for the code beats putting it in the URL: you can add this page to the home
// screen once and type the six characters shown by StageRig, rather than re-scanning
// a link every time the app restarts.
function askForCode(message) {
  document.getElementById("gate").style.display = "flex";
  document.getElementById("gatemsg").textContent = message || "";
  for (const id of ["inputs", "setview"]) document.getElementById(id).style.display = "none";
  document.querySelector(".tabs").style.display = "none";
  document.querySelector(".hdr").style.display = "none";
  const f = document.getElementById("codeform");
  f.onsubmit = (e) => {
    e.preventDefault();
    const v = document.getElementById("code").value.trim().toUpperCase();
    if (!v) return;
    token = v; localStorage.setItem("t", v);
    document.getElementById("gate").style.display = "none";
    document.querySelector(".tabs").style.display = "";
    document.querySelector(".hdr").style.display = "";
    syncTabs();
    refresh(true);
  };
  document.getElementById("code").focus();
}

async function api(path, opts) {
  let r;
  try { r = await fetch(path + (path.includes("?") ? "&" : "?") + "t=" + encodeURIComponent(token), opts); }
  catch (e) { throw new Error("Can't reach StageRig on the computer. Is this tablet on the same wifi?"); }
  if (r.status === 403) { localStorage.removeItem("t"); const e = new Error("gate"); e.gate = true; throw e; }
  if (!r.ok) {
    let text = "";
    try { const j = await r.json(); text = j && j.error ? j.error : ""; } catch (e) {}
    throw new Error(text || ("StageRig answered with an error (" + r.status + "). Try again."));
  }
  return r.json();
}
/* A message stays long enough to be read -- an error six seconds, a
   confirmation two -- rather than being wiped by the next poll. */
let showTimer = null;
function show(msg, ms) {
  const e = document.getElementById("err"); e.textContent = msg; e.style.display = msg ? "block" : "none";
  if (showTimer) { clearTimeout(showTimer); showTimer = null; }
  if (msg) showTimer = setTimeout(() => { e.style.display = "none"; showTimer = null; }, ms || 6000);
}

let lastState = null;
let editMode = false;        // the Set list tab in its editing dress: build and reorder set lists from the stand
const filters = {};          // input index -> chosen group, "" for all

/* Which tab is showing. Remembered per device: someone building a set works in
   Programs, someone playing a gig works in the set list, and neither should
   have to re-pick it every time the page reloads. */
let tab = localStorage.getItem("tab") || "songs";
/* ?tab=songs or ?tab=programs opens on that page: a home-screen shortcut
   straight to the one a player always wants. */
{ const want = new URLSearchParams(location.search).get("tab"); if (want === "songs" || want === "programs") { tab = want; localStorage.setItem("tab", tab); } }
if (tab !== "songs" && tab !== "programs") tab = "songs";
let songFilter = "";          // what is typed in the song search, if anything

function syncTabs() {
  const t = tab;
  document.getElementById("tabSongs").className = t === "songs" ? "on" : "";
  document.getElementById("tabProgs").className = t === "programs" ? "on" : "";
  document.getElementById("setview").style.display = t === "songs" ? "" : "none";
  document.getElementById("inputs").style.display  = t === "programs" ? "" : "none";
}

document.getElementById("tabSongs").onclick = () => { tab = "songs";    localStorage.setItem("tab", tab); syncTabs(); };
document.getElementById("tabProgs").onclick = () => { tab = "programs"; localStorage.setItem("tab", tab); syncTabs(); };

/* The backing track's button and clock, kept current twice a second while the
   song in play has a track and the page is showing. */
let transportUi = null, transportTimer = null;
const mmss = s => { s = Math.max(0, Math.floor(s || 0)); return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); };
function showTransport(t) {
  if (!transportUi || !t) return;
  transportUi.button.textContent = t.running ? "Stop" : "Play";
  transportUi.button.classList.toggle("running", !!t.running);
  if (transportUi.clickBtn) transportUi.clickBtn.classList.toggle("on", !!t.click);
  const sec = (t.sections || [])[t.section];
  const status = t.scenes ? (sec ? sec.name + " · " + (t.section + 1) + "/" + t.sections.length : t.name)
    : !t.ready ? ("Loading " + Math.round((t.loading || 0) * 100) + "%")
    : t.countIn ? "Counting in…"
    : ((t.sections || []).length > 1 && sec ? sec.name + (t.vamping ? " — repeating (vamp)" : "") : t.name) + " · "
      + (t.metronome ? "click " + mmss(t.pos) : mmss(t.pos) + " / " + mmss(t.len))
      + (t.queued >= 0 && t.sections[t.queued] ? " · next: " + t.sections[t.queued].name : "");
  transportUi.text.innerHTML = '<small></small><span></span>';
  transportUi.text.querySelector("small").textContent = t.scenes ? "SCENES" : "BACKING TRACK";
  transportUi.text.querySelector("span").textContent = status;

  /* Section buttons, redrawn only when something about them changes: a tap
     queues that section; during a vamp, "Go on" lets it play through. */
  const list = t.sections || [];
  const key = JSON.stringify([list.map(x => x.name + x.vamp), t.section, t.queued, t.vamping, t.running, t.scenes && editMode]);
  transportUi.parts = t.parts || [];
  transportUi.partsBtn.style.display = transportUi.parts.length ? "" : "none";
  if (partsSheet) partsSheet.refresh();

  if (key === transportUi.key) return;
  transportUi.key = key;
  const host = transportUi.sections;
  host.innerHTML = "";
  if (transportUi.caption) transportUi.caption.style.display = list.length < (t.scenes ? 1 : 2) ? "none" : "";
  if (t.scenes && editMode && lastState && lastState.canEdit !== false) {
    /* Building a song from the stand: set the sounds, add a scene; set them
       again, add another. Capture refreshes the scene in play. */
    const add = document.createElement("button"); add.className = "go"; add.textContent = "+ Add scene";
    add.onclick = async () => { try { showTransport(await api("/api/scene?do=add", { method: "POST" })); await refresh(true); } catch (e) { if (!e.gate) show(e.message); } };
    host.appendChild(add);
    if (list.length) {
      const cap = document.createElement("button"); cap.className = "go"; cap.textContent = "Capture this scene";
      cap.onclick = async () => { try { showTransport(await api("/api/scene?do=capture", { method: "POST" })); show("Scene captured: it now holds what is loaded.", 2000); } catch (e) { if (!e.gate) show(e.message); } };
      host.appendChild(cap);
    }
  }
  if (list.length < (t.scenes ? 1 : 2)) return;
  if (t.vamping) {
    const go = document.createElement("button"); go.className = "go"; go.textContent = "End vamp ▸";
    go.onclick = async () => { try { showTransport(await api("/api/transport?do=next", { method: "POST" })); } catch (e) { if (!e.gate) show(e.message); } };
    host.appendChild(go);
  }
  list.forEach((x, i) => {
    const b = document.createElement("button");
    b.textContent = x.name + (x.vamp ? " ↻ vamp" : "") + (i === t.queued && i !== t.section ? " · next" : "");
    if (i === t.section) b.className = "on";
    else if (i === t.queued) b.className = "queued";
    b.onclick = async () => {
      try { showTransport(await api("/api/transport?do=section&index=" + i, { method: "POST" })); }
      catch (e) { if (!e.gate) show(e.message); }
    };
    host.appendChild(b);
  });
}
/* The MIDI files' parts, from the stand: mute one, or open the plugin that
   plays it. What plays each part is chosen in StageRig; here it is read. */
let partsSheet = null;
function showParts() {
  if (!transportUi || !transportUi.parts.length) return;
  const back = document.createElement("div"); back.className = "sheet";
  const card = document.createElement("div"); card.className = "card";
  const t = document.createElement("div"); t.className = "sheet-title"; t.textContent = "Backing track parts — mute one to play it yourself";
  card.appendChild(t);
  const rows = document.createElement("div"); rows.style.display = "flex"; rows.style.flexDirection = "column"; rows.style.gap = "8px";
  card.appendChild(rows);
  const cancel = document.createElement("button"); cancel.className = "cancel"; cancel.textContent = "Done";
  cancel.onclick = () => { back.remove(); partsSheet = null; };
  card.appendChild(cancel);
  back.appendChild(card);
  back.onclick = e => { if (e.target === back) { back.remove(); partsSheet = null; } };
  document.body.appendChild(back);

  const refresh = () => {
    rows.innerHTML = "";
    (transportUi ? transportUi.parts : []).forEach(p => {
      const row = document.createElement("div"); row.className = "part" + (p.muted ? " muted" : "");
      const who = document.createElement("div"); who.className = "who";
      who.innerHTML = "<b></b><span></span>";
      who.querySelector("b").textContent = p.name;
      who.querySelector("span").textContent = "Plays on: " + (p.plays && p.plays !== "Nothing" ? p.plays : "nothing yet (choose it in the song's Backing track window on the computer)");
      row.appendChild(who);
      if (p.slot >= 0) {
        const g = document.createElement("button"); g.textContent = "GUI";
        g.onclick = () => openPluginWindow(p.input, p.slot, -1, p.name, g, "GUI", -1);
        row.appendChild(g);
      }
      const m = document.createElement("button"); m.textContent = p.muted ? "Unmute" : "Mute"; if (p.muted) m.className = "on";
      m.onclick = async () => {
        try { showTransport(await api("/api/part?ch=" + p.ch + "&muted=" + (p.muted ? 0 : 1), { method: "POST" })); }
        catch (e) { if (!e.gate) show(e.message); }
      };
      row.appendChild(m);
      rows.appendChild(row);
    });
  };
  partsSheet = { refresh };
  refresh();
}

/* The song's key, picked with a thumb: a root, a scale, or none. Each tap
   is saved at once and the sheet redraws from the state that comes back. */
let keySheet = null;
function showKeyPicker(songIndex) {
  if (keySheet) { keySheet.back.remove(); keySheet = null; }
  const back = document.createElement("div"); back.className = "sheet";
  const card = document.createElement("div"); card.className = "card";
  const t = document.createElement("div"); t.className = "sheet-title";
  card.appendChild(t);
  const roots = document.createElement("div"); roots.className = "keys";
  const scales = document.createElement("div"); scales.className = "scales";
  card.appendChild(roots); card.appendChild(scales);
  const cancel = document.createElement("button"); cancel.className = "cancel"; cancel.textContent = "Done";
  const close = () => { back.remove(); keySheet = null; };
  cancel.onclick = close;
  card.appendChild(cancel);
  back.appendChild(card);
  back.onclick = e => { if (e.target === back) close(); };
  document.body.appendChild(back);

  const set = async (root, mode) => {
    try { await api("/api/songkey?song=" + songIndex + "&root=" + root + "&mode=" + mode, { method: "POST" }); await refresh(true); }
    catch (e) { if (!e.gate) show(e.message); }
  };
  const draw = () => {
    const s = lastState; const sg = s && s.songs && s.songs[songIndex];
    if (!sg) { close(); return; }
    const root = sg.key ? sg.keyRoot : -1, mode = sg.key ? sg.keyMode : 0;
    t.textContent = "Key of " + (sg.name || "this song") + (sg.key ? " \u2014 " + sg.key : " \u2014 none");
    roots.innerHTML = ""; scales.innerHTML = "";
    (s.roots || []).forEach((name, i) => {
      const b = document.createElement("button"); b.textContent = name; if (i === root) b.className = "on";
      b.onclick = () => set(i, mode);
      roots.appendChild(b);
    });
    const none = document.createElement("button"); none.textContent = "None"; none.className = "none" + (root < 0 ? " on" : "");
    none.title = "Each Scale and Run keeps its own key; a chart is shown as written";
    none.onclick = () => set(-1, mode);
    roots.appendChild(none);
    (s.scales || []).forEach((name, i) => {
      const b = document.createElement("button"); b.textContent = name; if (root >= 0 && i === mode) b.className = "on";
      b.onclick = () => set(root >= 0 ? root : 0, i);
      scales.appendChild(b);
    });
  };
  keySheet = { back, draw };
  draw();
}

async function pollTransport() {
  try { showTransport(await api("/api/transport")); } catch (e) { /* the next poll is half a second away */ }
}
function syncTransportPolling() {
  const want = token && lastState && (lastState.track || lastState.scenes) && document.visibilityState === "visible";
  if (want && !transportTimer) { transportTimer = setInterval(pollTransport, 500); pollTransport(); }
  if (!want && transportTimer) { clearInterval(transportTimer); transportTimer = null; }
}
document.addEventListener("visibilitychange", syncTransportPolling);

/* The set list. Drawn from the state the page already polls, so it follows a
   footswitch press or a change made on the laptop without doing anything. */
function renderSets(s) {
  const host = document.getElementById("setview");
  host.innerHTML = "";
  const sets = s.sets || [], songs = s.songs || [];
  if (!sets.length) {
    const e = document.createElement("div"); e.className = "empty";
    e.textContent = "No set list yet. On the computer: capture each song (its programs and tempo) with the + button under SONGS, then put the songs in order in a set list. They appear here.";
    host.appendChild(e);
    return;
  }

  const wrap = document.createElement("div"); wrap.className = "setwrap";

  if (s.track || s.scenes) {
    const bar = document.createElement("div"); bar.className = "transport";
    const b = document.createElement("button"); b.textContent = "Play";
    const t = document.createElement("div"); t.className = "tt"; t.textContent = s.scenes ? "Scenes" : "Backing track";
    const cap = document.createElement("div"); cap.className = "empty"; cap.style.padding = "0 4px 6px"; cap.style.fontSize = "12px";
    cap.textContent = s.scenes ? "Tap a scene and the keyboards change at once."
                               : "Tap a section to go there when this one ends. ↻ repeats until you end the vamp.";
    if (s.scenes) b.style.display = "none";
    b.onclick = async () => {
      try { showTransport(await api("/api/transport?do=toggle", { method: "POST" })); }
      catch (e) { if (!e.gate) show(e.message); }
    };
    const parts = document.createElement("button"); parts.className = "parts"; parts.textContent = "Parts of the track";
    parts.style.display = "none";
    parts.onclick = showParts;
    const click = document.createElement("button"); click.className = "parts click"; click.textContent = "Click";
    click.title = "The metronome in the in-ears, through the song";
    click.onclick = async () => {
      try { showTransport(await api("/api/transport?do=click", { method: "POST" })); }
      catch (e) { if (!e.gate) show(e.message); }
    };
    if (s.scenes) click.style.display = "none";
    bar.appendChild(b); bar.appendChild(t); bar.appendChild(click); bar.appendChild(parts);
    wrap.appendChild(bar);
    const secs = document.createElement("div"); secs.className = "sections";
    wrap.appendChild(secs);
    wrap.appendChild(cap);
    transportUi = { button: b, text: t, sections: secs, partsBtn: parts, clickBtn: click, parts: [], key: "", caption: cap };
  } else {
    transportUi = null;
  }

  /* Search, whenever there is more than one song. It used to appear only past
     eight, which meant a five-song setup had a feature it could not discover --
     and a set list you cannot find a song in is the one case where you need to.
     Kept across redraws, because the page repaints every second and a filter
     that cleared itself while you were reading would be worse than none. */
  {
    const row = document.createElement("div"); row.className = "findrow";
    if (songs.length > 1) {
      const box = document.createElement("input");
      box.className = "find"; box.type = "search";
      box.placeholder = editMode ? "Find a song to add" : "Find a song";
      box.value = songFilter;
      box.oninput = () => { songFilter = box.value; renderSets(lastState); };
      row.appendChild(box);
      /* Put the caret back where it was: this runs inside a full redraw. */
      if (songFilter) { box.focus(); box.setSelectionRange(box.value.length, box.value.length); }
    }
    /* Editing the set lists from the stand: add, take out, reorder, make and
       name them. A mode rather than buttons on every row, so a row tapped in
       the heat of a gig only ever starts a song. */
    if (s.canEdit === false) editMode = false;                 // the computer keeps the stand from editing
    const ed = document.createElement("button"); ed.className = "editmode" + (editMode ? " on" : "");
    ed.textContent = editMode ? "Done" : "Edit";
    ed.title = editMode ? "Back to playing" : "Build or change set lists and scenes";
    ed.onclick = () => { editMode = !editMode; renderSets(lastState); if (transportUi) pollTransport(); };
    if (s.canEdit === false) ed.style.display = "none";
    row.appendChild(ed);
    wrap.appendChild(row);
  }

  const matches = (sg) => {
    const q = songFilter.trim().toLowerCase();
    if (!q) return true;
    if ((sg.name || "").toLowerCase().includes(q)) return true;
    if ((sg.notes || "").toLowerCase().includes(q)) return true;
    // The programs too: "kontakt" or "002" is how you find a song you cannot name.
    return (sg.parts || []).some(x =>
      (x.name || "").toLowerCase().includes(q)
      || String(x.program).padStart(3, "0").includes(q)
      || (x.input || "").toLowerCase().includes(q));
  };

  const post = async (path) => {
    try { await api(path, { method: "POST" }); await refresh(true); }
    catch (e) { if (!e.gate) show(e.message); }
  };
  const setEdit = (q) => post("/api/setedit?" + q);

  /* Which set list: a row of them when there is more than one, or none is
     chosen yet, or the lists are being edited. Editing lives here too: a new
     list, and for the one chosen a rename, a copy and a delete. */
  if (editMode || sets.length > 1 || !sets[s.currentSet]) {
    const pick = document.createElement("div"); pick.className = "setpick";
    sets.forEach((st, i) => {
      const b = document.createElement("button");
      b.textContent = st.name || ("Set list " + (i + 1));
      if (i === s.currentSet) b.className = "on";
      b.onclick = () => post("/api/setlist?index=" + i);
      pick.appendChild(b);
    });
    if (editMode) {
      const nb = document.createElement("button"); nb.className = "new"; nb.textContent = "+ New set list";
      nb.onclick = () => { const name = prompt("Name for the new set list", "Set list " + (sets.length + 1)); if (name !== null) setEdit("do=new&name=" + encodeURIComponent(name.trim())); };
      pick.appendChild(nb);
    }
    wrap.appendChild(pick);
  }
  if (editMode && sets[s.currentSet]) {
    const cur = sets[s.currentSet], i = s.currentSet, curName = cur.name || ("Set list " + (i + 1));
    const bar = document.createElement("div"); bar.className = "editbar";
    const mk = (label, fn) => { const b = document.createElement("button"); b.textContent = label; b.onclick = fn; bar.appendChild(b); };
    mk("Rename", () => { const name = prompt("Name for this set list", curName); if (name !== null && name.trim()) setEdit("do=rename&index=" + i + "&name=" + encodeURIComponent(name.trim())); });
    mk("Duplicate", () => { const name = prompt("Name for the copy", curName + " copy"); if (name !== null) setEdit("do=duplicate&index=" + i + "&name=" + encodeURIComponent(name.trim())); });
    mk("Delete", () => { if (confirm("Delete the set list \u201c" + curName + "\u201d? The songs themselves are kept.")) setEdit("do=delete&index=" + i); });
    wrap.appendChild(bar);
  }

  const cur = sets[s.currentSet];

  /* One row, used for the set and for the songs outside it. `pos` is the
     position within the set, or -1 for a song that is not in it. */
  const songRow = (sg, songIndex, pos) => {
    const b = document.createElement("button");
    /* The song in play lights up wherever it is: at its position in the set,
       or among the songs outside it when it was tapped there. */
    const playing = songIndex === s.playingSong && (pos < 0 || pos === s.currentSong);
    b.className = "song" + (playing ? " on" : "");

    if (pos >= 0) {
      const n = document.createElement("span"); n.className = "n";
      n.textContent = String(pos + 1);
      b.appendChild(n);
    }

    const body = document.createElement("div"); body.className = "songbody";
    const t = document.createElement("div"); t.className = "t";
    t.textContent = sg.name || "Untitled";
    body.appendChild(t);

    /* What each keyboard plays: on the song being played, and on any song not
       in the set -- there the programs are the only way to tell two songs with
       the same name apart, which a list of captures tends to produce. */
    if (sg.parts && sg.parts.length) {
      const pl = document.createElement("div"); pl.className = "parts";
      pl.textContent = sg.parts.map(x => x.input + ": " + String(x.program).padStart(3, "0")
                                      + (x.name ? " " + x.name : "")).join("  \u00b7  ");
      body.appendChild(pl);
    }
    /* The key, and the stage notes. The key is a chip: tap it to change the
       song's key from the stand -- the Scale and Run effects and the chart
       follow at once -- without picking the song again. */
    if (playing) {
      const nt = document.createElement("div"); nt.className = "notes";
      const k = document.createElement("span"); k.className = "key";
      k.textContent = sg.key ? sg.key : "Set the key";
      k.title = "The song's key: the Scale and Run effects play in it, a chord chart is shown in it";
      k.onclick = (ev) => { ev.stopPropagation(); showKeyPicker(songIndex); };
      nt.appendChild(k);
      if (sg.notes) nt.appendChild(document.createTextNode("  \u00b7  " + sg.notes));
      body.appendChild(nt);
    }

    b.appendChild(body);
    if (editMode) {
      /* Up, down and out for a song in the set; Add for one outside it. Each
         is one call, saved at once, so there is nothing to confirm or lose. */
      const ctl = document.createElement("span"); ctl.className = "ctl";
      const mk = (label, title, q, off) => {
        const c = document.createElement("span"); c.textContent = label; c.title = title;
        if (off) c.className = "off"; else c.onclick = (ev) => { ev.stopPropagation(); setEdit(q); };
        ctl.appendChild(c);
      };
      const i = s.currentSet;
      if (pos >= 0) {
        mk("\u25b2", "Earlier", "do=move&index=" + i + "&pos=" + pos + "&delta=-1", pos === 0);
        mk("\u25bc", "Later", "do=move&index=" + i + "&pos=" + pos + "&delta=1", pos >= cur.songs.length - 1);
        mk("\u2715", "Take out of the set list (the song is kept)", "do=remove&index=" + i + "&pos=" + pos, false);
      } else {
        mk("+ Add", "Put it on the end of the set list", "do=add&index=" + i + "&song=" + songIndex, !sets[i]);
      }
      b.appendChild(ctl);
      b.onclick = null;
      return b;
    }
    if (sg.tempo) {
      const bpm = document.createElement("span"); bpm.className = "bpm";
      bpm.textContent = Math.round(sg.tempo) + " bpm";
      b.appendChild(bpm);
    }
    /* Sheet music, in its own tab that follows the set from then on. A tap
       here must not also pick the song: that is the row's job. */
    if (sg.charts && sg.charts.length) {
      const ch = document.createElement("span"); ch.className = "chart";
      ch.textContent = "Chart";
      ch.onclick = (ev) => {
        ev.stopPropagation();
        const w = window.open("/chart.html?song=" + songIndex + "&t=" + encodeURIComponent(token), "stagerig-chart");
        if (w) w.focus();
      };
      b.appendChild(ch);
    }

    b.onclick = async () => {
      /* In the set, start there, so the footswitches step from here. Outside
         it, just load the song. */
      const path = pos >= 0 ? "/api/setsong?pos=" + pos : "/api/song?index=" + songIndex;
      try { await api(path, { method: "POST" }); await refresh(true); }
      catch (e) { if (!e.gate) show(e.message); }
    };
    return b;
  };

  if (!cur) {
    const e = document.createElement("div"); e.className = "empty";
    e.textContent = editMode ? "Make a set list above, or choose one." : "Choose a set list above.";
    wrap.appendChild(e); host.appendChild(wrap); return;
  }

  let shownInSet = 0;
  cur.songs.forEach((songIndex, pos) => {
    const sg = songs[songIndex];
    if (!sg) return;                       // a set pointing at a song that has gone
    if (!matches(sg)) return;
    wrap.appendChild(songRow(sg, songIndex, pos));
    ++shownInSet;
  });
  if (!shownInSet && songFilter.trim()) {
    const e = document.createElement("div"); e.className = "empty";
    e.textContent = "No song in this set list matches “" + songFilter.trim() + "”.";
    wrap.appendChild(e);
  }

  // Step buttons, matching the footswitches.
  const step = document.createElement("div"); step.className = "step";
  const mk = (label, dir, disabled) => {
    const b = document.createElement("button");
    b.textContent = label; b.disabled = disabled;
    b.onclick = async () => {
      try { await api("/api/step?dir=" + dir, { method: "POST" }); await refresh(true); }
      catch (e) { if (!e.gate) show(e.message); }
    };
    return b;
  };
  const last = cur.songs.length - 1;
  step.appendChild(mk("← PREV SONG", -1, s.currentSong <= 0));
  step.appendChild(mk("NEXT SONG →", 1, s.currentSong >= last));
  if (!editMode) wrap.appendChild(step);

  /* Everything not in this set, underneath. A song you have captured but not
     put in a set is still a song you may want to play -- and before this it
     was invisible from the stand, which looked like the app had lost it. */
  const inSet = new Set(cur.songs);
  const others = songs.map((sg, i) => [sg, i])
                      .filter(([sg, i]) => !inSet.has(i) && matches(sg))
                      .sort((a, b) => (a[0].name || "").localeCompare(b[0].name || "", undefined, { sensitivity: "base" }));
  if (others.length) {
    const h = document.createElement("div"); h.className = "otherhead";
    h.textContent = editMode ? "SONGS TO ADD  (" + others.length + ", A TO Z)" : "SONGS NOT IN THIS SET LIST";
    wrap.appendChild(h);
    others.forEach(([sg, i]) => wrap.appendChild(songRow(sg, i, -1)));
  }

  const where = document.createElement("div"); where.className = "empty";
  where.style.padding = "12px 4px 0";
  const outside = s.playingSong >= 0 && !inSet.has(s.playingSong) && songs[s.playingSong];
  where.textContent = outside
      ? "Playing \u201c" + (songs[s.playingSong].name || "Untitled") + "\u201d, outside the set list"
        + (s.currentSong >= 0 ? " \u2014 Next goes on from song " + (s.currentSong + 2) : "")
      : s.currentSong >= 0
      ? "Song " + (s.currentSong + 1) + " of " + cur.songs.length
      : cur.songs.length + (cur.songs.length === 1 ? " song" : " songs") + " in this set list — tap one to start";
  wrap.appendChild(where);

  host.appendChild(wrap);
}

/* The audio inputs' meters. Polled only while the page is showing one, ten
   times a second: a meter that lags is no meter, and a page in the background
   has nothing to show it to. The bar's gradient is fixed to the full width,
   so the colour at the end is the colour of that level. */
const meters = {};
let levelTimer = null;
function syncLevelPolling() {
  const want = token && lastState && lastState.inputs && lastState.inputs.some(x => x.audio)
            && document.visibilityState === "visible";
  if (want && !levelTimer) levelTimer = setInterval(pollLevels, 100);
  if (!want && levelTimer) { clearInterval(levelTimer); levelTimer = null; }
}
function meterPct(v) {
  if (!(v > 0.001)) return 0;
  const db = 20 * Math.log10(v);
  return Math.max(0, Math.min(100, (db + 60) / 60 * 100));
}
async function pollLevels() {
  try {
    const r = await api("/api/levels");
    (r.levels || []).forEach(l => {
      const m = meters[l.input]; if (!m) return;
      const lp = meterPct(l.left), rp = meterPct(l.right);
      m.l.style.width = lp + "%"; m.l.style.backgroundSize = (lp > 0 ? 10000 / lp : 100) + "% 100%";
      m.r.style.width = rp + "%"; m.r.style.backgroundSize = (rp > 0 ? 10000 / rp : 100) + "% 100%";
      m.root.classList.toggle("clip", !!l.clip);
    });
  } catch (e) { /* a missed poll is nothing; the next one is 100 ms away */ }
}
document.addEventListener("visibilitychange", syncLevelPolling);

function render(s) {
  lastState = s;
  for (const k in meters) delete meters[k];
  if (s.tempo) showTempo(s.tempo);
  const root = document.getElementById("inputs");
  root.innerHTML = "";
  {
    const cap = document.createElement("div"); cap.className = "empty"; cap.style.padding = "10px 16px 0"; cap.style.fontSize = "13px";
    cap.textContent = "Each keyboard's current program is highlighted. Tap another to switch to it now; the number is its program change. GUI opens the plugin's own window on this tablet.";
    root.appendChild(cap);
  }
  if (!s.inputs.some(inp => !inp.track)) {
    const e = document.createElement("div"); e.className = "empty";
    e.textContent = "No keyboards or audio inputs are set up on the computer yet.";
    root.appendChild(e);
  }
  s.inputs.forEach((inp, i) => {
    if (inp.track) return;
    const wrap = document.createElement("div"); wrap.className = "input";
    const now = document.createElement("div"); now.className = "now";
    now.innerHTML = '<div class="name"></div><div class="prog"><span class="num"></span><span class="title"></span></div>';
    now.querySelector(".name").textContent = inp.name.toUpperCase();
    now.querySelector(".num").textContent = String(inp.current).padStart(3, "0");
    /* An audio input with nothing in the program is not empty: the signal goes straight through. */
    now.querySelector(".title").textContent = inp.currentName || (inp.audio ? "Audio passes straight through (no effects)" : "Empty program — nothing loaded");

    /* Straight to the plugin's interface from the input's own header. This is
       the thing you reach for mid-set -- the slot rows below are further down
       the page, and on a tablet on a stand that is a scroll you do not want to
       be doing between songs.

       One slot opens it directly; several offer a choice, because a split with
       an organ over a pad has two interfaces and only you know which one you
       meant. Hidden entirely when the program has no plugins to show. */
    const gui = document.createElement("button");
    gui.className = "gui"; gui.textContent = "GUI";
    gui.style.display = "none";
    gui.onclick = () => {
      const list = slotCache[i] || [];
      if (!list.length) return;
      if (list.length === 1) { openPluginWindow(i, list[0].slot, list[0].effect, list[0].name, gui, "GUI", list[0].bus); return; }
      pickSlot(i, list, gui);
    };
    now.querySelector(".prog").appendChild(gui);
    guiButtons[i] = gui;
    syncGuiButton(i);

    if (inp.loading) { const l = document.createElement("div"); l.className = "loading"; l.textContent = "Loading plugins…"; now.appendChild(l); }
    if (inp.audio) {
      const m = document.createElement("div"); m.className = "meter";
      m.innerHTML = '<div class="bars"><i><span></span></i><i><span></span></i></div><b></b>';
      now.appendChild(m);
      meters[i] = { root: m, l: m.querySelectorAll("span")[0], r: m.querySelectorAll("span")[1] };
    }
    wrap.appendChild(now);
    /* Which group is being shown, per input. Kept across refreshes so a poll
       does not throw you back to "All" while you are looking for a sound. */
    const groups = [];
    inp.programs.forEach(p => { if (p.group && !groups.includes(p.group)) groups.push(p.group); });

    if (groups.length) {
      const bar = document.createElement("div"); bar.className = "groups";
      const mk = (label, value) => {
        const b = document.createElement("button");
        b.textContent = label;
        if ((filters[i] || "") === value) b.className = "on";
        b.onclick = () => { filters[i] = value; render(lastState); };
        bar.appendChild(b);
      };
      mk("All", "");
      groups.forEach(gname => mk(gname, gname));
      if (inp.programs.some(p => !p.group)) mk("Ungrouped", "\u0000other");
      wrap.appendChild(bar);
    }

    const active = filters[i] || "";
    const grid = document.createElement("div"); grid.className = "grid";
    inp.programs.forEach(p => {
      if (active === "\u0000other") { if (p.group) return; }
      else if (active && p.group !== active) return;
      const b = document.createElement("button");
      b.className = "p" + (p.index === inp.current ? " on" : "");
      b.innerHTML = '<span class="n"></span><span class="t"></span>';
      b.querySelector(".n").textContent = String(p.index).padStart(3, "0");
      b.querySelector(".t").textContent = p.name || "Untitled";
      b.onclick = async () => {
        try { await api("/api/select?input=" + i + "&program=" + p.index, { method: "POST" }); await refresh(true); show(""); }
        catch (e) { show(e.message); }
      };
      grid.appendChild(b);
    });
    wrap.appendChild(grid);

    const knobs = document.createElement("div"); knobs.className = "knobs"; knobs.dataset.input = i;
    wrap.appendChild(knobs);
    root.appendChild(wrap);
    loadSlots(i);
    loadPanel(i);
  });

  renderSets(s);
  syncTabs();
}

/* The front panel, as the Stage view on the computer shows it: each
   instrument's level and pan, then the plugin's cutoff, resonance, envelope
   and effects. Horizontal sliders, because a thumb on glass is not a finger
   on a knob. Values follow the plugin once a second unless a thumb is on
   them; a move is sent as it happens, a few times a second at most. */
const knobTouch = {};
async function loadPanel(i) {
  const host = document.querySelector('.knobs[data-input="' + i + '"]');
  if (!host) return;
  let data;
  try { data = await api("/api/panel?input=" + i); } catch (e) { if (e.gate) throw e; return; }
  const slots = data.slots || [];
  const key = JSON.stringify(slots.map(sl => [sl.slot, sl.name, sl.knobs.map(k => k.id + k.name)]));
  if (host.dataset.key !== key) {
    host.dataset.key = key;
    host.innerHTML = "";
    slots.forEach(sl => {
      if (!sl.knobs.length) return;
      const sec = document.createElement("div"); sec.className = "kslot";
      const title = document.createElement("div"); title.className = "kname"; title.textContent = sl.name;
      sec.appendChild(title);
      sl.knobs.forEach(k => {
        const row = document.createElement("label"); row.className = "knob";
        row.innerHTML = '<span class="kn"></span><input type="range" min="0" max="1000"><span class="kv"></span>';
        row.querySelector(".kn").textContent = k.name;
        const r = row.querySelector("input");
        r.dataset.id = k.id; r.dataset.slot = sl.slot;
        const tkey = i + "/" + sl.slot + "/" + k.id;
        let timer = null, pending = null;
        const send = () => {
          const v = pending; pending = null; timer = null;
          api("/api/knob?input=" + i + "&slot=" + sl.slot + "&id=" + encodeURIComponent(k.id) + "&value=" + v, { method: "POST" }).catch(e => { if (!e.gate) show(e.message); });
        };
        r.oninput = () => {
          pending = (r.value / 1000).toFixed(3);
          row.querySelector(".kv").textContent = k.id === "level" ? dbText(r.value / 1000) : k.id === "pan" ? panText(r.value / 1000) : Math.round(r.value / 10) + "%";
          if (!timer) timer = setTimeout(send, 60);
        };
        r.onpointerdown = () => { knobTouch[tkey] = true; };
        r.onpointerup = r.onpointercancel = () => { setTimeout(() => { delete knobTouch[tkey]; }, 300); };
        sec.appendChild(row);
      });
      host.appendChild(sec);
    });
  }
  slots.forEach(sl => sl.knobs.forEach(k => {
    const tkey = i + "/" + sl.slot + "/" + k.id;
    if (knobTouch[tkey]) return;
    const r = host.querySelector('input[data-slot="' + sl.slot + '"][data-id="' + k.id + '"]');
    if (!r) return;
    r.value = Math.round(k.value * 1000);
    r.parentElement.querySelector(".kv").textContent = k.text;
  }));
}
const dbText = v => { const db = -60 + v * 72; return db <= -59.9 ? "off" : (db >= 0 ? "+" : "") + db.toFixed(1) + " dB"; };
const panText = v => { const p = Math.round((v * 2 - 1) * 50); return p === 0 ? "C" : p < 0 ? "L " + (-p) : "R " + p; };
function pollPanels() {
  if (tab !== "programs" || document.visibilityState !== "visible" || !lastState) return;
  lastState.inputs.forEach((inp, i) => { if (!inp.track) loadPanel(i); });
}

// ---- controls ---------------------------------------------------------------
// Fetched separately from the program list: the programs change rarely, the
// parameter values change while you are touching them.
/* What each input's current program has in it, so the header button knows
   whether to open a window, offer a choice, or stay hidden. Filled by
   loadSlots, which the page already calls for every input on each render. */
const slotCache = {};
const guiButtons = {};

function syncGuiButton(i) {
  const b = guiButtons[i];
  if (!b) return;
  const n = (slotCache[i] || []).length;
  b.style.display = n ? "" : "none";
  b.textContent = "GUI";
}

/* Which instrument's window, when a program has more than one. A small sheet
   rather than a dropdown: it is chosen with a thumb, at arm's length. */
function pickSlot(i, list, btn) {
  const back = document.createElement("div"); back.className = "sheet";
  const card = document.createElement("div"); card.className = "card";
  const t = document.createElement("div"); t.className = "sheet-title";
  t.textContent = "Open which plugin's window?";
  card.appendChild(t);

  list.forEach(sl => {
    const b = document.createElement("button");
    b.textContent = sl.name;
    b.onclick = () => { back.remove(); openPluginWindow(i, sl.slot, sl.effect, sl.name, btn, "GUI", sl.bus); };
    card.appendChild(b);
  });

  const cancel = document.createElement("button");
  cancel.className = "cancel"; cancel.textContent = "Cancel";
  cancel.onclick = () => back.remove();
  card.appendChild(cancel);

  back.appendChild(card);
  back.onclick = e => { if (e.target === back) back.remove(); };
  document.body.appendChild(back);
}

/* Opens a plugin's own window, mirrored. Some plugins cannot be followed any
   other way -- Kontakt reports nothing when you move a drawbar in it -- so this
   shows the real thing rather than a guess at its state.

   Shared by the button in each input's header and the one on each slot, so both
   land on the same tab for the same plugin. */
async function openPluginWindow(i, slot, effect, fallbackName, btn, restore, bus) {
  const was = btn.textContent;
  btn.textContent = "...";
  btn.disabled = true;
  if (effect === undefined || effect === null) effect = -1;
  if (bus === undefined || bus === null) bus = -1;
  try {
    const r = await api("/api/pluginview?input=" + i + "&slot=" + slot + "&effect=" + effect + "&bus=" + bus, { method: "POST" });
    if (r.error) { show(r.error); return; }
    /* Through this same port: a stream on a port of its own would need its own
       hole in the firewall, which is precisely what left the tab loading
       forever the first time. */
    /* Name the tab after the sound, not the plugin: on a tablet the tab strip
       shows a few characters, and "Oye Como" says which one this is where
       "Kontakt 8" does not when three programs all use Kontakt.

       The name is also the window target, so pressing it again for the same
       slot brings that tab forward instead of opening another one. Browsers key
       window targets by name, so this costs nothing. */
    const state = lastState && lastState.inputs && lastState.inputs[i];
    const label = state ? (state.currentName || ("Program " + state.current)) : fallbackName;
    const target = bus >= 0 ? "stagerig-output-" + bus + "-fx" + effect
                            : "stagerig-plugin-" + i + "-" + slot + (effect >= 0 ? "-fx" + effect : "");

    const base = "/view/" + r.port;
    const url = base + "/plugin.html?path="
              + encodeURIComponent(base.slice(1) + "/websockify")
              + "&title=" + encodeURIComponent(label);

    const tab = window.open(url, target);
    if (tab) tab.focus();
  } catch (e) {
    if (!e.gate) show(e.message);
  } finally {
    btn.textContent = restore || was;
    btn.disabled = false;
  }
}

/* What this input's current program has loaded. Nothing is drawn for it: the
   only thing that needs to know is the GUI button in the header, which uses it
   to decide between opening a window, asking which instrument, and hiding. */
async function loadSlots(i) {
  try {
    const data = await api("/api/slots?input=" + i);
    slotCache[i] = data.slots;
  } catch (e) {
    if (e.gate) throw e;
    slotCache[i] = [];
  }
  syncGuiButton(i);
}

/* The plugin's own window is where its controls live now, so the page only has
   to follow the program the keyboard selected. */
async function refresh(force) {
  try {
    const s = await api("/api/state");
    if (force || s.revision !== rev) { rev = s.revision; render(s); if (keySheet) keySheet.draw(); }
    syncLevelPolling();
    syncTransportPolling();
    if (transportUi) pollTransport();
    pollPanels();
  } catch (e) {
    if (e.gate) askForCode("That code did not match the one in the Tablet control window on the computer. Try again.");
    else show(e.message);
  }
}
document.getElementById("panic").onclick = async () => {
  try { await api("/api/panic", { method: "POST" }); show("PANIC sent: every note stopped.", 2000); }
  catch (e) { if (!e.gate) show(e.message); }
};
if (token) refresh(true); else askForCode("");
setInterval(() => { if (token) refresh(false); }, 1000);

// Registering this is what makes a browser offer "install" / "Add to Home
// Screen" as a prompt rather than something buried in a menu.
if ('serviceWorker' in navigator)
  addEventListener('load', () => navigator.serviceWorker.register('/sw.js').catch(() => {}));
</script></body></html>)HTML";

static const char* kManifest = R"JSON({
  "name": "StageRig", "short_name": "StageRig",
  "start_url": ".", "display": "standalone",
  "background_color": "#15161c", "theme_color": "#15161c",
  "icons": [
    { "src": "/icon-192.png", "sizes": "192x192", "type": "image/png", "purpose": "any" },
    { "src": "/icon.png", "sizes": "256x256", "type": "image/png", "purpose": "any" },
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any" },
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable" }
  ]
})JSON";

//==============================================================================
RemoteServer::RemoteServer (Engine& e, PropertiesFile& s)
    : Thread ("stagerig-remote"), engine (e), settings (s)
{
    engine.addListener (this);
}

RemoteServer::~RemoteServer()
{
    engine.removeListener (this);
    stop();
}

/* Our own page for a plugin window, rather than noVNC's.

   Its vnc_lite has no viewport meta tag, so a tablet lays the page out at
   desktop width and pinch does nothing; and it can only scale-to-fit, which
   makes a 1010px Kontakt window unreadable on a tablet. This sets the viewport
   so the browser's own pinch-zoom works, and shows the plugin at full size in a
   scrollable box so there is something to zoom into.

   Served from here rather than patched into /usr/share/novnc, which belongs to
   the distribution and would be overwritten by an update. */
/* The stand. One song's chart, full screen, following the set: when the
   song changes -- footswitch, tablet, program change -- this page moves to
   the new song's chart by itself, like a stand that turns its own pages.
   A PDF that has been rendered is shown a page at a time (tap the right
   third or swipe for the next page, the left third for the previous); one
   that has not is handed to the browser. Images stretch to the width and
   pinch to zoom. Text is monospace; ChordPro gets its chords set above
   the words. */
static const char* kChartHtml = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=5,user-scalable=yes,viewport-fit=cover">
<meta name="theme-color" content="#15161c">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>Chart</title>
<style>
  :root { color-scheme: dark; }
  html,body { margin:0; padding:0; background:#15161c; color:#d7dbe2;
              font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; }
  #bar { position:sticky; top:0; z-index:5; display:flex; align-items:center; gap:10px; padding:8px 12px;
         background:#1e1f24; border-bottom:1px solid #2b2d35; }
  #bar a, #bar button { background:#2b2d35; color:#d7dbe2; border:0; border-radius:10px; padding:8px 14px;
                        font-size:15px; font-weight:600; text-decoration:none; }
  #bar button.on { background:#4f9dff; color:#06121f; }
  #title { flex:1; font-size:17px; font-weight:700; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  #tabs { display:flex; gap:6px; padding:8px 12px; flex-wrap:wrap; }
  #tabs:empty { display:none; }
  #tabs button { background:#26282f; color:#9aa0ab; border:0; border-radius:8px; padding:6px 12px; font-size:14px; }
  #tabs button.on { background:#3a3d47; color:#fff; }
  #page { min-height:70vh; }
  #page img { display:block; width:100%; height:auto; background:#fff; }
  #page iframe { display:block; width:100%; height:calc(100vh - 60px); border:0; background:#fff; }
  #page pre { margin:0; padding:16px; font:15px/1.45 ui-monospace,Menlo,Consolas,monospace; white-space:pre-wrap;
              background:#fbf8f1; color:#1b1b1b; min-height:70vh; }
  #page pre .c { color:#9b2d2d; font-weight:700; }
  #page pre .d { color:#6b5e3c; font-style:italic; }
  #empty { padding:40px 20px; text-align:center; color:#9aa0ab; font-size:18px; }
  #nav { position:fixed; left:0; right:0; bottom:0; display:flex; justify-content:space-between; padding:10px 12px;
         pointer-events:none; }
  #nav button { pointer-events:auto; background:#2b2d35cc; color:#fff; border:0; border-radius:12px; padding:12px 18px;
                font-size:16px; font-weight:700; }
  #nav span { align-self:center; background:#2b2d35cc; border-radius:10px; padding:6px 12px; font-size:14px; }
</style></head>
<body>
<div id="bar">
  <a id="back" href="/">&#8592; StageRig</a>
  <div id="title"></div>
  <button id="follow" class="on" title="Move to each song's chart as the set moves">Follow</button>
</div>
<div id="tabs"></div>
<div id="page"></div>
<div id="nav" style="display:none"><button id="prev">&#8249; Prev</button><span id="where"></span><button id="next">Next &#8250;</button></div>
<script>
let token = new URLSearchParams(location.search).get("t") || localStorage.getItem("t") || "";
if (token) localStorage.setItem("t", token);
document.getElementById("back").href = "/?t=" + encodeURIComponent(token);
async function api(path) {
  const r = await fetch(path + (path.includes("?") ? "&" : "?") + "t=" + encodeURIComponent(token));
  if (!r.ok) throw new Error("Can't reach StageRig on the computer");
  return r.json();
}
const withToken = (u) => u + "?t=" + encodeURIComponent(token);
const esc = (t) => t.replace(/[&<>]/g, (ch) => ({"&":"&amp;","<":"&lt;",">":"&gt;"}[ch]));

let follow = true;
let song = parseInt(new URLSearchParams(location.search).get("song") || "-1", 10);
let chart = parseInt(new URLSearchParams(location.search).get("c") || "0", 10);
let pageNo = 1, state = null, shownKey = "";

document.getElementById("follow").onclick = () => {
  follow = !follow;
  document.getElementById("follow").className = follow ? "on" : "";
};

/* The song in play, as an index into state.songs: the one the rig says, or the set's position. */
function playingSong(s) {
  if (s.playingSong >= 0) return s.playingSong;
  const st = (s.sets || [])[s.currentSet];
  if (!st || s.currentSong < 0 || s.currentSong >= st.songs.length) return -1;
  return st.songs[s.currentSong];
}

/* ChordPro: [C]chords inside the words become a line of chords above them,
   placed by character so a monospace face lines them up. Directives in
   braces become a title, a subtitle or a comment; the rest are dropped. */
function renderChordPro(text) {
  const out = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/\s+$/, "");
    const m = line.match(/^\{\s*([a-z_]+)\s*:?\s*(.*?)\s*\}$/i);
    if (m) {
      const k = m[1].toLowerCase(), v = esc(m[2]);
      if (k === "title" || k === "t") out.push('<span class="d" style="font-size:20px;font-weight:700;font-style:normal">' + v + "</span>");
      else if (k === "subtitle" || k === "st" || k === "artist") out.push('<span class="d">' + v + "</span>");
      else if (k === "comment" || k === "c" || k === "ci" || k === "cb") out.push('<span class="d">(' + v + ")</span>");
      continue;
    }
    if (line.indexOf("[") < 0) { out.push(esc(line)); continue; }
    let chords = "", words = "";
    const re = /\[([^\]]*)\]/g; let last = 0, mm;
    while ((mm = re.exec(line)) !== null) {
      words += line.slice(last, mm.index);
      while (chords.length < words.length) chords += " ";
      chords += mm[1] + " ";
      last = re.lastIndex;
    }
    words += line.slice(last);
    out.push('<span class="c">' + esc(chords.replace(/\s+$/, "")) + "</span>");
    out.push(esc(words));
  }
  return out.join("\n");
}

async function showChart() {
  const s = state; if (!s) return;
  const sg = (s.songs || [])[song];
  const page = document.getElementById("page"), tabs = document.getElementById("tabs"), nav = document.getElementById("nav");
  document.getElementById("title").textContent = sg ? (sg.name || "Untitled") : "";
  if (!sg || !sg.charts || !sg.charts.length) {
    tabs.innerHTML = ""; nav.style.display = "none";
    page.innerHTML = '<div id="empty">' + (sg ? "No chart for this song" : "No song is playing") + "</div>";
    shownKey = ""; return;
  }
  if (chart < 0 || chart >= sg.charts.length) chart = 0;
  const c = sg.charts[chart];
  const key = song + "/" + chart + "/" + (c.pages || 0) + "/" + pageNo;
  if (key === shownKey) return;          // nothing new to draw
  shownKey = key;

  tabs.innerHTML = ""; nav.style.display = "none";
  if (sg.charts.length > 1) {
    sg.charts.forEach((cc, i) => {
      const b = document.createElement("button"); b.textContent = cc.title; b.className = i === chart ? "on" : "";
      b.onclick = () => { chart = i; pageNo = 1; shownKey = ""; showChart(); };
      tabs.appendChild(b);
    });
  }

  if (c.kind === "pdf" && c.pages > 0) {
    if (pageNo > c.pages) pageNo = c.pages;
    page.innerHTML = '<img src="' + withToken(c.url + "/page/" + pageNo) + '" alt="page ' + pageNo + '">';
    if (c.pages > 1) {
      nav.style.display = "flex";
      document.getElementById("where").textContent = pageNo + " / " + c.pages;
      if (pageNo < c.pages) new Image().src = withToken(c.url + "/page/" + (pageNo + 1));   // have the next ready
    }
  } else if (c.kind === "pdf") {
    page.innerHTML = '<iframe src="' + withToken(c.url) + '"></iframe>'
                   + '<div id="empty"><a style="color:#4f9dff" href="' + withToken(c.url) + '" target="_blank">Open the PDF</a></div>';
  } else if (c.kind === "image") {
    page.innerHTML = '<img src="' + withToken(c.url) + '" alt="' + esc(c.title) + '">';
  } else {
    try {
      const r = await fetch(withToken(c.url)); const text = await r.text();
      page.innerHTML = "<pre>" + (c.kind === "chordpro" ? renderChordPro(text) : esc(text)) + "</pre>";
    } catch (e) { page.innerHTML = '<div id="empty">Could not load the chart</div>'; }
  }
}

function turn(delta) {
  const sg = state && state.songs[song]; const c = sg && sg.charts && sg.charts[chart];
  if (!c || !(c.pages > 1)) return;
  const n = Math.min(c.pages, Math.max(1, pageNo + delta));
  if (n !== pageNo) { pageNo = n; shownKey = ""; showChart(); window.scrollTo(0, 0); }
}
document.getElementById("prev").onclick = () => turn(-1);
document.getElementById("next").onclick = () => turn(+1);
/* Tap the right third for the next page, the left third for the previous;
   the middle is left alone so a pinch or a scroll does not turn pages. */
document.getElementById("page").addEventListener("click", (ev) => {
  const w = window.innerWidth;
  if (ev.clientX > w * 2 / 3) turn(+1); else if (ev.clientX < w / 3) turn(-1);
});
let touchX = null;
document.addEventListener("touchstart", (e) => { touchX = e.touches.length === 1 ? e.touches[0].clientX : null; }, { passive: true });
document.addEventListener("touchend", (e) => {
  if (touchX === null) return;
  const dx = e.changedTouches[0].clientX - touchX; touchX = null;
  if (Math.abs(dx) > 80) turn(dx < 0 ? +1 : -1);
}, { passive: true });

let rev = null, lastPlaying = null;
async function refresh() {
  try {
    const s = await api("/api/state");
    const first = state === null;
    state = s;
    /* Follow moves with the set, it does not override a choice: the song
       named in the URL stays until the set actually changes song. Opened
       without one, the playing song is shown. */
    const now = playingSong(s);
    if (song < 0) song = now;
    else if (follow && lastPlaying !== null && now >= 0 && now !== lastPlaying && now !== song)
    { song = now; chart = 0; pageNo = 1; shownKey = ""; }
    lastPlaying = now;
    if (first || s.revision !== rev) { rev = s.revision; shownKey = ""; }
    await showChart();
  } catch (e) {
    document.getElementById("title").textContent = e.message;
  }
}
refresh();
setInterval(refresh, 1000);
</script>
</body></html>
)HTML";

static const char* kPluginViewHtml = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,minimum-scale=0.2,maximum-scale=6,user-scalable=yes,viewport-fit=cover">
<meta name="theme-color" content="#15161c">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<link rel="manifest" href="/manifest.webmanifest">
<title>Plugin</title>
<style>
  :root { color-scheme: dark; }
  html,body { margin:0; padding:0; background:#15161c; color:#d7dbe2;
              font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; }
  /* Dynamic viewport height: 100vh is the height with the browser's chrome
     hidden, so on a tablet it puts the bottom of the plugin permanently under
     the address bar. 100dvh follows the bar as it comes and goes. The 100vh
     line is the fallback for browsers that do not know dvh. */
  #screen { width:100%; height:100vh; height:100dvh; overflow:auto; -webkit-overflow-scrolling:touch; }

  /* A plugin window is a control surface, not a document. Without this the
     browser treats a press-and-hold as the start of a text selection: the
     cursor turns into an I-beam, a long press pops up the selection handles,
     and the drag never reaches the plugin as a held note. */
  #screen, #screen * {
    -webkit-user-select:none; -moz-user-select:none; -ms-user-select:none; user-select:none;
    -webkit-touch-callout:none;
  }
  /* A finger, because everything under it is something to press. !important
     because noVNC sets the cursor as an inline style on the canvas -- the
     remote pointer's shape, or 'none' -- and inline beats a plain rule. With
     a Wine-bridged plugin that shape is whatever Wine reports, which is how an
     I-beam ended up over an organ. */
  #screen canvas { cursor:pointer !important; }

  /* noVNC needs the raw touches, so the browser must not claim them for
     scrolling or zooming first. */
  #screen { touch-action:none; overscroll-behavior:contain; }
  #msg { position:fixed; left:0; right:0; top:0; padding:10px 14px; font-size:14px;
         background:#26282f; border-bottom:1px solid #2b2d35; }
  #bar { position:fixed; right:10px; bottom:10px; display:flex; gap:8px;
         bottom:calc(10px + env(safe-area-inset-bottom)); }
  #bar button { background:#26282fdd; color:#d7dbe2; border:1px solid #3a3d47;
                border-radius:10px; padding:10px 14px; font-size:13px; font-weight:700; }
  #bar button.on { background:#5aa9ff; color:#06121f; border-color:#5aa9ff; }
</style>
</head><body>
<div id="msg">Connecting to the plugin window on the computer…</div>
<script>
  /* Before anything else: is this window still being mirrored? The module
     below imports noVNC from under the mirror, and on a window that has
     gone the import 404s and the module never runs -- so this check cannot
     live inside it. The relay answers 404 for a mirror StageRig no longer
     has (the window was closed on the computer, or its program swapped out). */
  fetch(location.pathname.replace(/plugin\.html$/, 'vnc.html'), { cache: 'no-store' })
    .then(r => { if (r.status === 404) { const m = document.getElementById('msg'); m.style.display = 'block';
                 m.textContent = 'The plugin window was closed. Go back to the StageRig tab and press GUI again.'; } })
    .catch(() => {});
</script>
<div id="screen"></div>
<div id="bar"><button id="move">Move view</button><button id="zoomout">-</button><button id="zoomin">+</button><button id="full">Full screen</button><button id="fit">Fit to screen</button></div>
<script type="module">
  import RFB from './core/rfb.js';

  const q = new URLSearchParams(location.search);
  const path = q.get('path') || 'websockify';
  const title = q.get('title');
  if (title) document.title = title;
  const url = (location.protocol === 'https:' ? 'wss' : 'ws') + '://'
            + location.hostname + (location.port ? ':' + location.port : '') + '/' + path;

  const msg = document.getElementById('msg');
  let rfb = null;              // the current connection; replaced on reconnect

  /* Input is noVNC's job. It has a tested path for mouse, touch and its own
     gestures; the hand-rolled one here sent press and release from our own
     pointer handlers, bypassed the viewport offset and captured the pointer --
     which between them put clicks somewhere other than where they were aimed.
     If touch needs tuning again, tune noVNC's gesture handler rather than
     going around it. */
  const screen = document.getElementById('screen');
  const fit = document.getElementById('fit');
  let zoom = 1;
  let fitting = false;
  let playing = true;               // false = Move: drag pans instead of clicking

  function syncFit() {
    fit.className = fitting ? 'on' : '';
    fit.textContent = fitting ? 'Actual size' : 'Fit to screen';
  }

  const canvasOf = () => screen.querySelector('canvas');
  function releaseAll() {}          // nothing of ours is held any more

  /* Zoom through noVNC's own Display scale rather than a CSS transform. absX
     and absY divide by that scale, so a touch or a mouse click at a scaled
     pixel lands on the right framebuffer pixel with no arithmetic here -- a
     transform would scale the canvas without telling noVNC, and every click
     would miss by the zoom factor. The canvas grows in layout too, so the
     scroll box grows with it and the far side stays reachable.

     noVNC resets this scale to 1 whenever the container resizes (its own
     ResizeObserver, one animation frame later), which happens each time the
     address bar hides or the screen turns. So it is re-applied after that. */
  function applyZoom() {
    if (fitting) return;
    /* Re-applying the scale resizes the canvas, and shrinking a scrolled
       element makes the browser clamp its scroll position -- so without this,
       every resize throws you back to the top of the plugin. A tablet resizes
       whenever the address bar hides or shows, which is constantly. */
    const c = canvasOf();
    const box = c ? c.parentElement : null;
    const sl = box ? box.scrollLeft : 0, st = box ? box.scrollTop : 0;
    try { rfb._display.scale = zoom; } catch (e) {}
    if (box && (box.scrollLeft !== sl || box.scrollTop !== st)) {
      box.scrollLeft = sl; box.scrollTop = st;
    }
  }
  function reapplyZoomLater() {
    requestAnimationFrame(() => requestAnimationFrame(applyZoom));
  }
  new ResizeObserver(reapplyZoomLater).observe(screen);

  function setZoom(z) {
    zoom = Math.min(6, Math.max(0.25, z));
    if (fitting) { fitting = false; rfb.scaleViewport = false; syncFit(); }
    applyZoom();
  }

  document.getElementById('zoomin').onclick = () => setZoom(zoom * 1.25);
  document.getElementById('zoomout').onclick = () => setZoom(zoom / 1.25);

  /* Move: hands the touches back to the browser so the plugin can be panned and
     pinched, and stops sending them to the plugin. Two modes rather than one
     clever one, because guessing wrong mid-song is worse than pressing a
     button. */
  const move = document.getElementById('move');
  function syncMode() {
    document.body.classList.toggle('playing', playing);
    move.className = playing ? '' : 'on';
    move.textContent = playing ? 'Move view' : 'Done moving';
    move.title = playing ? 'Drag to pan the plugin instead of operating it'
                         : 'Back to operating the plugin';
    try { rfb.dragViewport = !playing; } catch (e) {}
  }
  move.onclick = () => { playing = !playing; syncMode(); };
  syncMode();

  /* Fullscreen: the address bar is a real cost on a small screen, and a plugin
     window is exactly the thing you want the whole screen for. Only offered
     where the browser has the API -- iOS Safari does not, and there the way to
     lose the bar is the home-screen shortcut, which the manual explains. */
  const full = document.getElementById('full');
  if (document.fullscreenEnabled) {
    full.onclick = () => {
      if (document.fullscreenElement) document.exitFullscreen();
      else document.documentElement.requestFullscreen({ navigationUI: 'hide' }).catch(() => {});
    };
    document.addEventListener('fullscreenchange', () => {
      const on = !!document.fullscreenElement;
      full.className = on ? 'on' : '';
      full.textContent = on ? 'Exit full screen' : 'Full screen';
    });
  } else {
    full.style.display = 'none';
  }

  fit.onclick = () => {
    fitting = !fitting;
    /* Fit is noVNC's own scaling, which keeps the whole window on screen. It
       and the +/- zoom are two ways of doing one thing, so turning on either
       turns off the other. */
    if (fitting) zoom = 1;
    rfb.scaleViewport = fitting;
    syncFit();
    applyZoom();
  };

  /* Connect, and keep connecting. A wifi blip on a stand must not leave the
     page saying "lost the connection" until someone walks over and presses
     GUI again; it retries by itself, backing off to five seconds, and only
     gives up after a couple of minutes -- by which point the window really has
     gone, and the main page's GUI button is the way back. */
  let retries = 0;
  async function connect() {
    releaseAll();
    /* Still being shown? The relay answers 404 for a window StageRig no
       longer mirrors -- closed on the computer, or its program swapped out.
       Saying so beats retrying into nothing for two minutes. */
    try {
      const probe = await fetch(location.pathname.replace(/plugin\.html$/, 'vnc.html'), { cache: 'no-store' });
      if (probe.status === 404) {
        msg.style.display = 'block';
        msg.textContent = 'The plugin window was closed. Go back to the StageRig tab and press GUI again.';
        return;
      }
    } catch (e) { /* no answer at all: let the reconnect below find out */ }
    /* noVNC's own viewport, which is what its input path expects: with
       clipping on it sizes the canvas to the container and keeps a viewport
       offset, and absX/absY add that offset to turn a click into a framebuffer
       position. Turning it off left the offset at zero and the canvas bigger
       than its box, which is how clicks stopped landing where they were
       aimed -- Move pans it now, so the far side is still reachable. */
    screen.innerHTML = '';                 // a previous connection's canvas
    rfb = new RFB(screen, url, {});
    rfb.clipViewport = true;
    rfb.scaleViewport = fitting;
    rfb.resizeSession = false;
    /* noVNC focuses the canvas on every click, which scrolls it into view and
       throws the plugin window back to the top. We focus it ourselves, without
       the scroll. */
    rfb.focusOnClick = false;

    rfb.addEventListener('connect', () => { retries = 0; msg.style.display = 'none'; reapplyZoomLater(); });
    rfb.addEventListener('disconnect', e => {
      releaseAll();
      msg.style.display = 'block';
      if (retries < 30) {
        retries++;
        msg.textContent = 'Connection dropped, reconnecting…';
        setTimeout(connect, Math.min(5000, 1000 * retries));
      } else {
        msg.textContent = (e.detail && e.detail.clean)
          ? 'The plugin window was closed. Go back to the StageRig tab and press GUI again.'
          : 'Lost the connection to the plugin window. Go back to the StageRig tab and press GUI again.';
      }
    });
  }
  connect();
</script>
</body></html>)HTML";

/** Copies bytes both ways until one end goes quiet.

    A websocket is long-lived and either side may speak at any time, so this
    cannot be request-then-response like the rest of the server: it has to sit
    on the connection and pump. One thread per open plugin window is acceptable
    -- there is one window, occasionally two. */
bool RemoteServer::relayToPluginView (StreamingSocket& client, const String& firstChunk, int webPort)
{
    StreamingSocket upstream;
    if (! upstream.connect ("127.0.0.1", webPort, 3000))
        return false;

    /* One request per connection. The browser would otherwise send several down
       the same socket, and only the first carries a prefix this relay has
       already stripped -- the rest reached the bridge as /view/6910/core/... and
       404'd, which looked like a flaky module loader.

       Asking the browser to close each connection sidesteps rewriting a stream
       that turns into websocket frames partway through. It costs a connection
       per file, on loopback, once per window opened. */
    auto request = firstChunk;
    if (! request.containsIgnoreCase ("upgrade: websocket"))
        request = request.replace ("\r\nConnection: keep-alive", "\r\nConnection: close")
                         .replace ("\r\nConnection: Keep-Alive", "\r\nConnection: close");

    const auto utf8 = request.toRawUTF8();
    if (upstream.write (utf8, (int) strlen (utf8)) <= 0)
        return false;

    char buf[16384];
    for (;;)
    {
        bool moved = false;

        if (upstream.waitUntilReady (true, 20) == 1)
        {
            const int n = upstream.read (buf, sizeof (buf), false);
            if (n <= 0) break;
            if (client.write (buf, n) <= 0) break;
            moved = true;
        }

        if (client.waitUntilReady (true, 20) == 1)
        {
            const int n = client.read (buf, sizeof (buf), false);
            if (n <= 0) break;
            if (upstream.write (buf, n) <= 0) break;
            moved = true;
        }

        if (! moved && (! client.isConnected() || ! upstream.isConnected()))
            break;
    }

    upstream.close();
    return true;
}

bool RemoteServer::start (int p)
{
    /* Anything of ours left over from a crash. x11vnc runs with -forever and
       websockify never exits on its own, so after StageRig dies they keep the
       ports, and every GUI press on the tablet would fail until someone found
       and killed them by hand. That must not be the state of things on stage
       after a relaunch. */
    PluginView::sweepStrays();

    stop();
    // A short code you can read off the screen and type on a tablet, kept across
    // restarts so a home-screen shortcut keeps working: a token regenerated every
    // run would break the bookmark exactly when you least want to re-pair, on stage.
    // Letters that misread (O/0, I/1/l) are excluded.
    token = settings.getValue ("remoteCode");
    if (token.length() != 6)
    {
        static const char* alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
        auto& rng = Random::getSystemRandom();
        token.clear();
        for (int i = 0; i < 6; ++i) token << alphabet[rng.nextInt (31)];
        settings.setValue ("remoteCode", token);
        settings.saveIfNeeded();
    }
    listener = std::make_unique<StreamingSocket>();
    /* Close this on exec, or every child we start inherits it. An orphaned
       websockify held port 7777 open after StageRig had gone, so the next
       start could not bind and the tablet was unreachable until the stray
       process was found by hand. */
    if (! listener->createListener (p))
    {
        listener.reset();
        return false;
    }
    if (const int fd = listener->getRawSocketHandle(); fd >= 0)
        ::fcntl (fd, F_SETFD, FD_CLOEXEC);

    port = p;
    running = true;
    startThread();
    return true;
}

void RemoteServer::stop()
{
    PluginView::stopAll();      // no orphan x11vnc or websockify left behind
    running = false;
    if (listener != nullptr) listener->close();
    stopThread (2000);
    listener.reset();
    port = 0;
}

/** Which address should we print for the tablet to type?

    A developer machine can easily have a dozen IPv4 addresses -- Docker and LXD
    bridges, libvirt networks, one per container network -- and a tablet can reach
    none of them. Picking the first non-loopback address, as this used to, hands
    the user something like 172.17.0.1 and looks like the feature is broken.

    So rank by interface instead: a wireless interface first (on stage that is
    either the venue's network or our own hotspot), then wired, then anything
    else we do not recognise as virtual, and only then give up. Within wireless
    we prefer a hotspot-shaped address, because if the laptop is serving its own
    network that is certainly the one the tablet is on. */
static int addressRank (const String& iface, const String& addr)
{
    // Virtual interfaces: nothing external is ever on the other side of these.
    if (iface.startsWith ("docker") || iface.startsWith ("br-") || iface.startsWith ("virbr")
        || iface.startsWith ("lxdbr") || iface.startsWith ("veth") || iface.startsWith ("vnet")
        || iface.startsWith ("tun") || iface.startsWith ("tap") || iface.startsWith ("vmnet")
        || iface.startsWith ("zt") || iface.startsWith ("wg"))
        return 0;

    const auto wireless = iface.startsWith ("wl") || iface.startsWith ("wlan") || iface.startsWith ("ath");
    const auto wired    = iface.startsWith ("en") || iface.startsWith ("eth");

    // NetworkManager's shared mode always uses 10.42.x: that is our own hotspot.
    if (wireless && addr.startsWith ("10.42.")) return 4;
    if (wireless) return 3;
    if (wired)    return 2;
    return 1;
}

String RemoteServer::getUrl() const
{
    if (! running.load()) return {};
    return "http://" + getHostAddress() + ":" + String (port) + "/";
}

String RemoteServer::getHostAddress()
{
    String best = "127.0.0.1";
    int bestRank = -1;

   #if JUCE_LINUX || JUCE_MAC
    struct ifaddrs* list = nullptr;
    if (::getifaddrs (&list) == 0)
    {
        for (auto* i = list; i != nullptr; i = i->ifa_next)
        {
            if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
            if ((i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0) continue;

            char buf[INET_ADDRSTRLEN] = {};
            auto* in = reinterpret_cast<struct sockaddr_in*> (i->ifa_addr);
            if (::inet_ntop (AF_INET, &in->sin_addr, buf, sizeof (buf)) == nullptr) continue;

            const String iface (i->ifa_name), addr (buf);
            if (addr.startsWith ("127.") || addr.startsWith ("169.254.")) continue;

            if (const auto rank = addressRank (iface, addr); rank > bestRank)
            {
                bestRank = rank;
                best = addr;
            }
        }
        ::freeifaddrs (list);
    }
   #endif

    if (bestRank < 0)
        for (auto& ip : IPAddress::getAllAddresses())
            if (! ip.isNull() && ! ip.toString().startsWith ("127.") && ip.toString().containsChar ('.'))
                { best = ip.toString(); break; }

    return best;
}

//==============================================================================
bool RemoteServer::authorised (const String& request) const
{
    // Matched case-insensitively: the code is shown in capitals but a tablet keyboard
    // will happily offer lower case, and being fussy about that on stage is unkind.
    return token.isNotEmpty() && request.containsIgnoreCase ("t=" + token);
}

//==============================================================================
/* Produces a string on the message thread and waits for it. The setup is
   edited there -- a name being typed, a group changed, a file opened -- and
   reading it from a request thread at the same moment is a race with a torn
   String at the end of it. The tablet polls once a second, so any edit on the
   laptop mid-set would be rolling that dice.

   Results travel in shared_ptrs rather than stack references: if the wait
   times out (a modal dialog holding the message thread, say) the lambda still
   runs later, and must not write into a frame that has gone. */
static bool produceOnMessageThread (std::function<String()> produce, String& out, int timeoutMs)
{
    if (MessageManager::getInstance()->isThisTheMessageThread()) { out = produce(); return true; }

    auto result = std::make_shared<String>();
    auto done   = std::make_shared<WaitableEvent>();
    MessageManager::callAsync ([produce, result, done] { *result = produce(); done->signal(); });
    if (! done->wait (timeoutMs)) return false;
    out = *result;
    return true;
}

static void sendBusy (StreamingSocket& sock, void (*send) (StreamingSocket&, const String&, const String&, const String&))
{
    send (sock, "503 Service Unavailable", "application/json", "{\"error\":\"StageRig is busy; try again.\"}");
}

String RemoteServer::stateJson() const
{
    const auto& setup = engine.getSetup();
    DynamicObject::Ptr root (new DynamicObject());
    root->setProperty ("revision", revision.load());
    // Whether the song in play has a backing track; the page polls /api/transport for where it is.
    root->setProperty ("track", engine.hasTrack());
    // Or scenes: a song played by hand, its sections stepped from the stand.
    root->setProperty ("scenes", ! engine.hasTrack() && engine.songHasScenes (engine.currentSongIndex()));
    // Whether the stand may build set lists and scenes (Tablet window on the computer).
    root->setProperty ("canEdit", settings.getBoolValue ("remoteEditing", true));
    root->setProperty ("tempo", engine.getTempoBpm());
    {
        // The names a song's key is picked from on the tablet, the same words as on the laptop.
        Array<var> roots, scales;
        for (int i = 0; i < 12; ++i) roots.add (MidiFx::noteNames[i]);
        for (int i = 0; i < MidiFx::numScaleModes(); ++i) scales.add (MidiFx::scaleModes[i]);
        root->setProperty ("roots", roots);
        root->setProperty ("scales", scales);
    }
    Array<var> inputs;
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        const auto& in = setup.inputs[(size_t) i];
        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("name", in.name);
        o->setProperty ("current", in.currentProgram);
        const auto& cur = in.programs[(size_t) in.currentProgram];
        o->setProperty ("currentName", cur.name);
        bool loading = false;
        for (int s = 0; s < (int) cur.slots.size() && ! loading; ++s)
            loading = engine.isPluginLoading (i, in.currentProgram, s);
        for (int e = 0; e < (int) cur.effects.size() && ! loading; ++e)
            loading = engine.isPluginLoading (i, in.currentProgram, -1, e);
        o->setProperty ("loading", loading);
        if (in.isAudio()) o->setProperty ("audio", true);
        if (in.isTrack()) o->setProperty ("track", true);     // its program follows the song: nothing to pick
        Array<var> progs;
        for (int p = 0; p < InputDef::numPrograms; ++p)
        {
            const auto& def = in.programs[(size_t) p];
            if (def.isEmpty() && def.name.isEmpty()) continue;   // only what you have set up
            DynamicObject::Ptr pd (new DynamicObject());
            pd->setProperty ("index", p);
            pd->setProperty ("name", def.name);
            if (def.group.isNotEmpty()) pd->setProperty ("group", def.group);
            progs.add (var (pd.get()));
        }
        o->setProperty ("programs", progs);
        inputs.add (var (o.get()));
    }
    root->setProperty ("inputs", inputs);

    /* Songs and sets. Sent whole rather than on demand: the list is small --
       a gig is tens of songs, not thousands -- and the page already polls once
       a second, so this costs nothing and means the set list is never a
       request behind what the rig is doing. */
    const auto& songs = engine.getSongs();
    const auto& sets  = engine.getSets();
    if (! songs.empty() || ! sets.empty())
    {
        Array<var> songArr;
        for (const auto& sg : songs)
        {
            DynamicObject::Ptr so (new DynamicObject());
            so->setProperty ("name", sg.name);
            if (sg.tempoBpm > 0.0)     so->setProperty ("tempo", sg.tempoBpm);
            if (sg.notes.isNotEmpty()) so->setProperty ("notes", sg.notes);
            if (sg.hasKey())
            {
                so->setProperty ("key", MidiFx::keyName (sg.keyRoot, sg.keyMode));
                so->setProperty ("keyRoot", sg.keyRoot);
                so->setProperty ("keyMode", sg.keyMode);
            }

            /* Charts, by index: the page asks for /chart/<song>/<n>, never for
               a file name, so nothing a tablet sends can name a path. A PDF
               that has been rendered says how many pages it has. */
            if (! sg.charts.empty())
            {
                const int songIndex = (int) (&sg - songs.data());
                const auto folder = engine.getChartsFolder();
                Array<var> chartArr;
                for (int c = 0; c < (int) sg.charts.size(); ++c)
                {
                    const auto& ref = sg.charts[(size_t) c];
                    const auto file = folder.getChildFile (ref.file);
                    DynamicObject::Ptr co (new DynamicObject());
                    co->setProperty ("title", ref.displayName());
                    co->setProperty ("kind", chartKindFor (file));
                    co->setProperty ("url", "/chart/" + String (songIndex) + "/" + String (c));
                    if (chartKindFor (file) == "pdf")
                        co->setProperty ("pages", chartPages (file).size());
                    chartArr.add (var (co.get()));
                }
                so->setProperty ("charts", chartArr);
            }

            /* What each keyboard plays, named rather than numbered: "B3" is
               what you are looking for on a stand, not "002". */
            Array<var> parts;
            for (int i = 0; i < (int) setup.inputs.size(); ++i)
            {
                const int prog = sg.programFor (i);
                if (setup.inputs[(size_t) i].isTrack()) continue;     // the track's program is the song itself
                if (prog < 0) continue;
                DynamicObject::Ptr pd (new DynamicObject());
                pd->setProperty ("input", setup.inputs[(size_t) i].name);
                pd->setProperty ("program", prog);
                pd->setProperty ("name", setup.inputs[(size_t) i].programs[(size_t) prog].name);
                parts.add (var (pd.get()));
            }
            so->setProperty ("parts", parts);
            songArr.add (var (so.get()));
        }
        root->setProperty ("songs", songArr);

        Array<var> setArr;
        for (const auto& st : sets)
        {
            DynamicObject::Ptr to (new DynamicObject());
            to->setProperty ("name", st.name);
            Array<var> idx;
            for (int i : st.songs) idx.add (i);
            to->setProperty ("songs", idx);
            setArr.add (var (to.get()));
        }
        root->setProperty ("sets", setArr);
        root->setProperty ("currentSet", engine.getCurrentSet());
        root->setProperty ("currentSong", engine.getCurrentSongInSet());
        root->setProperty ("playingSong", engine.currentSongIndex());        // the song in play, in or outside the set list
    }
    return JSON::toString (var (root.get()), true);
}

const std::vector<RemoteServer::FilePart>& RemoteServer::partsOf (const File& f) const
{
    const auto key = f.getFullPathName() + "|" + String (f.getLastModificationTime().toMilliseconds());
    auto it = partCache.find (key);
    if (it != partCache.end()) return it->second;
    std::vector<FilePart> parts;
    MidiDoc doc;
    String err;
    if (doc.load (f, err))
        for (int ch = 1; ch <= 16; ++ch)
            if (doc.channelUsed (ch))
            {
                const auto name = doc.partName (ch);
                parts.push_back ({ ch, name.isNotEmpty() ? name : (ch == 10 ? String ("Drums") : String (GmSynth::programName (doc.firstProgram (ch)))), doc.firstProgram (ch) });
            }
    if (partCache.size() > 64) partCache.clear();
    return partCache[key] = std::move (parts);
}

String RemoteServer::panelJson (int input) const
{
    /* The front panel of an input's current program, as the Stage view shows
       it: each instrument's level and pan, then the plugin's parameters a
       player reaches for (or the ones chosen on the computer), with their
       values as the plugin last reported them -- no round trip here. */
    DynamicObject::Ptr o (new DynamicObject());
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size()) return JSON::toString (var (o.get()), true);
    const auto& in = inputs[(size_t) input];
    const int program = in.currentProgram;
    const auto& prog = in.programs[(size_t) program];
    o->setProperty ("input", input);
    o->setProperty ("program", program);
    Array<var> slots;
    for (int si = 0; si < (int) prog.slots.size(); ++si)
    {
        const auto& sd = prog.slots[(size_t) si];
        DynamicObject::Ptr so (new DynamicObject());
        so->setProperty ("slot", si);
        so->setProperty ("name", sd.kind == SlotDef::Kind::plugin ? sd.plugin.name : sd.kind == SlotDef::Kind::gm ? String ("General MIDI")
                                 : sd.kind == SlotDef::Kind::external ? String ("MIDI device") : sd.targetInputName);
        Array<var> knobs;
        auto knob = [&knobs] (const String& id, const String& name, double value, const String& text)
        {
            DynamicObject::Ptr k (new DynamicObject());
            k->setProperty ("id", id); k->setProperty ("name", name); k->setProperty ("value", value); k->setProperty ("text", text);
            knobs.add (var (k.get()));
        };
        knob ("level", "Level", (sd.gainDb + 60.0) / 72.0, sd.gainDb <= -59.9f ? String ("off") : (sd.gainDb >= 0.0f ? "+" : "") + String (sd.gainDb, 1) + " dB");
        const int pan = roundToInt (sd.pan * 50.0f);
        knob ("pan", "Pan", (sd.pan + 1.0) / 2.0, pan == 0 ? String ("C") : pan < 0 ? "L " + String (-pan) : "R " + String (pan));
        if (sd.kind == SlotDef::Kind::plugin && engine.isPluginAlive (input, program, si))
            if (auto* plugin = engine.getPlugin (input, program, si))
            {
                const auto& params = plugin->getParameters();
                std::vector<int> picked;
                if (sd.panelParams.empty()) picked = pickPanelParams (params, 6);
                else for (const auto& id : sd.panelParams) if (const int idx = plugin->findParameterIndex (id); idx >= 0 && picked.size() < 8) picked.push_back (idx);
                for (int idx : picked)
                    if (idx >= 0 && idx < (int) params.size())
                    {
                        const float v = plugin->getCachedParameterValue (idx);
                        knob ("p:" + String (idx), params[(size_t) idx].name, v, String (roundToInt (v * 100.0f)) + "%");
                    }
            }
        so->setProperty ("knobs", knobs);
        slots.add (var (so.get()));
    }
    o->setProperty ("slots", slots);
    return JSON::toString (var (o.get()), true);
}

String RemoteServer::transportJson() const
{
    DynamicObject::Ptr o (new DynamicObject());
    const auto& songs = engine.getSongs();
    if (! engine.hasTrack() && engine.songHasScenes (engine.currentSongIndex()))
    {
        // Scenes stand in for sections: the one in play, and the list to tap.
        const int sc = engine.currentSongIndex();
        o->setProperty ("has", true);
        o->setProperty ("scenes", true);
        o->setProperty ("song", sc);
        o->setProperty ("name", songs[(size_t) sc].name);
        o->setProperty ("running", false);
        o->setProperty ("ready", true);
        o->setProperty ("section", engine.getScene());
        o->setProperty ("queued", -1);
        o->setProperty ("vamping", false);
        Array<var> scenes;
        for (const auto& m : songs[(size_t) sc].markers)
        {
            DynamicObject::Ptr sec (new DynamicObject());
            sec->setProperty ("name", m.name);
            sec->setProperty ("vamp", false);
            scenes.add (var (sec.get()));
        }
        o->setProperty ("sections", scenes);
        return JSON::toString (var (o.get()), true);
    }
    const int song = engine.getTrackSong();
    o->setProperty ("has", engine.hasTrack());
    o->setProperty ("song", song);
    o->setProperty ("name", song >= 0 && song < (int) songs.size() ? songs[(size_t) song].name : String());
    o->setProperty ("running", engine.isTrackRunning());
    o->setProperty ("countIn", engine.isTrackCountingIn());
    o->setProperty ("pos", engine.getTrackPosition());
    o->setProperty ("len", engine.getTrackLength());
    o->setProperty ("ready", engine.isTrackReady());
    o->setProperty ("loading", engine.getTrackLoadProgress());
    o->setProperty ("section", engine.getTrackSection());
    o->setProperty ("queued", engine.getQueuedTrackSection());
    o->setProperty ("vamping", engine.isTrackVamping());
    o->setProperty ("click", song >= 0 && song < (int) songs.size() && songs[(size_t) song].click);
    o->setProperty ("metronome", song >= 0 && song < (int) songs.size() && songs[(size_t) song].stems.empty());
    Array<var> sections;
    for (int i = 0; i < engine.getTrackSectionCount(); ++i)
    {
        DynamicObject::Ptr sec (new DynamicObject());
        sec->setProperty ("name", engine.getTrackSectionName (i));
        sec->setProperty ("vamp", engine.isTrackSectionVamp (i));
        sections.add (var (sec.get()));
    }
    o->setProperty ("sections", sections);

    /* The MIDI files' parts: what each plays, whether it is muted, and the
       plugin to look at when one plays it -- so a part can be muted from the
       stand ("I'll play the piano myself tonight") and its sound opened. */
    Array<var> parts;
    const int track = engine.getTrackInput();
    if (song >= 0 && song < (int) songs.size() && track >= 0)
    {
        const int prog = songs[(size_t) song].programFor (track);
        const auto& setup = engine.getSetup();
        for (const auto& st : songs[(size_t) song].stems)
        {
            if (! st.isMidi() || prog < 0) continue;
            for (const auto& fp : partsOf (engine.getChartsFolder().getChildFile (st.file)))
            {
                DynamicObject::Ptr p (new DynamicObject());
                p->setProperty ("ch", fp.channel);
                p->setProperty ("name", fp.name);
                p->setProperty ("plays", partInstrumentName (engine, track, prog, fp.channel, fp.program));
                p->setProperty ("muted", engine.isPartMuted (track, prog, fp.channel));
                const int sl = engine.partInstrument (track, prog, fp.channel);
                const bool plugin = sl >= 0 && setup.inputs[(size_t) track].programs[(size_t) prog].slots[(size_t) sl].isPlugin();
                p->setProperty ("input", track);
                p->setProperty ("slot", plugin ? sl : -1);
                parts.add (var (p.get()));
            }
        }
    }
    o->setProperty ("parts", parts);
    return JSON::toString (var (o.get()), true);
}

String RemoteServer::levelsJson() const
{
    DynamicObject::Ptr root (new DynamicObject());
    Array<var> out;
    const auto& setup = engine.getSetup();
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        if (! setup.inputs[(size_t) i].isAudio()) continue;
        const auto l = engine.getInputLevel (i);
        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("input", i);
        o->setProperty ("left", l.left);
        o->setProperty ("right", l.right);
        o->setProperty ("clip", l.clipped);
        out.add (var (o.get()));
    }
    root->setProperty ("levels", out);
    return JSON::toString (var (root.get()), true);
}

String RemoteServer::healthJson() const
{
    DynamicObject::Ptr root (new DynamicObject());
    root->setProperty ("lateBlocks", engine.getLateBlockCount());
    root->setProperty ("blockSize", engine.getLastBlockSize());
    root->setProperty ("sampleRate", engine.getSampleRate());
    Array<var> plugins;
    const auto& setup = engine.getSetup();
    auto add = [&plugins] (const String& where, RemotePlugin* p)
    {
        if (p == nullptr) return;
        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("where", where);
        o->setProperty ("plugin", p->getName());
        o->setProperty ("alive", p->isAlive());
        o->setProperty ("missedBlocks", p->getMissedBlocks());
        plugins.add (var (o.get()));
    };
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        const auto& in = setup.inputs[(size_t) i];
        for (int p = 0; p < InputDef::numPrograms; ++p)
        {
            if (! engine.isProgramLoaded (i, p)) continue;
            const auto& def = in.programs[(size_t) p];
            const auto tag = in.name + " " + String (p).paddedLeft ('0', 3) + (def.name.isNotEmpty() ? " " + def.name : String());
            for (int s = 0; s < (int) def.slots.size(); ++s)
            {
                add (tag, engine.getPlugin (i, p, s, -1));
                for (int e = 0; e < (int) def.slots[(size_t) s].midiEffects.size(); ++e) add (tag + " > MIDI", engine.getPlugin (i, p, ProgramDef::midiChainOf (s), e));
                for (int e = 0; e < (int) def.slots[(size_t) s].effects.size(); ++e) add (tag + " > FX", engine.getPlugin (i, p, s, e));
            }
            for (int e = 0; e < (int) def.effects.size(); ++e) add (tag + " program FX", engine.getPlugin (i, p, -1, e));
        }
    }
    for (int o = 0; o < (int) setup.outputs.size(); ++o)
        for (int e = 0; e < (int) setup.outputs[(size_t) o].chain.effects.size(); ++e)
            add ("Output " + setup.outputs[(size_t) o].name, engine.getPlugin (Engine::outputAddress (o), 0, -1, e));
    root->setProperty ("plugins", plugins);
    return JSON::toString (var (root.get()), true);
}

/** The current program's slots, so the tablet can offer a window onto each one.

    Grouped by slot because that is how the setup is built and how the desktop
    labels things: "1: Kontakt 8" is the same slot in both views. */
String RemoteServer::nowJson() const
{
    const auto& setup = engine.getSetup();
    DynamicObject::Ptr root (new DynamicObject());
    root->setProperty ("revision", revision.load());
    root->setProperty ("transpose", engine.getTranspose());
    const int song = engine.currentSongIndex();
    root->setProperty ("song", song);
    root->setProperty ("songName", song >= 0 ? setup.songs[(size_t) song].name : String());

    auto fxObject = [] (int slot, int effect, const String& name, bool bypassed)
    {
        DynamicObject::Ptr so (new DynamicObject());
        so->setProperty ("slot", slot);
        so->setProperty ("effect", effect);
        so->setProperty ("name", name);
        so->setProperty ("bypassed", bypassed);
        return var (so.get());
    };

    Array<var> inputs;
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        const auto& in = setup.inputs[(size_t) i];
        if (in.isTrack()) continue;                 // its parts have their own sheet
        const int prog = in.currentProgram;
        const auto& def = in.programs[(size_t) prog];
        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("index", i);
        o->setProperty ("name", in.name);
        o->setProperty ("audio", in.isAudio());
        o->setProperty ("program", prog);
        o->setProperty ("programName", def.name);
        Array<var> instruments, effects;
        for (int s = 0; s < (int) def.slots.size(); ++s)
        {
            const auto& sd = def.slots[(size_t) s];
            DynamicObject::Ptr so (new DynamicObject());
            so->setProperty ("slot", s);
            so->setProperty ("name", sd.displayName());
            so->setProperty ("enabled", sd.enabled);
            so->setProperty ("gainDb", sd.gainDb);
            instruments.add (var (so.get()));
            // Its MIDI chain and its effects, in signal order, named after it.
            for (int e = 0; e < (int) sd.midiEffects.size(); ++e)
            {
                const auto& md = sd.midiEffects[(size_t) e];
                effects.add (fxObject (ProgramDef::midiChainOf (s), e, (md.isSend() ? "Send to " + md.portName : md.plugin.name) + " \xe2\x80\x94 MIDI chain of " + sd.displayName(), md.bypassed));
            }
            for (int e = 0; e < (int) sd.effects.size(); ++e)
                effects.add (fxObject (s, e, sd.effects[(size_t) e].plugin.name + " \xe2\x80\x94 effect on " + sd.displayName(), sd.effects[(size_t) e].bypassed));
        }
        for (int e = 0; e < (int) def.effects.size(); ++e)
            effects.add (fxObject (-1, e, def.effects[(size_t) e].plugin.name + (in.isAudio() ? " \xe2\x80\x94 effect on this input" : " \xe2\x80\x94 effect on the whole program"), def.effects[(size_t) e].bypassed));
        o->setProperty ("instruments", instruments);
        o->setProperty ("effects", effects);
        inputs.add (var (o.get()));
    }
    root->setProperty ("inputs", inputs);

    Array<var> outputs;
    for (int k = 0; k < (int) setup.outputs.size(); ++k)
    {
        const auto& od = setup.outputs[(size_t) k];
        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("index", k);
        o->setProperty ("name", od.name);
        o->setProperty ("gainDb", od.gainDb);
        Array<var> effects;
        for (int e = 0; e < (int) od.chain.effects.size(); ++e)
            effects.add (fxObject (-1, e, od.chain.effects[(size_t) e].plugin.name + " \xe2\x80\x94 effect on output " + od.name, od.chain.effects[(size_t) e].bypassed));
        o->setProperty ("effects", effects);
        outputs.add (var (o.get()));
    }
    root->setProperty ("outputs", outputs);
    return JSON::toString (var (root.get()), true);
}

String RemoteServer::slotsJson (int inputIndex) const
{
    DynamicObject::Ptr root (new DynamicObject());
    Array<var> out;

    const auto& setup = engine.getSetup();
    if (inputIndex >= 0 && inputIndex < (int) setup.inputs.size())
    {
        const auto& in = setup.inputs[(size_t) inputIndex];
        const int prog = in.currentProgram;
        const auto& def = in.programs[(size_t) prog];

        if (in.isAudio())
        {
            // An audio input has no instruments; its windows are the chain's effects.
            for (int e = 0; e < (int) def.effects.size(); ++e)
            {
                const auto& fx = def.effects[(size_t) e];
                if (fx.plugin.name.isEmpty()) continue;
                DynamicObject::Ptr so (new DynamicObject());
                so->setProperty ("slot", -1);
                so->setProperty ("effect", e);
                so->setProperty ("name", fx.plugin.name + " \xe2\x80\x94 effect on this input");
                so->setProperty ("enabled", ! fx.bypassed);
                out.add (var (so.get()));
            }
        }
        else
        {
            /* Each instrument, then its insert effects, then the program's own
               chain: the order the desktop shows them in, so "1 > FX2" means
               the same thing on both. */
            auto add = [&out] (int slot, int effect, const String& name, bool enabled)
            {
                DynamicObject::Ptr so (new DynamicObject());
                so->setProperty ("slot", slot);
                so->setProperty ("effect", effect);
                so->setProperty ("name", name);
                so->setProperty ("enabled", enabled);
                out.add (var (so.get()));
            };
            for (int sIdx = 0; sIdx < (int) def.slots.size(); ++sIdx)
            {
                const auto& slot = def.slots[(size_t) sIdx];
                // In the slot's signal order: its MIDI processors (a send has no window), the sound, its effects.
                for (int e = 0; e < (int) slot.midiEffects.size(); ++e)
                    if (const auto& md = slot.midiEffects[(size_t) e]; ! md.isSend() && md.plugin.name.isNotEmpty())
                        add (ProgramDef::midiChainOf (sIdx), e, md.plugin.name + " \xe2\x80\x94 MIDI chain of " + slot.displayName(), ! md.bypassed);
                // An external instrument has no window of its own; its effects still do.
                if (slot.plugin.name.isNotEmpty() && slot.isPlugin())
                    add (sIdx, -1, slot.plugin.name + " (instrument " + String (sIdx + 1) + ")", slot.enabled);
                for (int e = 0; e < (int) slot.effects.size(); ++e)
                    if (slot.effects[(size_t) e].plugin.name.isNotEmpty())
                        add (sIdx, e, slot.effects[(size_t) e].plugin.name + " \xe2\x80\x94 effect on " + slot.displayName(),
                             ! slot.effects[(size_t) e].bypassed);
            }
            for (int e = 0; e < (int) def.effects.size(); ++e)
                if (def.effects[(size_t) e].plugin.name.isNotEmpty())
                    add (-1, e, def.effects[(size_t) e].plugin.name + " \xe2\x80\x94 effect on the whole program", ! def.effects[(size_t) e].bypassed);
        }
        // And the master chain of the output this input lands on.
        if (in.output >= 0 && in.output < (int) setup.outputs.size())
        {
            const auto& od = setup.outputs[(size_t) in.output];
            for (int e = 0; e < (int) od.chain.effects.size(); ++e)
                if (od.chain.effects[(size_t) e].plugin.name.isNotEmpty())
                {
                    DynamicObject::Ptr so (new DynamicObject());
                    so->setProperty ("slot", -1);
                    so->setProperty ("effect", e);
                    so->setProperty ("bus", in.output);
                    so->setProperty ("name", od.chain.effects[(size_t) e].plugin.name + " \xe2\x80\x94 effect on output " + od.name);
                    so->setProperty ("enabled", ! od.chain.effects[(size_t) e].bypassed);
                    out.add (var (so.get()));
                }
        }
    }

    root->setProperty ("slots", out);
    return JSON::toString (var (root.get()), true);
}

//==============================================================================
/** Writes raw bytes: the body is already UTF-8, and Content-Length counts bytes,
    not characters. Going through juce::String here once mangled an em-dash in the
    page, because the literal was reinterpreted rather than passed through. */
static void sendBytes (StreamingSocket& s, const String& status, const String& type,
                       const void* body, int len)
{
    String head;
    head << "HTTP/1.1 " << status << "\r\n"
         << "Content-Type: " << type << "\r\n"
         << "Content-Length: " << len << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n";
    const auto h = head.toRawUTF8();
    s.write (h, (int) strlen (h));
    if (len > 0) s.write (body, len);
}

static void sendResponse (StreamingSocket& s, const String& status, const String& type, const String& body)
{
    const auto utf8 = body.toRawUTF8();
    const int len = (int) strlen (utf8);
    String head;
    head << "HTTP/1.1 " << status << "\r\n"
         << "Content-Type: " << type << "\r\n"
         << "Content-Length: " << len << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n";
    s.write (head.toRawUTF8(), (int) strlen (head.toRawUTF8()));
    if (len > 0) s.write (utf8, len);
}

/* Chrome refuses to offer "install" unless the page registers a service worker
   with a fetch handler, so there has to be one -- but caching a stage tool's
   pages would be actively harmful: a tablet showing a stale program list is worse
   than one showing none. This worker therefore always goes to the network and
   only falls back to a cached shell when the network is gone, which is the
   honest behaviour for something whose whole job is to reflect live state. */
static const char* kServiceWorker = R"JS(
const SHELL = 'stagerig-shell-v1';

self.addEventListener('install', e => {
  e.waitUntil(caches.open(SHELL).then(c => c.addAll(['./'])).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys()
    .then(ks => Promise.all(ks.filter(k => k !== SHELL).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);
  // Never serve state or commands from a cache: stale is worse than absent.
  if (url.pathname.startsWith('/api/')) return;

  e.respondWith(
    fetch(e.request)
      .then(r => {
        if (r && r.ok && e.request.method === 'GET' && url.pathname === '/')
          caches.open(SHELL).then(c => c.put('./', r.clone()));
        return r;
      })
      .catch(() => caches.match(e.request).then(hit => hit || caches.match('./')))
  );
});
)JS";

void RemoteServer::handle (StreamingSocket& sock, bool& takeOver)
{
    takeOver = false;
    char buf[4096] = {};
    const int got = sock.read (buf, sizeof (buf) - 1, false);
    if (got <= 0) return;
    const String request (CharPointer_UTF8 (buf), (size_t) got);
    const String line = request.upToFirstOccurrenceOf ("\r\n", false, false);
    const String path = line.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (" ", false, false);

    if (path.startsWith ("/manifest.webmanifest")) { sendBytes (sock, "200 OK", "application/manifest+json", kManifest, (int) strlen (kManifest)); return; }
    if (path.startsWith ("/sw.js"))
    {
        sendBytes (sock, "200 OK", "text/javascript; charset=utf-8",
                   kServiceWorker, (int) strlen (kServiceWorker));
        return;
    }

    // The app icon, so an installed web app has one. Chrome wants at least 192
    // before it will offer to install, and 512 for the splash screen.
    if (path.startsWith ("/icon"))
    {
        const void* src = BinaryData::stagerig256_png;
        int srcSize = BinaryData::stagerig256_pngSize;
        int resize = 0;

        if (path.startsWith ("/icon-512"))
        {
            src = BinaryData::stagerig512_png;
            srcSize = BinaryData::stagerig512_pngSize;
        }
        else if (path.startsWith ("/icon-192"))
        {
            // No 192 asset: scale the 512 down, which is sharper than scaling up.
            src = BinaryData::stagerig512_png;
            srcSize = BinaryData::stagerig512_pngSize;
            resize = 192;
        }

        MemoryOutputStream png;
        if (auto img = ImageCache::getFromMemory (src, srcSize); img.isValid())
        {
            if (resize > 0)
                img = img.rescaled (resize, resize, Graphics::highResamplingQuality);
            PNGImageFormat().writeImageToStream (img, png);
        }
        String head;
        head << "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " << (int) png.getDataSize()
             << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        sock.write (head.toRawUTF8(), (int) strlen (head.toRawUTF8()));
        sock.write (png.getData(), (int) png.getDataSize());
        return;
    }

    if (! authorised (path))
    {
        // The page itself is harmless without a token; the API is not.
        if (path.startsWith ("/api/")) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
    }

    /* A plugin window's stream, relayed so it uses this port rather than one of
       its own. The port is in the path because the tablet has no other way to
       say which window it wants. */
    if (path.startsWith ("/view/"))
    {
        const auto rest = path.fromFirstOccurrenceOf ("/view/", false, false);
        const int wanted = rest.upToFirstOccurrenceOf ("/", false, false).getIntValue();

        /* Our own page, served whether or not the window is still shown: a
           tablet that reloads it after the window closed should see the page
           say so, not a bare 404. The page asks for a file under the mirror
           to learn whether it is alive. Everything else goes to the bridge. */
        if (rest.fromFirstOccurrenceOf ("/", false, false).startsWith ("plugin.html"))
        {
            sendBytes (sock, "200 OK", "text/html; charset=utf-8",
                       kPluginViewHtml, (int) strlen (kPluginViewHtml));
            return;
        }

        bool known = false;
        for (const auto& s : PluginView::active())
            if (s.webPort == wanted) known = true;

        if (! known)
        {
            sendResponse (sock, "404 Not Found", "text/plain", "This plugin window is no longer being shown. Go back to the StageRig tab and press GUI again.");
            return;
        }

        // Strip the /view/<port> prefix so the bridge sees the path it expects.
        auto forwarded = request;
        const auto prefix = "/view/" + String (wanted);
        forwarded = forwarded.replaceFirstOccurrenceOf (prefix, String());

        /* Hand the socket to its own thread. A websocket stays open for as long
           as someone is looking at the plugin, and this server accepts one
           connection at a time: relaying inline wedged everything, the page's
           own polling included, the moment a window was opened. */
        takeOver = true;
        std::thread ([this, sockPtr = &sock, forwarded, wanted]
        {
            relayToPluginView (*sockPtr, forwarded, wanted);
            sockPtr->close();
            delete sockPtr;
        }).detach();
        return;
    }

    
    if (path.startsWith ("/api/health"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        String body;
        if (produceOnMessageThread ([this] { return healthJson(); }, body, 2000))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendBusy (sock, sendResponse);
        return;
    }

    if (path.startsWith ("/api/levels"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        String body;
        if (produceOnMessageThread ([this] { return levelsJson(); }, body, 500))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendBusy (sock, sendResponse);
        return;
    }

    if (path.startsWith ("/api/state"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        String body;
        if (produceOnMessageThread ([this] { return stateJson(); }, body, 2000))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendResponse (sock, "503 Service Unavailable", "application/json", "{\"error\":\"StageRig is busy; try again.\"}");
        return;
    }
    if (path.startsWith ("/api/select"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int input = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        const int prog  = path.fromFirstOccurrenceOf ("program=", false, false).getIntValue();
        // the engine is not thread-safe for this: do it on the message thread
        MessageManager::callAsync ([this, input, prog] { engine.selectProgram (input, prog); });
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    /* Songs and sets. Each one nudges the revision so every other tablet
       redraws on its next poll rather than showing a stale set list. */
    if (path.startsWith ("/api/setedit"))
    {
        /* POST ...?do=new&name=        a set list, made current, to fill
                 do=duplicate&index=&name=   a copy, made current
                 do=rename&index=&name=
                 do=delete&index=
                 do=add&index=&song=      the song on the end
                 do=remove&index=&pos=
                 do=move&index=&pos=&delta=
           Set lists built from the stand, each step saved at once. */
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        if (! settings.getBoolValue ("remoteEditing", true)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"Editing from the tablet is off (Tablet window on the computer).\"}"); return; }
        String what, name;
        int index = -1, song = -1, pos = -1, delta = 0;
        for (const auto& kv : StringArray::fromTokens (path.fromFirstOccurrenceOf ("?", false, false), "&", {}))
        {
            const auto key = kv.upToFirstOccurrenceOf ("=", false, false);
            const auto value = URL::removeEscapeChars (kv.fromFirstOccurrenceOf ("=", false, false).replace ("+", " "));
            if (key == "do")         what = value;
            else if (key == "name")  name = value.trim();
            else if (key == "index") index = value.getIntValue();
            else if (key == "song")  song = value.getIntValue();
            else if (key == "pos")   pos = value.getIntValue();
            else if (key == "delta") delta = value.getIntValue();
        }
        MessageManager::callAsync ([this, what, name, index, song, pos, delta]
        {
            if (what == "new")            engine.selectSet (engine.addSet (name));
            else if (what == "duplicate") { if (const int n = engine.duplicateSet (index, name); n >= 0) engine.selectSet (n); }
            else if (what == "rename")    engine.setSetName (index, name);
            else if (what == "delete")    engine.removeSet (index);
            else if (what == "add")       engine.addSongToSet (index, song);
            else if (what == "remove")    engine.removeFromSet (index, pos);
            else if (what == "move")      engine.moveInSet (index, pos, delta);
        });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/songkey"))
    {
        // POST ...?song=<index>&root=<0..11, -1 for none>&mode=<scale>: the song's key, changed from the stand.
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        auto number = [&path] (const char* key, int fallback)
        {
            for (const auto& kv : StringArray::fromTokens (path.fromFirstOccurrenceOf ("?", false, false), "&", {}))
                if (kv.startsWith (String (key) + "=")) return kv.fromFirstOccurrenceOf ("=", false, false).getIntValue();
            return fallback;
        };
        const int song = number ("song", -1), root = number ("root", -1), mode = number ("mode", 0);
        MessageManager::callAsync ([this, song, root, mode] { engine.setSongKey (song, root, mode); });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/song"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int song = path.fromFirstOccurrenceOf ("index=", false, false).getIntValue();
        MessageManager::callAsync ([this, song] { engine.selectSong (song); });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/panel"))
    {
        // GET ...?input=  the knobs of that input's current program, with their values now.
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int input = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        String body;
        if (produceOnMessageThread ([this, input] { return panelJson (input); }, body, 2000))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendBusy (sock, sendResponse);
        return;
    }
    if (path.startsWith ("/api/knob"))
    {
        /* POST ...?input=&slot=&id=&value=   a knob turned on the stand: level, pan, or p:<index> a plugin parameter, value 0..1.
           The parameter is set with a short timeout, like the Stage view's knobs: a plugin that has stopped answering costs a moment. */
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        auto field = [&path] (const char* name) { return path.fromFirstOccurrenceOf (String (name) + "=", false, false).upToFirstOccurrenceOf ("&", false, false); };
        const int input = field ("input").getIntValue(), slot = field ("slot").getIntValue();
        const String id = URL::removeEscapeChars (field ("id"));
        const double value = jlimit (0.0, 1.0, field ("value").getDoubleValue());
        MessageManager::callAsync ([this, input, slot, id, value]
        {
            const auto& inputs = engine.getSetup().inputs;
            if (input < 0 || input >= (int) inputs.size()) return;
            const int program = inputs[(size_t) input].currentProgram;
            if (id == "level")    engine.setSlotGainDb (input, program, slot, (float) (-60.0 + value * 72.0));
            else if (id == "pan") engine.setSlotPan (input, program, slot, (float) (value * 2.0 - 1.0));
            else if (id.startsWith ("p:") && engine.isPluginAlive (input, program, slot))
                if (auto* plugin = engine.getPlugin (input, program, slot))
                    plugin->setParameterValue (id.substring (2).getIntValue(), (float) value, 100);
        });
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/scene"))
    {
        /* POST ...?do=add        a new scene on the song in play, holding what is loaded now
                 do=capture     the scene in play takes what is loaded now
           Only while the computer allows editing from the tablet. */
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        if (! settings.getBoolValue ("remoteEditing", true)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"Editing from the tablet is off (Tablet window on the computer).\"}"); return; }
        const auto what = path.fromFirstOccurrenceOf ("do=", false, false).upToFirstOccurrenceOf ("&", false, false);
        String body;
        if (produceOnMessageThread ([this, what]
            {
                const int song = engine.currentSongIndex();
                if (what == "add") engine.addScene (song, {});
                else if (what == "capture") engine.captureScene (song, engine.getScene());
                return transportJson();
            }, body, 3000))
        {
            ++revision;
            sendResponse (sock, "200 OK", "application/json", body);
        }
        else
            sendBusy (sock, sendResponse);
        return;
    }
    if (path.startsWith ("/api/transport"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        // POST ...?do=toggle|start|stop changes it; any request answers with where it is.
        const auto what = path.fromFirstOccurrenceOf ("do=", false, false).upToFirstOccurrenceOf ("&", false, false);
        String body;
        const int sectionIndex = path.fromFirstOccurrenceOf ("index=", false, false).getIntValue();
        if (produceOnMessageThread ([this, what, sectionIndex]
            {
                if (what == "toggle")       engine.toggleTrack();
                else if (what == "start")   engine.startTrack();
                else if (what == "stop")    engine.stopTrack();
                else if (what == "next")    engine.advanceTrack();
                else if (what == "section") engine.queueTrackSection (sectionIndex);
                else if (what == "click")
                {
                    const int song = engine.getTrackSong();
                    if (song >= 0 && song < (int) engine.getSongs().size())
                        engine.setSongClick (song, ! engine.getSongs()[(size_t) song].click, engine.getSongs()[(size_t) song].beatsPerBar);
                }
                return transportJson();
            }, body, 3000))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendBusy (sock, sendResponse);
        return;
    }

    if (path.startsWith ("/api/part"))
    {
        // POST ...?ch=N&muted=0|1 mutes a part of the armed song's MIDI files; answers with the transport.
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int ch = path.fromFirstOccurrenceOf ("ch=", false, false).getIntValue();
        const bool muted = path.fromFirstOccurrenceOf ("muted=", false, false).getIntValue() != 0;
        String body;
        if (produceOnMessageThread ([this, ch, muted]
            {
                const int track = engine.getTrackInput(), song = engine.getTrackSong();
                if (track >= 0 && song >= 0 && song < (int) engine.getSongs().size())
                    if (const int prog = engine.getSongs()[(size_t) song].programFor (track); prog >= 0)
                        engine.setPartMuted (track, prog, ch, muted);
                return transportJson();
            }, body, 3000))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendBusy (sock, sendResponse);
        return;
    }

    if (path.startsWith ("/api/setlist"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int set = path.fromFirstOccurrenceOf ("index=", false, false).getIntValue();
        MessageManager::callAsync ([this, set] { engine.selectSet (set); });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/setsong"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int pos = path.fromFirstOccurrenceOf ("pos=", false, false).getIntValue();
        MessageManager::callAsync ([this, pos] { engine.selectSongInSet (pos); });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/step"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const bool back = path.contains ("dir=-1");
        MessageManager::callAsync ([this, back] { if (back) engine.previousSong(); else engine.nextSong(); });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/now"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        String body;
        if (produceOnMessageThread ([this] { return nowJson(); }, body, 2000))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendBusy (sock, sendResponse);
        return;
    }
    {
        /* Live changes from a client -- an instrument on or off or louder, an
           effect bypassed, an output's volume, the transpose, the song
           captured -- each one engine call on the message thread, acknowledged
           at once. The page itself no longer offers them; the routes stay for
           anything else on the network that wants them. */
        auto number = [&path] (const char* key, double fallback = 0.0)
        {
            for (const auto& kv : StringArray::fromTokens (path.fromFirstOccurrenceOf ("?", false, false), "&", {}))
                if (kv.startsWith (String (key) + "=")) return kv.fromFirstOccurrenceOf ("=", false, false).getDoubleValue();
            return fallback;
        };
        auto has = [&path] (const char* key) { return path.contains ("?" + String (key) + "=") || path.contains ("&" + String (key) + "="); };
        auto post = [&] (const char* route, std::function<void()> change)
        {
            if (! path.startsWith (route)) return false;
            if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return true; }
            MessageManager::callAsync (std::move (change));
            ++revision;
            sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
            return true;
        };
        auto programOf = [this] (int in) { const auto& inputs = engine.getSetup().inputs; return in >= 0 && in < (int) inputs.size() ? inputs[(size_t) in].currentProgram : -1; };
        const int in = (int) number ("input", -1), slot = (int) number ("slot", -1), effect = (int) number ("effect", -1), output = (int) number ("output", -1);
        const bool on = number ("on") != 0.0;
        const float db = (float) number ("db");
        if (post ("/api/slotenable", [this, programOf, in, slot, on] { engine.setSlotEnabled (in, programOf (in), slot, on); })) return;
        if (post ("/api/slotgain",   [this, programOf, in, slot, db] { engine.setSlotGainDb (in, programOf (in), slot, db); })) return;
        if (has ("output") && post ("/api/bypass", [this, output, effect, on] { engine.setEffectBypassed (Engine::outputAddress (output), 0, -1, effect, ! on); })) return;
        if (post ("/api/bypass",     [this, programOf, in, slot, effect, on] { engine.setEffectBypassed (in, programOf (in), slot, effect, ! on); })) return;
        if (post ("/api/outgain",    [this, output, db] { engine.setOutputGainDb (output, db); })) return;
        if (post ("/api/transpose",  [this, st = (int) number ("st")] { engine.setTranspose (st); })) return;
        if (post ("/api/savesong",   [this] { if (const int s = engine.currentSongIndex(); s >= 0) engine.captureSong (s); })) return;
    }
    if (path.startsWith ("/api/slots"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int input = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        String body;
        if (produceOnMessageThread ([this, input] { return slotsJson (input); }, body, 2000))
            sendResponse (sock, "200 OK", "application/json", body);
        else
            sendResponse (sock, "503 Service Unavailable", "application/json", "{\"error\":\"StageRig is busy; try again.\"}");
        return;
    }
    if (path.startsWith ("/api/tap"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }

        /* Tapped on the tablet. The tap has to be timed where it happens, and it
           happens here: the round trip from a music stand is a few milliseconds
           on a local network, far below the precision of a human finger. What
           would ruin it is waiting for the next poll, so this answers with the
           new tempo rather than making the page wait a second to see it. */
        auto bpm  = std::make_shared<double> (0.0);
        auto done = std::make_shared<WaitableEvent>();
        MessageManager::callAsync ([this, bpm, done]
        {
            *bpm = engine.tapTempo();
            if (*bpm <= 0.0) *bpm = engine.getTempoBpm();
            done->signal();
        });
        if (! done->wait (500))
        {
            sendResponse (sock, "503 Service Unavailable", "application/json", "{\"error\":\"StageRig is busy; try again.\"}");
            return;
        }
        ++revision;

        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("tempo", *bpm);
        sendResponse (sock, "200 OK", "application/json", JSON::toString (var (o.get()), true));
        return;
    }
    if (path.startsWith ("/api/settempo"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const double bpm = path.fromFirstOccurrenceOf ("bpm=", false, false).getDoubleValue();
        MessageManager::callAsync ([this, bpm] { engine.setTempoBpm (bpm); engine.resetTapTempo(); });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/pluginview"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        const int input  = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        const int slot   = path.fromFirstOccurrenceOf ("slot=", false, false).getIntValue();
        const int effect = path.contains ("effect=") ? path.fromFirstOccurrenceOf ("effect=", false, false).getIntValue() : -1;
        const int bus    = path.contains ("bus=")    ? path.fromFirstOccurrenceOf ("bus=", false, false).getIntValue()    : -1;

        /* Mirrors the plugin's own window. Needed because some plugins cannot be
           followed any other way: Kontakt's host-visible parameters are MIDI
           controller inputs, so reading one back gives what was last written to
           it, never where the drawbar actually is. */
        auto title = std::make_shared<String>();
        auto error = std::make_shared<String>();
        auto ready = std::make_shared<WaitableEvent>();
        MessageManager::callAsync ([this, input, slot, effect, bus, title, error, ready]
        {
            const auto& setup = engine.getSetup();
            if (bus >= 0 && bus < (int) setup.outputs.size())
            {
                // An output's master chain, reached from any input that lands on it.
                const auto& od = setup.outputs[(size_t) bus];
                auto* plugin = effect >= 0 && effect < (int) od.chain.effects.size()
                             ? engine.getPlugin (Engine::outputAddress (bus), 0, -1, effect) : nullptr;
                const auto name = "Output " + od.name + " / FX" + String (effect + 1) + " / " + (plugin != nullptr ? plugin->getName() : String());
                if (plugin == nullptr || ! plugin->isAlive())
                    *error = "That plugin is not loaded on the computer.";
                else if (! plugin->isEditorOpen() && ! plugin->showEditor (name))
                    *error = "Could not open that plugin's window.";
                else
                {
                    *title = name;
                    plugin->keepEditorOnTop (true);
                }
            }
            else if (input >= 0 && input < (int) setup.inputs.size())
            {
                const auto& in = setup.inputs[(size_t) input];
                const int prog = in.currentProgram;
                const auto& def = in.programs[(size_t) prog];
                // An instrument (effect -1), one of its insert effects, or (slot -1) a program effect.
                const auto* chain = def.chainFor (slot);
                const bool valid = effect < 0 ? slot >= 0 && slot < (int) def.slots.size()
                                              : chain != nullptr && effect < (int) chain->size();
                if (valid)
                {
                    auto* plugin = engine.getPlugin (input, prog, slot, effect);
                    if (plugin == nullptr || ! plugin->isAlive())
                        *error = "That plugin is not loaded on the computer.";
                    else if (! plugin->isEditorOpen() && ! plugin->showEditor (
                                 in.name + " / " + String (prog).paddedLeft ('0', 3)
                                 + "  " + def.name + " / " + plugin->getName()))
                        *error = "Could not open that plugin's window.";
                    else
                    {
                        *title = in.name + " / " + String (prog).paddedLeft ('0', 3)
                              + "  " + def.name + " / " + plugin->getName();
                        /* Whether just opened or already open: from now on a
                           tablet is looking at it, and taps only land if it is
                           not covered. See keepEditorOnTop in the host. */
                        plugin->keepEditorOnTop (true);
                    }
                }
            }
            ready->signal();
        });
        const bool got = ready->wait (20000);

        DynamicObject::Ptr o (new DynamicObject());
        if (! got)
        {
            o->setProperty ("error", "StageRig is busy; try again.");
        }
        else if (error->isNotEmpty() || title->isEmpty())
        {
            o->setProperty ("error", error->isNotEmpty() ? *error : String ("That instrument has no plugin window."));
        }
        else
        {
            String startError;
            // Give the window a moment to appear before looking for it.
            for (int i = 0; i < 20 && PluginView::findWindow (*title) == 0; ++i)
                Thread::sleep (100);

            const auto session = PluginView::start (*title, startError);
            if (! session.isValid()) o->setProperty ("error", startError);
            else                     o->setProperty ("port", session.webPort);
        }
        sendResponse (sock, "200 OK", "application/json", JSON::toString (var (o.get()), true));
        return;
    }
    if (path.startsWith ("/chart.html"))
    {
        sendBytes (sock, "200 OK", "text/html; charset=utf-8", kChartHtml, (int) strlen (kChartHtml));
        return;
    }
    if (path.startsWith ("/chart/"))
    {
        /* /chart/<song>/<n>            the file itself
           /chart/<song>/<n>/page/<p>   one rendered page of a PDF, from 1
           Everything is resolved through the model by index; the only file
           name used is the one the setup recorded, inside the charts folder. */
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "text/plain", "bad token"); return; }
        const auto parts = StringArray::fromTokens (path.upToFirstOccurrenceOf ("?", false, false).fromFirstOccurrenceOf ("/chart/", false, false), "/", {});
        const int song = parts.size() > 0 ? parts[0].getIntValue() : -1;
        const int n    = parts.size() > 1 ? parts[1].getIntValue() : -1;
        const int page = (parts.size() > 3 && parts[2] == "page") ? parts[3].getIntValue() : 0;

        File file;
        {
            const auto& songs = engine.getSongs();
            if (song >= 0 && song < (int) songs.size() && n >= 0 && n < (int) songs[(size_t) song].charts.size())
            {
                const auto& ref = songs[(size_t) song].charts[(size_t) n];
                const auto folder = engine.getChartsFolder();
                if (folder != File() && ref.file.isNotEmpty()
                    && ! ref.file.containsAnyOf ("/\\") && ! ref.file.startsWith ("."))
                {
                    file = folder.getChildFile (ref.file);
                    if (page > 0)
                    {
                        const auto pages = chartPages (file);
                        file = page <= pages.size() ? pages[page - 1] : File();
                    }
                    if (file != File() && ! file.isAChildOf (folder)) file = File();
                }
            }
        }
        if (file == File() || ! file.existsAsFile())
        {
            sendResponse (sock, "404 Not Found", "text/plain", "No such chart.");
            return;
        }
        // A chord chart that names its key is shown in the song's, when the song has one.
        if (page == 0 && chartKindFor (file) == "chordpro")
            if (const auto& sg = engine.getSongs()[(size_t) song]; sg.hasKey())
            {
                sendResponse (sock, "200 OK", chartContentTypeFor (file),
                              transposeChordPro (file.loadFileAsString(), sg.keyRoot, MidiFx::keyPrefersFlats (sg.keyRoot, sg.keyMode)));
                return;
            }
        MemoryBlock data;
        if (! file.loadFileAsData (data)) { sendResponse (sock, "500 Internal Server Error", "text/plain", "Could not read the chart."); return; }
        sendBytes (sock, "200 OK", chartContentTypeFor (file), static_cast<const char*> (data.getData()), (int) data.getSize());
        return;
    }
    if (path.startsWith ("/api/panic"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"That code is not valid.\"}"); return; }
        MessageManager::callAsync ([this] { engine.panic(); });
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    sendBytes (sock, "200 OK", "text/html; charset=utf-8", kIndexHtml, (int) strlen (kIndexHtml));
}

void RemoteServer::run()
{
    while (! threadShouldExit() && running.load())
    {
        if (listener == nullptr) break;
        std::unique_ptr<StreamingSocket> conn (listener->waitForNextConnection());
        /* Null means the listener failed, not that nobody called. Without a
           pause this would spin a whole core if, say, file descriptors ran
           out -- mid-set, with the audio thread competing for the machine. */
        if (conn == nullptr) { Thread::sleep (50); continue; }

        // A relayed plugin stream takes the socket with it and closes it itself.
        bool takenOver = false;
        if (conn->waitUntilReady (true, 2000) == 1)
            handle (*conn, takenOver);

        if (takenOver) { conn.release(); continue; }
        conn->close();
    }
}

} // namespace perf
