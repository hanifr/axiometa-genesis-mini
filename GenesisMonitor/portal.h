#pragma once
// portal.h — commissioning over a browser: WiFi and MQTT only.
//
// MIRRORS the sibling relay portal so commissioning feels the same across the
// estate, and departs from it in four deliberate ways:
//
//  1. IT SCANS. that portal makes you type the SSID from memory; this lists
//     what the radio can hear and you pick one. A typo in an SSID is
//     indistinguishable from a network being out of range, and that is a
//     miserable thing to debug on site.
//
//  2. IT IS SMALL. that portal is 42.8 KB of HTML because it also carries
//     timings, relay control and diagnostics. Thresholds and power mode belong
//     on the slider -- they are small numbers, adjusted often, and a physical
//     control beats a form. So this page carries only the long strings you
//     enter once. The handbook is blunt that 160 KB of near-identical embedded
//     HTML across seven receivers is what the ground station exists to end;
//     the way to respect that is to keep each page to its job.
//
//  3. A BLANK SECRET MEANS KEEP. The page never sends a stored password back
//     to the browser, so its field always renders empty -- and a handler that
//     wrote whatever it received would erase the password every time somebody
//     edited the broker port. That exact fault is documented in chapter 8 of
//     the handbook. Blank means keep; there is a separate box to clear.
//
//  4. THE AP PASSWORD IS PER DEVICE, from ESP32Eco's ap_password.h, rather
//     than the "12345678" eleven sketches in this estate used to share.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "ap_password.h"          // ESP32Eco — one AP password per device
#include "globals.h"

static WebServer portalServer(80);

// One server, two exposures, and the difference matters.
//
//   AP mode      the setup portal: WiFi, broker, credentials. Reachable only
//                by someone who has joined this device's own access point,
//                whose password is printed on its screen.
//   station mode the SIGN only: availability and a notice. Reachable by the
//                whole office network.
//
// The config handlers therefore refuse to run unless the AP is up. Serving the
// WiFi form on a campus LAN would let anyone who found the device repoint it,
// and the page also displays the device's own AP password.
static bool portalApMode  = false;
static bool portalStarted = false;
static char apPass[64] = "";
static char apSsid[24] = "";
static bool portalUp = false;

static const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Genesis Monitor setup</title>
<style>
:root{color-scheme:dark}
body{margin:0;padding:18px;background:#0d1317;color:#e5ebee;
 font:15px/1.5 -apple-system,system-ui,sans-serif;max-width:32rem}
h1{font-size:1.15rem;margin:0 0 2px}
p.sub{color:#7d8e96;margin:0 0 20px;font-size:.85rem}
fieldset{border:1px solid #27343b;border-radius:6px;padding:14px;margin:0 0 16px}
legend{color:#3fb3b8;font-size:.78rem;letter-spacing:.06em;text-transform:uppercase;padding:0 6px}
label{display:block;margin:10px 0 4px;font-size:.82rem;color:#afbdc4}
input,select{width:100%;box-sizing:border-box;padding:9px;border-radius:5px;
 border:1px solid #27343b;background:#151e24;color:#e5ebee;font-size:15px}
button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:5px;
 background:#3fb3b8;color:#06212a;font-size:15px;font-weight:600}
button.ghost{background:#1b262c;color:#3fb3b8;margin-top:8px}
.hint{color:#7d8e96;font-size:.76rem;margin-top:5px}
.row{display:flex;gap:10px}.row>*{flex:1}
#msg{margin-top:14px;padding:10px;border-radius:5px;display:none}
.ok{background:#12301f;color:#68c08c}.bad{background:#341a17;color:#f08a80}
</style>
<h1>Genesis Monitor</h1>
<p class=sub id=eui>&nbsp;</p>

<fieldset><legend>Network</legend>
<label>Network name</label>
<select id=ssid><option value="">scanning…</option></select>
<button class=ghost type=button onclick=scan()>Scan again</button>
<label>Password</label>
<input id=wpass type=password autocomplete=off placeholder="leave blank to keep">
<div class=hint>Blank keeps the stored password &mdash; it is never sent to
this page. If none is stored yet, blank is refused rather than saving a network
the device cannot join.</div>
</fieldset>

<fieldset><legend>Reporting</legend>
<label>Broker</label><input id=host placeholder=broker.example.com>
<div class=row><div><label>Port</label><input id=port type=number></div>
<div><label>Publish every (s)</label><input id=pub type=number></div></div>
<div class=row><div><label>Broker user</label>
<input id=muser autocomplete=off placeholder="blank = anonymous"></div>
<div><label>Broker password</label>
<input id=mpass type=password autocomplete=off placeholder="leave blank to keep"></div></div>
<label>Publish topic</label><input id=topic placeholder="sensors/{eui}/data">
<div class=hint>{eui} is replaced with this device's id. Alarms, availability
and commands sit <em>beside</em> this topic, not under it: replacing the last
segment gives <code>/event</code>, <code>/avail</code> and <code>/cmd</code>.
</div>
</fieldset>

<fieldset><legend>Lab sign</legend>
<label><input type=checkbox id=signset onchange="sp()"> Change sign password</label>
<input id=signpw type=password autocomplete=new-password disabled
       placeholder="tick the box to set one">
<div class=hint>Needed to change availability from the office network. Set it
here, on this device's own Wi-Fi, so it is never shown on the panel facing the
corridor. Leave the box unticked and nothing is changed &mdash; if a password
manager fills the field, it is ignored. Lost it? Send
<code>#SIGNPASS,clear</code> over USB to fall back to this device's Wi-Fi
password.</div>
</fieldset>

<button type=button onclick=save()>Save and restart</button>
<div id=msg></div>

<script>
function show(t,ok){var m=document.getElementById('msg');m.textContent=t;
 m.className=ok?'ok':'bad';m.style.display='block'}
function scan(){var s=document.getElementById('ssid');
 s.innerHTML='<option value="">scanning…</option>';
 fetch('/scan').then(r=>r.json()).then(function(n){
  s.innerHTML='';
  if(n&&n.error){s.innerHTML='<option value="">scan failed</option>';
   show(n.error,0);return}
  if(!n.length){s.innerHTML='<option value="">no networks in range</option>';return}
  n.forEach(function(x){var o=document.createElement('option');
   o.value=x.ssid;o.textContent=x.ssid+'  ('+x.rssi+' dBm'+(x.open?', open':'')+')';
   s.appendChild(o)})}).catch(function(){show('scan failed',0)})}
function sp(){var c=document.getElementById('signset').checked;
 var f=document.getElementById('signpw');f.disabled=!c;
 if(!c)f.value='';if(c)f.focus()}
function load(){fetch('/status').then(r=>r.json()).then(function(d){
 document.getElementById('eui').textContent=d.eui+' · '+d.fw;
 document.getElementById('host').value=d.host;
 document.getElementById('port').value=d.port;
 document.getElementById('pub').value=d.pub;
 document.getElementById('topic').value=d.topic||'';
 document.getElementById('muser').value=d.muser||'';
 if(d.ssid){var s=document.getElementById('ssid');
  var o=document.createElement('option');o.value=d.ssid;
  o.textContent=d.ssid+' (stored)';o.selected=true;s.appendChild(o)}})}
function save(){
 var b=new URLSearchParams();
 b.set('ssid',document.getElementById('ssid').value);
 b.set('pass',document.getElementById('wpass').value);
 b.set('host',document.getElementById('host').value);
 b.set('port',document.getElementById('port').value);
 b.set('pub',document.getElementById('pub').value);
 b.set('topic',document.getElementById('topic').value);
 b.set('muser',document.getElementById('muser').value);
 b.set('mpass',document.getElementById('mpass').value);
 // signset is what the server tests; the field alone is ignored. A disabled
 // input can still be autofilled, so the contents never express intent.
 if(document.getElementById('signset').checked){
  b.set('signset','1');
  b.set('signpw',document.getElementById('signpw').value)}
 fetch('/save',{method:'POST',headers:{'Content-Type':
  'application/x-www-form-urlencoded'},body:b.toString()})
 .then(r=>r.text()).then(function(t){show(t,1)})
 .catch(function(){show('Saved — the device is restarting.',1)})}
window.onload=function(){load();scan()};
</script>)HTML";

// ── Handlers ────────────────────────────────────────────────────────────────

static Settings  *portalCfg = nullptr;
static const char *portalEui = "";
static void (*portalSave)() = nullptr;

static void handleRoot() {
  portalServer.sendHeader("Cache-Control", "no-store");
  portalServer.send_P(200, "text/html", PORTAL_HTML);
}

// What the page may know. Note what is absent: the WiFi password and the MQTT
// password are never sent, so the browser cannot leak what it was not given.
static void handleStatus() {
  char buf[448];
  snprintf(buf, sizeof(buf),
           "{\"eui\":\"%s\",\"fw\":\"genesis-monitor\",\"ssid\":\"%s\","
           "\"host\":\"%s\",\"port\":%u,\"pub\":%u,\"topic\":\"%s\","
           "\"muser\":\"%s\"}",
           portalEui, portalCfg->ssid, portalCfg->mqttHost,
           (unsigned)portalCfg->mqttPort, (unsigned)portalCfg->publishS,
           portalCfg->pubTopic, portalCfg->mqttUser);
  portalServer.send(200, "application/json", buf);
}

static void handleScan() {
  if (!portalApMode) {                        // setup is AP-only, by design
    portalServer.send(403, "text/plain",
                      "Setup is only available on this device's own Wi-Fi. "
                      "Hold USER for 5 s to open it.");
    return;
  }
  // scanNetworks() returns a NEGATIVE code on failure (-1 running, -2 failed),
  // and the old loop bound `i < n` simply never ran -- so a failed scan was
  // served as an empty array and the page said "nothing found". A scan that
  // could not run and a place with no WiFi are different problems with
  // different fixes, and the page must not present them identically.
  int n = WiFi.scanNetworks();
  if (n < 0) {
    WiFi.scanDelete();
    WiFi.disconnect(false, false);     // drop any half-open STA attempt
    delay(150);
    n = WiFi.scanNetworks();           // one retry, now unopposed
  }
  if (n < 0) {
    portalServer.send(200, "application/json",
                      "{\"error\":\"scan failed - the radio was busy; try again\"}");
    return;
  }
  String out = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i) out += ',';
    out += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
           ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  out += "]";
  WiFi.scanDelete();
  portalServer.send(200, "application/json", out);
}

// ── The sign, on the office network ────────────────────────────────────────
// Read by anyone: that is the whole point of a sign. WRITING requires the
// device's own AP password as a token -- the same secret already printed on its
// SETTINGS screen, so there is no new credential to invent or store. Without
// it, anyone on a campus network could tell a corridor full of students that
// the owner is out.
static const char SIGN_PAGE[] PROGMEM = R"HTML(
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Lab sign</title>
<style>
body{font:16px system-ui;margin:0;background:#101413;color:#e8eceb;
 display:flex;min-height:100vh;align-items:center;justify-content:center}
main{width:min(92vw,420px);padding:22px}
h1{font-size:13px;letter-spacing:.14em;text-transform:uppercase;color:#7d8a86;
 margin:0 0 14px}
#now{font-size:34px;font-weight:600;margin:0 0 4px}
#note{color:#9aa7a3;margin:0 0 20px;min-height:1.2em}
.row{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px}
button{font:inherit;padding:12px;border:1px solid #2c3a36;border-radius:8px;
 background:#18201e;color:#e8eceb;cursor:pointer}
button:hover{border-color:#4d6b62}
input{font:inherit;width:100%;box-sizing:border-box;padding:10px;
 border:1px solid #2c3a36;border-radius:8px;background:#0c100f;color:#e8eceb;
 margin-bottom:8px}
small{color:#6b7873;display:block;margin-top:14px;line-height:1.5}
#msg{margin-top:12px;min-height:1.2em;font-size:14px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:0 0 14px}
.grid div{background:#18201e;border:1px solid #2c3a36;border-radius:8px;
 padding:10px;text-align:center}
.grid b{display:block;font-size:22px;font-variant-numeric:tabular-nums}
.grid span{font-size:11px;color:#7d8a86;text-transform:uppercase;
 letter-spacing:.08em}
#meta{color:#6b7873;font-size:12px;margin:0 0 16px}
.hint{color:#6b7873;font-size:12px;margin:-4px 0 8px}
.bad b{color:#e2725b}
</style>
<main>
<h1>Lab availability</h1>
<p id=now>...</p>
<p id=note></p>
<div class=grid>
 <div><b id=tval>--</b><span>&deg;C</span></div>
 <div><b id=rhval>--</b><span>%RH</span></div>
 <div><b id=alval>--</b><span>alarm</span></div>
</div>
<p id=meta></p>
<div class=row>
 <button onclick="setA('in')">IN</button>
 <button onclick="setA('out')">OUT</button>
 <button onclick="setA('busy')">BUSY</button>
 <button onclick="setA('in class')">IN CLASS</button>
</div>
<input id=n placeholder="note, e.g. back 3pm">
<input id=pw type=password placeholder="sign password">
<div class=hint id=th></div>
<button style="width:100%" onclick="scroll_()">Scroll a notice on the matrix</button>
<div id=msg></div>
<small>The sign password is set over USB with <code>#SIGNPASS</code> and is
never shown on the device's screen. Network and broker settings are not
available here &mdash; hold USER for 5 s on the device to open setup.</small>
</main>
<script>
var T=localStorage.getItem('tok')||'';
if(T){var f0=document.getElementById('pw');
 f0.value=T;f0.placeholder='saved on this device'}
function load(){fetch('/sign.json').then(r=>r.json()).then(function(d){
 document.getElementById('now').textContent=d.avail;
 document.getElementById('note').textContent=d.note||'';
 var q=function(i){return document.getElementById(i)};
 q('tval').textContent=(d.t==null?'--':d.t.toFixed(1));
 q('rhval').textContent=(d.rh==null?'--':d.rh.toFixed(0));
 // "none" under a tile labelled "state" read as the availability being
 // unset. It is the alarm status, and the absence of an alarm is better said
 // as OK than as the absence of a word.
 q('alval').textContent=(!d.alarm||d.alarm=='none')?'OK':d.alarm;
 q('alval').parentNode.className=(d.alarm&&d.alarm!='none')?'bad':'';
 q('meta').textContent='threshold '+d.rhmax+'%RH  \u00b7  broker '+
  (d.mqtt?'connected':'offline')+'  \u00b7  up '+
  Math.floor(d.up/3600)+'h '+Math.floor(d.up%3600/60)+'m'+
  (d.muted?'  \u00b7  buzzer muted':'');
 if(!document.getElementById('n').value)document.getElementById('n').value=d.note||''})}
function post(b){
 var f=document.getElementById('pw'),t=f.value.trim();
 if(!t){var m0=document.getElementById('msg');
  m0.textContent='Enter the sign password first.';
  m0.style.color='#e2725b';f.focus();return Promise.resolve()}
 b.set('tok',t);
 return fetch('/sign',{method:'POST',headers:{'Content-Type':
  'application/x-www-form-urlencoded'},body:b.toString()})
 .then(function(r){return r.text().then(function(x){
   var m=document.getElementById('msg');
   m.textContent=x;m.style.color=r.ok?'#7fd1b9':'#e2725b';
   if(r.ok){localStorage.setItem('tok',t);f.placeholder='saved on this device'}
   else if(r.status==401){
    // A remembered password that the device rejects is worse than none: the
    // field is type=password, so a stale value looks exactly like an empty
    // one and every click silently resends it. Forget it and say so.
    localStorage.removeItem('tok');f.value='';
    f.placeholder='enter the sign password';
    m.textContent=x+' The saved one was cleared \u2014 enter it again.';
    f.focus()}
   load()})})}
function setA(a){var b=new URLSearchParams();b.set('avail',a);
 b.set('note',document.getElementById('n').value);post(b)}
function scroll_(){var b=new URLSearchParams();
 b.set('msg',document.getElementById('n').value);post(b)}
document.getElementById('th').textContent =
 T ? 'A password is remembered in this browser.'
   : 'Set one on the device with #SIGNPASS,gen \u2014 it is shown on the panel.';
load();setInterval(load,5000);
</script>
)HTML";

static void handleSignPage() {
  portalServer.send_P(200, "text/html", SIGN_PAGE);
}

// Hooks the sketch supplies, so portal.h stays ignorant of signage state.
static void (*signApply)(const char *avail, const char *note) = nullptr;
static void (*signMessage)(const char *text) = nullptr;
static const char *(*signRead)(const char **note) = nullptr;
static bool (*signAuth)(const char *token) = nullptr;
static void (*signSetPass)(const char *pw) = nullptr;
// Filled by the sketch: everything the page shows that is not signage.
static void (*signStats)(char *out, size_t n) = nullptr;

static void handleSignJson() {
  const char *note = "";
  const char *av = signRead ? signRead(&note) : "?";
  char stats[224] = "";
  if (signStats) signStats(stats, sizeof(stats));
  char buf[448];
  snprintf(buf, sizeof(buf),
           "{\"avail\":\"%s\",\"note\":\"%s\"%s%s}",
           av, note, stats[0] ? "," : "", stats);
  portalServer.send(200, "application/json", buf);
}

static void handleSignSet() {
  // Constant-time-ish is overkill here, but an empty stored password must never
  // authorise anything -- that would make a device whose AP password had not
  // been generated yet wide open.
  // Trimmed defensively. NOT the cause of the nine-character token that cost
  // several rounds of "wrong password" on a password that was right -- that was
  // a duplicate element id in the page: the readings grid reused id="t" from
  // the password input, so getElementById returned the temperature element,
  // f.value was undefined, and the page posted the literal string
  // "undefined", which is nine characters long. The length matched a plausible
  // wrong theory exactly, which is why measuring the length was not enough and
  // the browser's own error was what actually identified it.
  //
  // The trim stays because a phone keyboard really does append spaces, and the
  // generated alphabet contains no whitespace so it cannot collide.
  String tok = portalServer.arg("tok");
  tok.trim();
  if (!signAuth || !signAuth(tok.c_str())) {
    // Say WHY, in lengths. Three guesses at this failure were wrong because
    // there was nothing to look at: a refusal that reports only "wrong" cannot
    // distinguish a mistyped password from a token that never arrived, a hook
    // that was never registered, or a body the server did not parse.
    Serial.printf("#SIGN,auth refused: token=%u chars, args=%d, hook=%s, "
                  "method=%d\n",
                  (unsigned)tok.length(), portalServer.args(),
                  signAuth ? "set" : "MISSING",
                  (int)portalServer.method());
    for (int i = 0; i < portalServer.args(); i++)
      Serial.printf("#SIGN,  arg[%d] name=%s len=%u\n", i,
                    portalServer.argName(i).c_str(),
                    (unsigned)portalServer.arg(i).length());
    portalServer.send(401, "text/plain", "Wrong sign password.");
    return;
  }
  String msg = portalServer.arg("msg");
  if (msg.length()) {
    if (signMessage) signMessage(msg.c_str());
    portalServer.send(200, "text/plain", "Scrolling.");
    return;
  }
  String av = portalServer.arg("avail");
  if (!av.length()) { portalServer.send(400, "text/plain", "Nothing to set."); return; }
  if (signApply) signApply(av.c_str(), portalServer.arg("note").c_str());
  portalServer.send(200, "text/plain", "Updated.");
}

static void handleSave() {
  if (!portalApMode) {                        // setup is AP-only, by design
    portalServer.send(403, "text/plain",
                      "Setup is only available on this device's own Wi-Fi. "
                      "Hold USER for 5 s to open it.");
    return;
  }
  String ssid = portalServer.arg("ssid");
  String pass = portalServer.arg("pass");

  if (ssid.length()) strncpy(portalCfg->ssid, ssid.c_str(), sizeof(portalCfg->ssid) - 1);

  // Blank means KEEP, not clear. The page is never given the stored password,
  // so its field always arrives empty -- writing it through would erase the
  // password every time somebody edited the port. That fault is documented in
  // handbook chapter 8; this is the fix, not an oversight.
  //
  // But "keep" is meaningless when there is nothing stored, which is the state
  // after an NVS erase or on a new device. Saving then stored an SSID with no
  // password and reported success, leaving a device that could never associate
  // and said nothing about why. Refuse instead.
  if (!pass.length() && !portalCfg->pass[0] && ssid.length()) {
    portalServer.send(400, "text/plain",
                      "This device has no stored Wi-Fi password, so leaving "
                      "the field blank would save a network it cannot join. "
                      "Enter the password.");
    return;
  }
  if (pass.length()) strncpy(portalCfg->pass, pass.c_str(), sizeof(portalCfg->pass) - 1);

  if (portalServer.arg("pub").length())
    portalCfg->publishS = clampTo((uint16_t)portalServer.arg("pub").toInt(),
                                  Limits::PUB_MIN, Limits::PUB_MAX);

  // These three were rendered as editable fields and then silently discarded:
  // the page showed a compile-time constant and handleSave never read them
  // back. A control that cannot do what it appears to do is worse than no
  // control -- someone repoints the broker, sees "Saved", and believes it.
  String host = portalServer.arg("host"); host.trim();
  if (host.length())
    strncpy(portalCfg->mqttHost, host.c_str(), sizeof(portalCfg->mqttHost) - 1);

  if (portalServer.arg("port").length()) {
    long pt = portalServer.arg("port").toInt();
    if (pt > 0 && pt < 65536) portalCfg->mqttPort = (uint16_t)pt;
  }

  String topic = portalServer.arg("topic"); topic.trim();
  if (topic.length())
    strncpy(portalCfg->pubTopic, topic.c_str(), sizeof(portalCfg->pubTopic) - 1);

  // The user IS sent back to the page, so an empty box means "clear it" and
  // anonymous becomes reachable again. The password is NEVER sent back, so its
  // box always arrives empty and blank has to mean KEEP -- the same asymmetry
  // as the WiFi password, and for the same reason.
  if (portalServer.hasArg("muser")) {
    String mu = portalServer.arg("muser"); mu.trim();
    strncpy(portalCfg->mqttUser, mu.c_str(), sizeof(portalCfg->mqttUser) - 1);
    portalCfg->mqttUser[sizeof(portalCfg->mqttUser) - 1] = '\0';
  }
  String mp = portalServer.arg("mpass");
  if (mp.length())
    strncpy(portalCfg->mqttPass, mp.c_str(), sizeof(portalCfg->mqttPass) - 1);

  // Requires an explicit signset field, checked HERE rather than in the page.
  //
  // The first attempt put the guard in the browser: a checkbox that enabled
  // the input. That is not enforcement. The server still accepted signpw
  // whenever it arrived, so a cached copy of the older page -- or any POST at
  // all -- set a password anyway, and the device acquired one nobody had
  // chosen for the second time. A client-side guard protects nothing.
  if (portalServer.arg("signset") == "1") {
    String sp = portalServer.arg("signpw"); sp.trim();
    if (sp.length() >= 6 && signSetPass) signSetPass(sp.c_str());
  }

  if (portalSave) portalSave();
  portalServer.send(200, "text/plain",
                    "Saved. Restarting and joining the network.");
  delay(400);
  ESP.restart();
}

// Raise the access point and serve the page. Called when there is no stored
// network, or when joining one fails.
// Registered once, whichever exposure comes up first. In AP mode "/" is the
// setup form and unknown paths fall back to it, which is what makes a captive
// portal work; on the LAN "/" is the sign and there is no captive behaviour to
// want.
static void portalRegisterRoutes() {
  static bool done = false;
  if (done) return;
  done = true;
  portalServer.on("/", []() { portalApMode ? handleRoot() : handleSignPage(); });
  portalServer.on("/sign", HTTP_GET, handleSignPage);
  portalServer.on("/sign.json", handleSignJson);
  portalServer.on("/sign", HTTP_POST, handleSignSet);
  portalServer.on("/status", handleStatus);
  portalServer.on("/scan", handleScan);
  portalServer.on("/save", HTTP_POST, handleSave);
  portalServer.onNotFound([]() {
    if (portalApMode) handleRoot();          // captive-portal behaviour
    else portalServer.send(404, "text/plain", "Not here. Try /");
  });
}

// The sign, on whatever network the device joined. Config handlers refuse in
// this mode; see the note by portalApMode.
static void portalBeginStation(Settings &cfg, const char *eui,
                               const char *mdnsName) {
  portalCfg = &cfg;
  portalEui = eui;
  portalRegisterRoutes();
  portalApMode = false;
  if (!portalStarted) { portalServer.begin(); portalStarted = true; }
  portalUp = true;

  // mDNS so nobody has to be told a DHCP address. It is advisory: plenty of
  // networks block multicast, and the IP is printed here for exactly that case.
  if (MDNS.begin(mdnsName)) MDNS.addService("http", "tcp", 80);
  Serial.println("=====================================================");
  Serial.printf("  lab sign:  http://%s.local\n", mdnsName);
  Serial.printf("             http://%s\n", WiFi.localIP().toString().c_str());
  Serial.println("  setup is NOT served here -- hold USER 5 s for the AP");
  Serial.println("=====================================================");
}

static void portalHooks(void (*apply)(const char *, const char *),
                        void (*message)(const char *),
                        const char *(*read)(const char **),
                        bool (*auth)(const char *),
                        void (*setPass)(const char *),
                        void (*stats)(char *, size_t)) {
  signApply = apply; signMessage = message; signRead = read;
  signAuth = auth;   signSetPass = setPass; signStats = stats;
}

static void portalBegin(Settings &cfg, const char *eui, void (*saveFn)()) {
  portalCfg = &cfg; portalEui = eui; portalSave = saveFn;

  WiFi.mode(WIFI_AP_STA);            // STA too, so the scan can see networks
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof(apSsid), "GENESIS-%02X%02X", mac[4], mac[5]);
  const char *ssid = apSsid;

  // Per device, from the shared helper -- not the "12345678" that eleven
  // sketches in this estate used to share.
  strncpy(apPass, cfg.apPass, sizeof(apPass) - 1);
  apPasswordEnsure(apPass, sizeof(apPass));
  strncpy(cfg.apPass, apPass, sizeof(cfg.apPass) - 1);
  if (saveFn) saveFn();

  WiFi.softAP(ssid, apPass);
  portalRegisterRoutes();
  portalApMode = true;
  if (!portalStarted) { portalServer.begin(); portalStarted = true; }
  portalUp = true;

  Serial.println("=====================================================");
  Serial.printf("  setup portal:  SSID %s\n", ssid);
  // The password is NOT printed. It is shown on the device's own screen, large,
  // and serial output ends up in logs, terminal scrollback and transcripts --
  // one of which is exactly how this device's password got disclosed. Length
  // only, which is enough to confirm one exists.
  Serial.printf("  password:      %u characters, shown on the SETTINGS screen\n",
                (unsigned)strlen(apPass));
  Serial.println("  open:          http://192.168.4.1");
  Serial.println("=====================================================");
}

static const char *portalApSsid() { return apSsid; }
static void portalService() { if (portalUp) portalServer.handleClient(); }
// "The SETUP AP is open" -- NOT "a web server is running".
//
// The distinction was free until the sign server started serving in station
// mode, which sets portalUp for the life of the device. Callers use this to
// decide whether to suppress WiFi reconnection, whether to hold off deep
// sleep, and whether to blink the setup ring; with it permanently true, the
// reconnect path in networkService() was dead and MQTT could never come back
// after a drop. The visible symptom was a cyan ring that would not stop.
static bool portalRunning() { return portalUp && portalApMode; }

// A web server is listening, on either interface.
static bool portalServing() { return portalUp; }
static const char *portalApPassword() { return apPass; }
