#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <DNSServer.h>   // Captive portal — auto-opens browser

// ── Access Point Config ──────────────────────────────────────
const char* AP_SSID     = "buleshthakur";
const char* AP_PASSWORD = "12345678";        // min 8 chars

// Fixed IP so it never changes — always 192.168.4.1
IPAddress AP_IP      (192, 168,   4, 1);
IPAddress AP_GATEWAY (192, 168,   4, 1);
IPAddress AP_SUBNET  (255, 255, 255, 0);

// ── Pins ─────────────────────────────────────────────────────
#define DHT_PIN         4
#define DHT_TYPE        DHT22

#define PIR_PIN         13

#define RELAY_PIN       33
#define RELAY_ON        LOW    // Active LOW relay module
#define RELAY_OFF       HIGH

#define MOTOR_PWM_PIN   25
#define MOTOR_IN1_PIN   26
#define MOTOR_IN2_PIN   27

// ── PWM ──────────────────────────────────────────────────────
#define PWM_FREQ        5000
#define PWM_RESOLUTION  8        // 0–255

// ── Motor deadband fix ────────────────────────────────────────
// Motors need a minimum kick to start spinning.
// If your motor hums but doesn't spin, raise this value.
#define MIN_PWM         100

// ── Temperature thresholds ────────────────────────────────────
#define TEMP_OFF        16.0f   // Below this — fan stays OFF no matter what
#define TEMP_LOW        25.0f   // Fan starts here
#define TEMP_FULL       32.0f   // Fan at 100% here

// ── Timing ───────────────────────────────────────────────────
#define SENSOR_MS       2000UL   // Read DHT every 2 seconds
#define MOTION_TIMEOUT  30000UL  // 30 sec no motion → check temp fallback

// ── Sleeping person: temp threshold ──────────────────────────
// If temp stays above TEMP_LOW + this offset after motion timeout,
// we assume someone is still in the room (sleeping / sitting still)
#define TEMP_OCCUPIED_OFFSET  1.5f

// ── Objects ──────────────────────────────────────────────────
DHT       dht(DHT_PIN, DHT_TYPE);
WebServer server(80);
DNSServer dns;              // Captive portal DNS

// ── State ────────────────────────────────────────────────────
float temperature   = 25.0f;
float humidity      = 50.0f;

bool  pirActive     = false;   // raw PIR pin right now
bool  motionLatched = false;   // stays true for MOTION_TIMEOUT after last motion
bool  roomOccupied  = false;   // final decision (includes sleeping logic)
String occupancyReason = "empty";

bool  autoMode      = true;
int   manualSpeed   = 0;       // 0–100 %
int   currentPWM    = 0;       // 0–255
bool  relayOn       = false;

unsigned long lastMotionTime = 0;
unsigned long lastSensorTime = 0;
unsigned long lastPIRTime    = 0;   // debounce

// ─────────────────────────────────────────────────────────────
//  PWM — compatible with ESP32 Arduino Core v2 AND v3
// ─────────────────────────────────────────────────────────────
void pwmSetup() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(MOTOR_PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
#else
  ledcSetup(0, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(MOTOR_PWM_PIN, 0);
#endif
}

void pwmWrite(int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(MOTOR_PWM_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

// ─────────────────────────────────────────────────────────────
//  Relay
// ─────────────────────────────────────────────────────────────
void setRelay(bool enable) {
  if (relayOn == enable) return;
  relayOn = enable;
  digitalWrite(RELAY_PIN, enable ? RELAY_ON : RELAY_OFF);
  Serial.printf("[RELAY] %s\n", enable ? "ON" : "OFF");
}

// ─────────────────────────────────────────────────────────────
//  Motor speed
// ─────────────────────────────────────────────────────────────
void setMotorSpeed(int duty) {
  duty = constrain(duty, 0, 255);
  currentPWM = duty;

  if (duty == 0) {
    digitalWrite(MOTOR_IN1_PIN, LOW);
    digitalWrite(MOTOR_IN2_PIN, LOW);
    pwmWrite(0);
  } else {
    digitalWrite(MOTOR_IN1_PIN, HIGH);
    digitalWrite(MOTOR_IN2_PIN, LOW);
    pwmWrite(duty);
  }
}

// ─────────────────────────────────────────────────────────────
//  Temperature → PWM conversion
//  Linear mapping from TEMP_LOW→0% to TEMP_FULL→100%
//  Skips the motor deadband automatically
// ─────────────────────────────────────────────────────────────
int tempToPWM(float t) {
  if (t < TEMP_OFF)  return 0;    // Hard rule: off below 16°C
  if (t < TEMP_LOW)  return 0;    // Comfortable — no need for fan
  if (t >= TEMP_FULL) return 255; // Max speed

  // Map temp range to PWM range (MIN_PWM to 255)
  // This skips the deadband so motor actually spins
  float ratio = (t - TEMP_LOW) / (TEMP_FULL - TEMP_LOW);
  return (int)(MIN_PWM + ratio * (255.0f - MIN_PWM));
}

// ─────────────────────────────────────────────────────────────
//  DUAL-CONDITION OCCUPANCY CHECK
//
//  Fan turns OFF only when BOTH are true:
//    1. No motion for > 30 seconds
//    2. Temperature returned to unoccupied baseline
//
//  Sleeping person logic:
//    A sleeping person emits ~80-100W body heat continuously.
//    Even without moving, the room stays above the baseline.
//    So condition 2 stays false → fan stays ON correctly.
// ─────────────────────────────────────────────────────────────
void checkOccupancy() {
  unsigned long now = millis();
  bool timedOut = (now - lastMotionTime > MOTION_TIMEOUT);

  // Is temperature elevated above what an empty room would be?
  bool tempElevated = (temperature > (TEMP_LOW + TEMP_OCCUPIED_OFFSET));

  if (pirActive) {
    // Active motion right now — definitely occupied
    roomOccupied    = true;
    occupancyReason = "motion";
  }
  else if (!timedOut) {
    // No current motion but within the 30-second hold window
    roomOccupied    = true;
    occupancyReason = "hold_timer";
  }
  else if (tempElevated) {
    // No motion >30s BUT temp is still elevated
    // → Sleeping / very still person → keep fan ON
    roomOccupied    = true;
    occupancyReason = "temp_elevated";
  }
  else {
    // No motion >30s AND temp at baseline → truly empty
    roomOccupied    = false;
    occupancyReason = "empty";
  }
}

// ─────────────────────────────────────────────────────────────
//  Main fan automation
// ─────────────────────────────────────────────────────────────
void updateFan() {
  // Manual mode — dashboard controls everything
  if (!autoMode) {
    // Still enforce hard temperature minimum
    if (temperature < TEMP_OFF) {
      setRelay(false);
      setMotorSpeed(0);
      return;
    }
    setRelay(manualSpeed > 0);
    setMotorSpeed((manualSpeed * 255) / 100);
    return;
  }

  // Auto mode — use occupancy + temperature logic
  checkOccupancy();

  if (!roomOccupied) {
    setRelay(false);
    setMotorSpeed(0);
    return;
  }

  // Room is occupied — set speed from temperature
  int duty = tempToPWM(temperature);
  if (duty == 0) {
    setRelay(false);
    setMotorSpeed(0);
  } else {
    setRelay(true);
    setMotorSpeed(duty);
  }
}

// ─────────────────────────────────────────────────────────────
//  JSON state — sent to dashboard every 2 seconds via polling
// ─────────────────────────────────────────────────────────────
String buildJSON() {
  int speedPct = (currentPWM * 100) / 255;
  String j = "{";
  j += "\"temp\":"     + String(temperature, 1) + ",";
  j += "\"hum\":"      + String(humidity, 1)    + ",";
  j += "\"motion\":"   + String(pirActive ? 1 : 0) + ",";
  j += "\"occupied\":" + String(roomOccupied ? 1 : 0) + ",";
  j += "\"reason\":\""  + occupancyReason + "\",";
  j += "\"speed\":"    + String(speedPct) + ",";
  j += "\"relay\":"    + String(relayOn ? 1 : 0) + ",";
  j += "\"auto\":"     + String(autoMode ? 1 : 0) + ",";
  j += "\"below16\":"  + String(temperature < TEMP_OFF ? 1 : 0);
  j += "}";
  return j;
}

// ─────────────────────────────────────────────────────────────
//  HTML Dashboard
// ─────────────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1.0"/>
<meta http-equiv="Cache-Control" content="no-cache, no-store"/>
<title>SmartFan Control</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@400;600;700&family=Space+Mono:wght@400;700&display=swap');
:root{
  --bg:#0b0f1a;--surface:#131929;--border:#1e2d4a;
  --accent:#00c6ff;--accent2:#0072ff;
  --hot:#ff6b35;--ok:#39ff14;--danger:#ff3860;
  --warn:#ffd166;--text:#e4eaf5;--muted:#5a6a8a
}
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:'Rajdhani',sans-serif;background:var(--bg);
  color:var(--text);min-height:100vh;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px 48px;
  background-image:
    radial-gradient(ellipse 80% 40% at 50% -10%,rgba(0,114,255,.18) 0%,transparent 70%),
    repeating-linear-gradient(0deg,transparent,transparent 39px,rgba(30,45,74,.35) 40px),
    repeating-linear-gradient(90deg,transparent,transparent 39px,rgba(30,45,74,.35) 40px)
}
header{text-align:center;margin-bottom:24px}
.logo{
  font-size:2.2rem;font-weight:700;letter-spacing:.12em;
  background:linear-gradient(90deg,var(--accent),var(--accent2));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;
  text-transform:uppercase
}
.subtitle{font-family:'Space Mono',monospace;font-size:.68rem;color:var(--muted);letter-spacing:.18em;margin-top:4px}
.status-bar{
  display:flex;align-items:center;gap:8px;
  font-family:'Space Mono',monospace;font-size:.68rem;
  color:var(--muted);margin-bottom:22px
}
.dot{width:8px;height:8px;border-radius:50%;background:var(--ok);box-shadow:0 0 8px var(--ok);animation:pulse 2s infinite}
.dot.off{background:var(--danger);box-shadow:0 0 8px var(--danger);animation:none}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}

/* ── Metric grid ── */
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:14px;width:100%;max-width:900px;margin-bottom:16px}
.card{
  background:var(--surface);border:1px solid var(--border);border-radius:14px;
  padding:22px 18px;position:relative;overflow:hidden;transition:border-color .3s
}
.card:hover{border-color:var(--accent)}
.card::before{content:'';position:absolute;inset:0;background:linear-gradient(135deg,rgba(0,198,255,.04),transparent 60%);pointer-events:none}
.card-label{font-family:'Space Mono',monospace;font-size:.62rem;letter-spacing:.18em;color:var(--muted);text-transform:uppercase;margin-bottom:10px}
.card-value{font-size:2.6rem;font-weight:700;line-height:1}
.card-unit{font-size:.95rem;color:var(--muted);margin-left:3px}
.card-icon{position:absolute;right:18px;top:18px;font-size:1.7rem;opacity:.15}
.temp-val{color:#ff6b35}.hum-val{color:#00c6ff}.fan-val{color:#39ff14}

/* ── Status badges ── */
.badge{
  display:inline-flex;align-items:center;gap:7px;
  padding:5px 12px;border-radius:99px;
  font-family:'Space Mono',monospace;font-size:.68rem;font-weight:700;
  letter-spacing:.08em;transition:all .4s;
  border:1px solid var(--muted);color:var(--muted);background:rgba(90,106,138,.1)
}
.badge.on{border-color:var(--ok);color:var(--ok);background:rgba(57,255,20,.1);box-shadow:0 0 10px rgba(57,255,20,.2)}
.badge.relay-on{border-color:var(--hot);color:var(--hot);background:rgba(255,107,53,.1);box-shadow:0 0 10px rgba(255,107,53,.2)}
.badge.warn{border-color:var(--warn);color:var(--warn);background:rgba(255,209,102,.1)}
.bdot{width:6px;height:6px;border-radius:50%;background:currentColor}
.badge.on .bdot,.badge.relay-on .bdot{animation:pulse 1s infinite}

/* ── Occupancy reason strip ── */
.reason-strip{
  width:100%;max-width:900px;
  background:var(--surface);border:1px solid var(--border);
  border-radius:12px;padding:12px 18px;margin-bottom:16px;
  display:flex;align-items:center;gap:12px;
  font-family:'Space Mono',monospace;font-size:.7rem;
}
.reason-label{color:var(--muted);flex-shrink:0}
.reason-val{color:var(--accent);font-weight:700;letter-spacing:.05em}
.below16-warn{
  width:100%;max-width:900px;
  background:rgba(255,56,96,.07);border:1px solid rgba(255,56,96,.3);
  border-radius:12px;padding:12px 18px;margin-bottom:16px;
  font-family:'Space Mono',monospace;font-size:.68rem;color:var(--danger);
  display:none;
}

/* ── Control card ── */
.control-card{
  background:var(--surface);border:1px solid var(--border);
  border-radius:14px;padding:26px 22px;width:100%;max-width:900px;margin-bottom:16px
}
.ctrl-title{font-family:'Space Mono',monospace;font-size:.62rem;letter-spacing:.18em;color:var(--muted);text-transform:uppercase;margin-bottom:20px}

/* ── Fan visual ── */
.fan-wrap{display:flex;flex-direction:column;align-items:center;gap:8px;margin-bottom:22px}
.fan-svg{transition:opacity .4s}.fan-off{opacity:.18}
.spd-bar-wrap{width:180px;height:5px;background:var(--border);border-radius:3px;overflow:hidden}
.spd-bar{height:100%;background:linear-gradient(90deg,var(--accent2),var(--accent));border-radius:3px;transition:width .7s;width:0%}

/* ── Toggle switch ── */
.toggle-row{display:flex;align-items:center;gap:14px;margin-bottom:22px}
.tlabel{font-size:1rem;font-weight:600;min-width:72px;transition:color .3s}
.sw{position:relative;width:60px;height:30px;cursor:pointer;flex-shrink:0}
.sw input{display:none}
.sw-track{position:absolute;inset:0;background:var(--border);border-radius:15px;transition:background .3s}
.sw input:checked+.sw-track{background:var(--accent2)}
.sw-thumb{position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:transform .3s;box-shadow:0 2px 6px rgba(0,0,0,.4)}
.sw input:checked~.sw-thumb{transform:translateX(30px)}

/* ── Manual controls ── */
.hidden{display:none!important}
.slider-lbl{display:flex;justify-content:space-between;align-items:center;margin-bottom:9px;font-size:.9rem}
.slider-pct{font-family:'Space Mono',monospace;font-size:1rem;font-weight:700;color:var(--accent)}
input[type=range]{-webkit-appearance:none;width:100%;height:5px;border-radius:3px;background:var(--border);outline:none;margin-bottom:18px;cursor:pointer}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:linear-gradient(135deg,var(--accent),var(--accent2));cursor:pointer;box-shadow:0 0 10px rgba(0,198,255,.5)}
input[type=range]:disabled{opacity:.35;cursor:not-allowed}

/* ── Relay manual row ── */
.relay-row{display:flex;align-items:center;gap:12px;padding-top:14px;border-top:1px solid var(--border)}
.relay-row .rlabel{font-family:'Space Mono',monospace;font-size:.66rem;color:var(--muted);letter-spacing:.1em}
.relay-row .rval{font-family:'Space Mono',monospace;font-size:.82rem;font-weight:700}
.sw-sm{width:46px;height:24px}.sw-sm .sw-thumb{width:18px;height:18px}
.sw-sm input:checked~.sw-thumb{transform:translateX(22px)}

/* ── IP helper banner ── */
.ip-banner{
  width:100%;max-width:900px;background:rgba(0,114,255,.08);
  border:1px solid rgba(0,198,255,.2);border-radius:12px;
  padding:12px 18px;margin-bottom:16px;
  font-family:'Space Mono',monospace;font-size:.65rem;color:var(--muted);
  line-height:1.9;
}
.ip-banner strong{color:var(--accent)}
footer{font-family:'Space Mono',monospace;font-size:.58rem;color:var(--muted);text-align:center;margin-top:12px;letter-spacing:.15em}
</style>
</head>
<body>

<header>
  <div class="logo">&#128168; SmartFan</div>
  <div class="subtitle">ESP32 &nbsp;&#183;&nbsp; AP MODE &nbsp;&#183;&nbsp; DHT22 + PIR + L298N</div>
</header>

<!-- Connection status -->
<div class="status-bar">
  <span class="dot off" id="status-dot"></span>
  <span id="status-txt">CONNECTING...</span>
  <span style="margin-left:8px;font-size:.6rem" id="last-update"></span>
</div>

<!-- Below 16°C warning -->
<div class="below16-warn" id="below16-warn">
  &#9888; Temperature below 16°C — fan is locked OFF regardless of settings
</div>

<!-- Occupancy reason strip -->
<div class="reason-strip">
  <span class="reason-label">OCCUPANCY:</span>
  <span class="reason-val" id="reason-val">waiting...</span>
</div>

<!-- Metric cards -->
<div class="grid">
  <div class="card">
    <div class="card-icon">&#127777;</div>
    <div class="card-label">Temperature</div>
    <div class="card-value temp-val"><span id="temp">--</span><span class="card-unit">°C</span></div>
  </div>
  <div class="card">
    <div class="card-icon">&#128167;</div>
    <div class="card-label">Humidity</div>
    <div class="card-value hum-val"><span id="hum">--</span><span class="card-unit">%</span></div>
  </div>
  <div class="card">
    <div class="card-icon">&#128168;</div>
    <div class="card-label">Fan Speed</div>
    <div class="card-value fan-val"><span id="fanspd">--</span><span class="card-unit">%</span></div>
  </div>
  <div class="card" style="display:flex;flex-direction:column;gap:11px;">
    <div>
      <div class="card-label">PIR &nbsp; GPIO 13</div>
      <span class="badge" id="motion-badge"><span class="bdot"></span><span id="motion-txt">NO MOTION</span></span>
    </div>
    <div>
      <div class="card-label">Relay &nbsp; GPIO 33</div>
      <span class="badge" id="relay-badge"><span class="bdot"></span><span id="relay-txt">POWER CUT</span></span>
    </div>
  </div>
</div>

<!-- Control Panel -->
<div class="control-card">
  <div class="ctrl-title">Control Panel</div>

  <!-- Fan visual + speed bar -->
  <div class="fan-wrap">
    <svg class="fan-svg fan-off" id="fan-visual" width="96" height="96" viewBox="0 0 100 100">
      <circle cx="50" cy="50" r="48" fill="none" stroke="#1e2d4a" stroke-width="1.5"/>
      <g id="fan-blades" fill="rgba(0,198,255,.12)" stroke="#00c6ff" stroke-width="2.5" stroke-linecap="round">
        <path d="M50 50 Q30 20 50 10 Q70 20 50 50"/>
        <path d="M50 50 Q80 30 90 50 Q80 70 50 50"/>
        <path d="M50 50 Q70 80 50 90 Q30 80 50 50"/>
        <path d="M50 50 Q20 70 10 50 Q20 30 50 50"/>
      </g>
      <circle cx="50" cy="50" r="5" fill="#00c6ff"/>
    </svg>
    <div class="spd-bar-wrap"><div class="spd-bar" id="spd-bar"></div></div>
  </div>

  <!-- Mode toggle -->
  <div class="toggle-row">
    <span class="tlabel" id="lbl-manual" style="color:var(--muted)">MANUAL</span>
    <label class="sw">
      <input type="checkbox" id="mode-toggle" checked onchange="onModeChange()"/>
      <span class="sw-track"></span><span class="sw-thumb"></span>
    </label>
    <span class="tlabel" id="lbl-auto" style="color:var(--accent)">AUTO</span>
  </div>

  <!-- Manual controls (hidden in auto mode) -->
  <div id="manual-section" class="hidden">
    <div class="slider-lbl">
      <span>Fan Speed</span>
      <span class="slider-pct" id="slider-pct">50%</span>
    </div>
    <input type="range" id="speed-slider" min="0" max="100" value="50"
           oninput="onSliderMove(this.value)" onchange="sendSpeed(this.value)"/>
    <div class="relay-row">
      <span class="rlabel">RELAY POWER &nbsp; GPIO 33</span>
      <label class="sw sw-sm">
        <input type="checkbox" id="relay-toggle" onchange="onRelayChange()"/>
        <span class="sw-track"></span><span class="sw-thumb"></span>
      </label>
      <span class="rval" id="relay-lbl" style="color:var(--muted)">OFF</span>
    </div>
  </div>
</div>

<!-- Connection helper -->
<div class="ip-banner">
  <strong>Connection tip:</strong> If this page stops updating, your phone switched to mobile data.<br>
  On Android → tap "Stay connected" when asked. &nbsp; On iPhone → tap (i) on the network → disable Auto-Join override.<br>
  Always use: <strong>http://192.168.4.1</strong> &nbsp; (http — not https)
</div>

<footer>SMARTFAN v3 &nbsp;&#183;&nbsp; AP: SmartFan_ESP32 &nbsp;&#183;&nbsp; 192.168.4.1</footer>

<script>
// ── Fan blade animation ──────────────────────────────────────
var fanAnim = null;
function updateFanVisual(pct) {
  var vis    = document.getElementById('fan-visual');
  var blades = document.getElementById('fan-blades');
  document.getElementById('spd-bar').style.width = pct + '%';
  if (pct === 0) {
    vis.classList.add('fan-off');
    if (fanAnim) { try { fanAnim.cancel(); } catch(e){} fanAnim = null; }
    return;
  }
  vis.classList.remove('fan-off');
  var dur = Math.round(1000 + (100 - pct) / 100 * 3500);
  try {
    if (fanAnim) fanAnim.cancel();
    fanAnim = blades.animate(
      [{ transform:'rotate(0deg)' }, { transform:'rotate(360deg)' }],
      { duration:dur, iterations:Infinity, easing:'linear' }
    );
  } catch(e) {}
}

// ── Mode toggle ───────────────────────────────────────────────
function onModeChange() {
  var isAuto = document.getElementById('mode-toggle').checked;
  var ms = document.getElementById('manual-section');
  isAuto ? ms.classList.add('hidden') : ms.classList.remove('hidden');
  document.getElementById('lbl-auto').style.color   = isAuto ? 'var(--accent)' : 'var(--muted)';
  document.getElementById('lbl-manual').style.color = isAuto ? 'var(--muted)'  : 'var(--accent)';
  safeFetch('/mode?auto=' + (isAuto ? '1' : '0'));
}

// ── Manual speed slider ───────────────────────────────────────
function onSliderMove(v) { document.getElementById('slider-pct').textContent = v + '%'; }
function sendSpeed(v)    { safeFetch('/speed?val=' + v); }

// ── Manual relay toggle ───────────────────────────────────────
function onRelayChange() {
  var on  = document.getElementById('relay-toggle').checked;
  var lbl = document.getElementById('relay-lbl');
  lbl.textContent = on ? 'ON' : 'OFF';
  lbl.style.color = on ? 'var(--hot)' : 'var(--muted)';
  safeFetch('/relay?val=' + (on ? '1' : '0'));
}

// ── Apply JSON state to UI ────────────────────────────────────
var reasonLabels = {
  'motion':       'MOTION DETECTED',
  'hold_timer':   'HOLD TIMER (30s after motion)',
  'temp_elevated':'SLEEPING / STILL PERSON (temp elevated)',
  'empty':        'ROOM EMPTY — fan off'
};

function applyState(d) {
  if (d.temp  !== undefined) document.getElementById('temp').textContent  = parseFloat(d.temp).toFixed(1);
  if (d.hum   !== undefined) document.getElementById('hum').textContent   = parseFloat(d.hum).toFixed(1);

  if (d.speed !== undefined) {
    var pct = parseInt(d.speed);
    document.getElementById('fanspd').textContent = pct;
    updateFanVisual(pct);
  }

  if (d.motion !== undefined) {
    var on = d.motion == 1;
    var mb = document.getElementById('motion-badge');
    mb.className = 'badge' + (on ? ' on' : '');
    document.getElementById('motion-txt').textContent = on ? 'MOTION DETECTED' : 'NO MOTION';
  }

  if (d.relay !== undefined) {
    var ron = d.relay == 1;
    document.getElementById('relay-badge').className = 'badge' + (ron ? ' relay-on' : '');
    document.getElementById('relay-txt').textContent = ron ? 'POWER PASSING' : 'POWER CUT';
    var rt = document.getElementById('relay-toggle');
    if (rt) { rt.checked = ron; }
    var rl = document.getElementById('relay-lbl');
    if (rl) { rl.textContent = ron?'ON':'OFF'; rl.style.color = ron?'var(--hot)':'var(--muted)'; }
  }

  if (d.reason !== undefined) {
    document.getElementById('reason-val').textContent = reasonLabels[d.reason] || d.reason;
  }

  if (d.below16 !== undefined) {
    document.getElementById('below16-warn').style.display = d.below16 == 1 ? 'block' : 'none';
  }

  if (d.auto !== undefined) {
    var a = d.auto == 1;
    document.getElementById('mode-toggle').checked = a;
    onModeChange();
  }
}

// ── Safe fetch with retry ─────────────────────────────────────
function safeFetch(url) {
  return fetch(url, { signal: AbortSignal.timeout(3000) }).catch(function(){});
}

// ── Polling loop ──────────────────────────────────────────────
// Polls /state every 2 seconds.
// This is more reliable than WebSockets on AP mode because
// every phone browser handles HTTP fetch natively.
var connected = false;
var pollFails  = 0;

function poll() {
  fetch('/state', { signal: AbortSignal.timeout(4000) })
    .then(function(r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    })
    .then(function(d) {
      connected = true;
      pollFails = 0;
      document.getElementById('status-dot').classList.remove('off');
      document.getElementById('status-txt').textContent = 'CONNECTED';
      document.getElementById('last-update').textContent =
        'last: ' + new Date().toLocaleTimeString();
      applyState(d);
    })
    .catch(function(e) {
      pollFails++;
      if (pollFails >= 2) {
        connected = false;
        document.getElementById('status-dot').classList.add('off');
        document.getElementById('status-txt').textContent =
          'RECONNECTING... (turn off mobile data if stuck)';
      }
    });
}

// Start polling — every 2 seconds
setInterval(poll, 2000);
poll(); // immediate first call

// Init UI
onModeChange();
</script>
</body>
</html>
)rawliteral";

// ─────────────────────────────────────────────────────────────
//  HTTP Route Handlers
// ─────────────────────────────────────────────────────────────
void handleRoot() {
  // Prevent caching so phone always gets fresh HTML
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleState() {
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", buildJSON());
}

void handleMode() {
  if (server.hasArg("auto")) {
    autoMode = server.arg("auto").toInt() == 1;
    Serial.printf("[MODE] Switched to %s\n", autoMode ? "AUTO" : "MANUAL");
  }
  updateFan();
  server.send(200, "text/plain", "OK");
}

void handleSpeed() {
  if (server.hasArg("val")) {
    manualSpeed = constrain(server.arg("val").toInt(), 0, 100);
    Serial.printf("[SPEED] Manual: %d%%\n", manualSpeed);
  }
  if (!autoMode) updateFan();
  server.send(200, "text/plain", "OK");
}

void handleRelay() {
  if (server.hasArg("val") && !autoMode) {
    bool on = server.arg("val").toInt() == 1;
    setRelay(on);
    if (!on) setMotorSpeed(0);
    Serial.printf("[RELAY] Manual set: %s\n", on ? "ON" : "OFF");
  }
  server.send(200, "text/plain", "OK");
}

// Captive portal redirect — any unknown URL goes to dashboard
void handleCaptive() {
  server.sendHeader("Location", "http://192.168.4.1", true);
  server.send(302, "text/plain", "");
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Smart Fan Controller ===");

  // ── GPIO init ──────────────────────────────────────────────
  pinMode(PIR_PIN,       INPUT);
  pinMode(RELAY_PIN,     OUTPUT);
  pinMode(MOTOR_IN1_PIN, OUTPUT);
  pinMode(MOTOR_IN2_PIN, OUTPUT);

  // Safe state at boot — relay OFF, motor stopped
  digitalWrite(RELAY_PIN,     RELAY_OFF);
  digitalWrite(MOTOR_IN1_PIN, LOW);
  digitalWrite(MOTOR_IN2_PIN, LOW);

  // ── PWM ────────────────────────────────────────────────────
  pwmSetup();
  pwmWrite(0);

  // ── DHT ────────────────────────────────────────────────────
  dht.begin();
  delay(2000); // DHT22 needs 2s to stabilize

  // Initial sensor read
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;
  Serial.printf("[DHT] Initial: %.1f°C  %.1f%%\n", temperature, humidity);

  // ── WiFi Access Point ──────────────────────────────────────
  // Disconnect from any previous WiFi first
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  delay(200);

  // Set fixed IP BEFORE starting AP — this is the key fix
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  delay(100);

  // Start AP
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 4);
  // Channel 6 is less congested than default channel 1
  // Last arg = max 4 simultaneous connections

  delay(500); // give AP time to initialize

  if (apStarted) {
    Serial.println("[WiFi] Access Point started successfully!");
  } else {
    Serial.println("[WiFi] AP start FAILED — restarting...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("====================================");
  Serial.println("  SSID     : SmartFan_ESP32");
  Serial.println("  Password : 12345678");
  Serial.println("  URL      : http://192.168.4.1");
  Serial.println("====================================");

  // ── DNS (captive portal) ───────────────────────────────────
  // Any domain typed in browser → redirects to 192.168.4.1
  dns.start(53, "*", AP_IP);

  // ── HTTP Routes ────────────────────────────────────────────
  server.on("/",       HTTP_GET, handleRoot);
  server.on("/state",  HTTP_GET, handleState);
  server.on("/mode",   HTTP_GET, handleMode);
  server.on("/speed",  HTTP_GET, handleSpeed);
  server.on("/relay",  HTTP_GET, handleRelay);

  // Captive portal routes (phones check these on connect)
  server.on("/generate_204",          HTTP_GET, handleCaptive); // Android
  server.on("/gen_204",               HTTP_GET, handleCaptive); // Android alt
  server.on("/hotspot-detect.html",   HTTP_GET, handleCaptive); // iOS
  server.on("/library/test/success.html", HTTP_GET, handleCaptive); // macOS
  server.on("/ncsi.txt",              HTTP_GET, handleCaptive); // Windows
  server.on("/connecttest.txt",       HTTP_GET, handleCaptive); // Windows 10+
  server.onNotFound(handleCaptive);  // everything else → redirect

  server.begin();
  Serial.println("[HTTP] Server started on port 80");

  // Init motion timer
  lastMotionTime = millis();
  lastSensorTime = millis();

  Serial.println("[SYSTEM] Ready. Waiting for connections...");
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  // These two MUST be called every single loop — no delay() allowed here
  dns.processNextRequest();
  server.handleClient();

  unsigned long now = millis();

  // ── PIR reading with debounce (check every loop) ──────────
  bool pirRaw = (digitalRead(PIR_PIN) == HIGH);
  if (pirRaw) {
    if (!pirActive) {
      Serial.println("[PIR] Motion detected! Timer reset.");
    }
    pirActive      = true;
    lastMotionTime = now;
  } else {
    // Only clear pirActive flag — motionLatched handles the 30s window
    pirActive = false;
  }

  // ── DHT + fan logic every 2 seconds ──────────────────────
  if (now - lastSensorTime >= SENSOR_MS) {
    lastSensorTime = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      temperature = t;
      humidity    = h;
    } else {
      Serial.println("[DHT] Read failed — using last known values");
    }

    updateFan();

    Serial.printf("[%lus] %.1f°C  %.1f%%  PIR:%d  %s  Fan:%d%%\n",
      now/1000, temperature, humidity,
      pirActive, occupancyReason.c_str(),
      (currentPWM * 100) / 255
    );
  }
}
