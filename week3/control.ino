/*
 * TH03 - ESP32: Máy trạng thái DỪNG/CHẠY/LỖI - Máy bơm DC + Web điều khiển
 *
 * Phần cứng (sơ đồ nhà kính farm_house_schematic_rev1.0.1):
 *   MOTOR1_INT1 = GPIO5  → IN1 A4950ELJTR  (PWM LEDC tốc độ bơm)
 *   MOTOR1_INT2 = GPIO17 → IN2 A4950ELJTR  (chiều quay / phanh)
 *   BTN         = GPIO36 → nút nhấn vật lý (pull-up ngoài 10 kΩ)
 *   LED_G       = GPIO26 → LED trạng thái
 *   SDA=GPIO33, SCL=GPIO32 → I2C đọc DHT11 từ Arduino slave 0x08
 *
 * Web: http://<IP>/          → giao diện điều khiển
 *      GET  /api/status      → JSON trạng thái
 *      POST /api/cmd?action=start|stop  → bật/tắt bơm từ web
 *      POST /api/duty?pct=25|50|90      → đặt duty cycle
 *
 * Máy trạng thái:
 *   DỪNG → [nút/web] → CHẠY → [hết MAX_RUN_MS | sensor lỗi] → LỖI → [5s] → DỪNG
 *   Khi LỖI: mọi lệnh CHẠY bị bỏ qua
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>

// ================================================================
//  CẤU HÌNH WiFi
// ================================================================
// Thông tin WiFi nằm trong secrets.h (không commit). Xem secrets.h.example.
#include "secrets.h"

// ================================================================
//  PIN
// ================================================================
#define PIN_MOTOR1_IN1  5
#define PIN_MOTOR1_IN2  17
#define PIN_BTN         36
#define PIN_LED         26
#define PIN_SDA         33
#define PIN_SCL         32

// ================================================================
//  LEDC (PWM)
// ================================================================
#define LEDC_CH       0
#define LEDC_FREQ_HZ  5000
#define LEDC_BITS     8
const uint8_t DUTY_25 = 64;
const uint8_t DUTY_50 = 127;
const uint8_t DUTY_90 = 229;
uint8_t pumpDuty      = DUTY_90;

// ================================================================
//  MÁY TRẠNG THÁI
// ================================================================
enum State : uint8_t { STATE_STOP = 0, STATE_RUN, STATE_ERROR };
State    state        = STATE_STOP;
uint32_t runStartMs   = 0;
uint32_t faultStartMs = 0;

const uint32_t MAX_RUN_MS    = 20000;
const uint32_t FAULT_HOLD_MS = 5000;
const char*    STATE_NAME[]  = {"DUNG", "CHAY", "LOI"};

// ================================================================
//  NGẮT NGOÀI + CHỐNG DỘI
// ================================================================
volatile uint32_t btnRawCount = 0;
uint32_t          btnEventCount = 0;
const uint32_t    DEBOUNCE_MS = 50;

void IRAM_ATTR btnISR() {
  btnRawCount++;
}

// ================================================================
//  TIMER PHẦN CỨNG
// ================================================================
hw_timer_t*   hwTimer     = nullptr;
volatile bool timerFlag   = false;
uint32_t      timerPrevMs = 0;
const uint32_t TIMER_PERIOD_US = 2000000;

void IRAM_ATTR onTimer() {
  timerFlag = true;
}

// ================================================================
//  CẢM BIẾN
// ================================================================
#define I2C_SLAVE_ADDR 0x08

struct SensorData { float temp; float hum; bool valid; };
SensorData lastSd = {NAN, NAN, false};

SensorData readSensor() {
  SensorData sd = {NAN, NAN, false};
  uint8_t n = Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)4);
  if (n < 4) { while (Wire.available()) Wire.read(); return sd; }
  uint8_t raw[4];
  for (int i = 0; i < 4; i++) raw[i] = Wire.read();
  uint16_t rt = ((uint16_t)raw[0] << 8) | raw[1];
  uint16_t rh = ((uint16_t)raw[2] << 8) | raw[3];
  if (rt == 0xFFFF || rh == 0xFFFF) return sd;
  sd.temp  = rt / 10.0f;
  sd.hum   = rh / 10.0f;
  sd.valid = (!isnan(sd.temp) && !isnan(sd.hum) &&
              sd.temp >= 0 && sd.temp <= 50 &&
              sd.hum  >= 20 && sd.hum  <= 90);
  return sd;
}

// ================================================================
//  ĐIỀU KHIỂN BƠM
// ================================================================
void pumpOn(uint8_t duty) {
  digitalWrite(PIN_MOTOR1_IN2, LOW);
  ledcWrite(LEDC_CH, duty);
  Serial.printf("# PUMP ON duty=%d (~%d%%)\n", duty, duty * 100 / 255);
}
void pumpOff() {
  ledcWrite(LEDC_CH, 0);
  digitalWrite(PIN_MOTOR1_IN2, LOW);
  Serial.println("# PUMP OFF");
}

void enterStop() {
  pumpOff(); digitalWrite(PIN_LED, LOW);
  Serial.printf("# STATE -> DUNG  t=%lu ms\n", (unsigned long)millis());
}
void enterRun() {
  runStartMs = millis(); pumpOn(pumpDuty); digitalWrite(PIN_LED, HIGH);
  Serial.printf("# STATE -> CHAY  duty=%d%%  max=%lu s\n",
                pumpDuty * 100 / 255, (unsigned long)(MAX_RUN_MS / 1000));
}
void enterError(const char* reason) {
  pumpOff(); faultStartMs = millis();
  Serial.printf("# STATE -> LOI  reason=%s\n", reason);
}

void doStart() {
  if (state == STATE_STOP) { state = STATE_RUN; enterRun(); }
}
void doStop() {
  if (state == STATE_RUN) { state = STATE_STOP; enterStop(); }
}

void updateState(const SensorData& sd) {
  uint32_t now = millis();
  switch (state) {
    case STATE_STOP: break;
    case STATE_RUN:
      if (now - runStartMs >= MAX_RUN_MS) { state = STATE_ERROR; enterError("TIMEOUT"); return; }
      if (!sd.valid)                       { state = STATE_ERROR; enterError("SENSOR_INVALID"); return; }
      digitalWrite(PIN_LED, (now - runStartMs >= MAX_RUN_MS - 5000) ? ((now/250)%2) : 1);
      break;
    case STATE_ERROR:
      digitalWrite(PIN_LED, (now / 200) % 2);
      if (now - faultStartMs >= FAULT_HOLD_MS) { state = STATE_STOP; enterStop(); }
      break;
  }
}

// ================================================================
//  WEB SERVER
// ================================================================
WebServer server(80);

// Trang HTML nhúng thẳng vào flash
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="vi"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TH03 - Điều khiển máy bơm</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap');
:root{--bg:#0f1117;--card:#1a1d27;--card2:#21253a;--border:#2d3248;--accent:#4f8ef7;--accent2:#6c63ff;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;--tx:#e2e8f0;--mut:#64748b}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:'Inter',system-ui,sans-serif;min-height:100vh;padding:20px}
.wrap{max-width:520px;margin:0 auto}
h1{font-size:22px;font-weight:700;margin-bottom:4px;background:linear-gradient(135deg,var(--accent),var(--accent2));-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.sub{font-size:13px;color:var(--mut);margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:20px;margin-bottom:14px}
/* Trạng thái */
.state-box{display:flex;align-items:center;gap:14px}
.dot{width:14px;height:14px;border-radius:50%;flex-shrink:0;transition:background .3s}
.dot.stop {background:var(--mut)}
.dot.run  {background:var(--ok);box-shadow:0 0 12px var(--ok);animation:pulse 1.2s infinite}
.dot.error{background:var(--bad);box-shadow:0 0 12px var(--bad);animation:blink .4s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
.state-label{font-size:26px;font-weight:700}
.state-sub{font-size:12px;color:var(--mut);margin-top:2px}
/* Countdown */
.prog-wrap{margin-top:14px}
.prog-label{display:flex;justify-content:space-between;font-size:12px;color:var(--mut);margin-bottom:6px}
.prog-bg{background:var(--card2);border-radius:99px;height:8px;overflow:hidden}
.prog-fill{height:100%;border-radius:99px;background:linear-gradient(90deg,var(--accent),var(--accent2));transition:width .5s}
.prog-fill.warn{background:linear-gradient(90deg,var(--warn),#f97316)}
/* Sensor */
.sensor-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.s-card{background:var(--card2);border-radius:12px;padding:14px}
.s-lbl{font-size:11px;color:var(--mut);margin-bottom:4px}
.s-val{font-size:28px;font-weight:700}
.s-unit{font-size:13px;color:var(--mut);font-weight:400}
.s-card.temp .s-val{color:#f97316}
.s-card.hum  .s-val{color:var(--accent)}
/* Nút */
.btn-row{display:flex;gap:10px;margin-top:14px}
button{flex:1;padding:13px;border-radius:12px;border:none;font-size:15px;font-weight:600;cursor:pointer;transition:all .2s;letter-spacing:.3px}
#btn-start{background:linear-gradient(135deg,var(--ok),#16a34a);color:#fff}
#btn-start:hover{filter:brightness(1.1);transform:translateY(-1px)}
#btn-stop {background:var(--card2);color:var(--tx);border:1px solid var(--border)}
#btn-stop:hover{background:var(--bad);color:#fff;border-color:var(--bad)}
button:disabled{opacity:.4;cursor:not-allowed;transform:none!important;filter:none!important}
/* Duty */
.duty-row{display:flex;gap:8px;margin-top:12px}
.d-btn{flex:1;padding:9px;border-radius:10px;border:1px solid var(--border);background:var(--card2);color:var(--mut);font-size:13px;font-weight:500;cursor:pointer;transition:all .2s}
.d-btn:hover{border-color:var(--accent);color:var(--accent)}
.d-btn.active{background:var(--accent);color:#fff;border-color:var(--accent)}
/* Log */
.log-box{background:var(--card2);border-radius:10px;padding:12px;font-family:'Courier New',monospace;font-size:11.5px;color:#94a3b8;max-height:130px;overflow-y:auto;margin-top:10px;line-height:1.6}
.log-box .ok{color:var(--ok)}.log-box .err{color:var(--bad)}.log-box .inf{color:var(--accent)}
/* IP badge */
.badge{display:inline-block;background:var(--card2);border:1px solid var(--border);border-radius:8px;padding:5px 10px;font-size:12px;color:var(--mut);margin-bottom:16px}
.badge b{color:var(--tx)}
/* Status bar */
.sbar{display:flex;justify-content:space-between;font-size:11px;color:var(--mut);margin-top:8px}
</style></head>
<body><div class="wrap">
  <h1>🌿 Nhà Kính - Máy Bơm</h1>
  <p class="sub">TH03 · ESP32 · Máy trạng thái DỪNG / CHẠY / LỖI</p>
  <div class="badge">IP: <b id="ip">--</b></div>

  <!-- Trạng thái -->
  <div class="card">
    <div class="state-box">
      <div class="dot stop" id="dot"></div>
      <div>
        <div class="state-label" id="stlabel">DỪNG</div>
        <div class="state-sub"  id="stsub">Đang chờ lệnh bật</div>
      </div>
    </div>
    <div class="prog-wrap" id="prog-wrap" style="display:none">
      <div class="prog-label">
        <span>Thời gian chạy</span>
        <span id="prog-txt">0 / 20 s</span>
      </div>
      <div class="prog-bg"><div class="prog-fill" id="prog-fill" style="width:0%"></div></div>
    </div>
  </div>

  <!-- Cảm biến -->
  <div class="card">
    <div style="font-size:12px;color:var(--mut);margin-bottom:10px;font-weight:500">🌡 CẢM BIẾN DHT11</div>
    <div class="sensor-grid">
      <div class="s-card temp">
        <div class="s-lbl">Nhiệt độ</div>
        <div class="s-val" id="tv">--<span class="s-unit"> °C</span></div>
      </div>
      <div class="s-card hum">
        <div class="s-lbl">Độ ẩm</div>
        <div class="s-val" id="hv">--<span class="s-unit"> %</span></div>
      </div>
    </div>
    <div class="sbar">
      <span id="valid-tag">Trạng thái cảm biến: --</span>
      <span id="last-upd">--</span>
    </div>
  </div>

  <!-- Điều khiển -->
  <div class="card">
    <div style="font-size:12px;color:var(--mut);margin-bottom:10px;font-weight:500">⚡ ĐIỀU KHIỂN</div>
    <div class="btn-row">
      <button id="btn-start" onclick="cmd('start')">▶ BẬT BƠM</button>
      <button id="btn-stop"  onclick="cmd('stop')" disabled>■ TẮT BƠM</button>
    </div>
    <div style="font-size:12px;color:var(--mut);margin:12px 0 6px">Tốc độ bơm (Duty cycle PWM)</div>
    <div class="duty-row">
      <button class="d-btn" id="d25" onclick="setDuty(25)">25%<br><span style="font-size:10px;opacity:.7">Chậm</span></button>
      <button class="d-btn" id="d50" onclick="setDuty(50)">50%<br><span style="font-size:10px;opacity:.7">Vừa</span></button>
      <button class="d-btn active" id="d90" onclick="setDuty(90)">90%<br><span style="font-size:10px;opacity:.7">Mạnh</span></button>
    </div>
  </div>

  <!-- Log -->
  <div class="card">
    <div style="font-size:12px;color:var(--mut);margin-bottom:8px;font-weight:500">📋 NHẬT KÝ</div>
    <div class="log-box" id="log"></div>
    <div class="sbar">
      <span>Nút vật lý: <b id="btn-raw">0</b> lần thô / <b id="btn-evt">0</b> lần hợp lệ</span>
      <span>Chu kỳ timer: <b id="tcycle">-- ms</b></span>
    </div>
  </div>
</div>

<script>
const $=id=>document.getElementById(id);
let currentDuty=90, currentState='DUNG', maxRunMs=20000;

function addLog(msg, cls=''){
  const box=$('log');
  const line=document.createElement('div');
  line.className=cls;
  line.textContent='['+new Date().toLocaleTimeString('vi')+'] '+msg;
  box.appendChild(line);
  if(box.children.length>40) box.removeChild(box.firstChild);
  box.scrollTop=box.scrollHeight;
}

function updateUI(d){
  // Trạng thái
  const dot=$('dot'), lbl=$('stlabel'), sub=$('stsub');
  const pw=$('prog-wrap'), pf=$('prog-fill'), pt=$('prog-txt');
  dot.className='dot '+d.state.toLowerCase();
  currentState=d.state; maxRunMs=d.maxRunMs||20000;

  if(d.state==='DUNG'){
    lbl.textContent='DỪNG'; lbl.style.color='var(--mut)';
    sub.textContent='Đang chờ lệnh bật';
    pw.style.display='none';
    $('btn-start').disabled=false; $('btn-stop').disabled=true;
  } else if(d.state==='CHAY'){
    lbl.textContent='ĐANG CHẠY'; lbl.style.color='var(--ok)';
    const elapsed=d.runElapsed||0, pct=Math.min(elapsed/maxRunMs*100,100);
    const remain=Math.max(0,Math.round((maxRunMs-elapsed)/1000));
    sub.textContent='Còn '+remain+' giây';
    pw.style.display='block';
    pf.style.width=pct+'%';
    pf.className='prog-fill'+(pct>75?' warn':'');
    pt.textContent=Math.round(elapsed/1000)+' / '+Math.round(maxRunMs/1000)+' s';
    $('btn-start').disabled=true; $('btn-stop').disabled=false;
  } else {
    lbl.textContent='LỖI ⚠'; lbl.style.color='var(--bad)';
    sub.textContent='Lý do: '+(d.reason||'?')+' — tự về DỪNG sau '+(d.faultRemain||0)+' s';
    pw.style.display='none';
    $('btn-start').disabled=true; $('btn-stop').disabled=true;
  }

  // Cảm biến
  $('tv').innerHTML=d.temp!==null?d.temp.toFixed(1)+'<span class="s-unit"> °C</span>':'--<span class="s-unit"> °C</span>';
  $('hv').innerHTML=d.hum!==null?d.hum.toFixed(0)+'<span class="s-unit"> %</span>':'--<span class="s-unit"> %</span>';
  $('valid-tag').textContent='Cảm biến: '+(d.valid?'✅ Hợp lệ':'❌ Lỗi / mất tín hiệu');
  $('last-upd').textContent='Cập nhật: '+new Date().toLocaleTimeString('vi');

  // Duty
  [25,50,90].forEach(p=>{
    const b=$('d'+p);
    b.className='d-btn'+(d.duty===p?' active':'');
  });
  currentDuty=d.duty||90;

  // Stats
  $('btn-raw').textContent=d.btnRaw||0;
  $('btn-evt').textContent=d.btnEvent||0;
  $('tcycle').textContent=(d.timerCycle||0)+' ms';
  $('ip').textContent=d.ip||'--';
}

async function poll(){
  try{
    const r=await fetch('/api/status');
    const d=await r.json();
    updateUI(d);
  }catch(e){ addLog('Mất kết nối...','err'); }
}

async function cmd(action){
  try{
    await fetch('/api/cmd?action='+action,{method:'POST'});
    addLog(action==='start'?'▶ Đã bật bơm':'■ Đã tắt bơm', action==='start'?'ok':'inf');
    poll();
  }catch(e){ addLog('Lỗi gửi lệnh','err'); }
}

async function setDuty(pct){
  try{
    await fetch('/api/duty?pct='+pct,{method:'POST'});
    addLog('⚡ Duty cycle -> '+pct+'%','inf');
    poll();
  }catch(e){ addLog('Lỗi đặt duty','err'); }
}

addLog('Kết nối ESP32...','inf');
poll();
setInterval(poll, 1000);
</script></body></html>
)rawliteral";

// ================================================================
//  API HANDLERS
// ================================================================
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStatus() {
  uint32_t now = millis();
  uint32_t runElapsed  = (state == STATE_RUN)   ? (now - runStartMs)   : 0;
  uint32_t faultElapsed= (state == STATE_ERROR)  ? (now - faultStartMs) : 0;
  uint32_t faultRemain = (FAULT_HOLD_MS > faultElapsed) ? (FAULT_HOLD_MS - faultElapsed) / 1000 : 0;

  // IP hiện tại
  String ip = (WiFi.status() == WL_CONNECTED)
              ? WiFi.localIP().toString()
              : WiFi.softAPIP().toString();

  String json = "{";
  json += "\"state\":\"" + String(STATE_NAME[state]) + "\",";
  json += "\"maxRunMs\":" + String(MAX_RUN_MS) + ",";
  json += "\"runElapsed\":" + String(runElapsed) + ",";
  json += "\"faultRemain\":" + String(faultRemain) + ",";
  json += "\"reason\":\"" + String(state == STATE_ERROR ? "TIMEOUT/SENSOR" : "") + "\",";
  if (!isnan(lastSd.temp)) json += "\"temp\":" + String(lastSd.temp, 1) + ",";
  else                      json += "\"temp\":null,";
  if (!isnan(lastSd.hum))  json += "\"hum\":"  + String(lastSd.hum,  1) + ",";
  else                      json += "\"hum\":null,";
  json += "\"valid\":" + String(lastSd.valid ? "true" : "false") + ",";
  json += "\"duty\":" + String(pumpDuty * 100 / 255 > 80 ? 90 : (pumpDuty > 100 ? 50 : 25)) + ",";
  json += "\"btnRaw\":" + String(btnRawCount) + ",";
  json += "\"btnEvent\":" + String(btnEventCount) + ",";
  json += "\"timerCycle\":" + String(timerPrevMs ? (now - timerPrevMs) : 0) + ",";
  json += "\"ip\":\"" + ip + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleCmd() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    if (action == "start")     doStart();
    else if (action == "stop") doStop();
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleDuty() {
  if (server.hasArg("pct")) {
    int pct = server.arg("pct").toInt();
    if      (pct == 25) pumpDuty = DUTY_25;
    else if (pct == 50) pumpDuty = DUTY_50;
    else                pumpDuty = DUTY_90;
    if (state == STATE_RUN) pumpOn(pumpDuty);
    Serial.printf("# WEB duty -> %d%%\n", pct);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// ================================================================
//  WiFi
// ================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("# Ket noi WiFi '%s'", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("# IP: http://");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("# WiFi that bai -> AP '");
    Serial.print(AP_SSID);
    Serial.print("' http://");
    Serial.println(WiFi.softAPIP());
  }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  // GPIO - trạng thái an toàn khi khởi động
  pinMode(PIN_MOTOR1_IN2, OUTPUT); digitalWrite(PIN_MOTOR1_IN2, LOW);
  pinMode(PIN_LED, OUTPUT);        digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BTN, INPUT);  // GPIO36 không có pull-up nội

  // LEDC
  ledcSetup(LEDC_CH, LEDC_FREQ_HZ, LEDC_BITS);
  ledcAttachPin(PIN_MOTOR1_IN1, LEDC_CH);
  ledcWrite(LEDC_CH, 0);

  // I2C
  Wire.begin(PIN_SDA, PIN_SCL);

  // Ngắt ngoài (A)
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), btnISR, FALLING);

  // Timer phần cứng (B): prescaler=80 -> 1MHz -> 2s
  hwTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(hwTimer, &onTimer, true);
  timerAlarmWrite(hwTimer, TIMER_PERIOD_US, true);
  timerAlarmEnable(hwTimer);

  // WiFi + Web
  connectWiFi();
  server.on("/",           handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/cmd",    HTTP_POST, handleCmd);
  server.on("/api/duty",   HTTP_POST, handleDuty);
  server.onNotFound([]() { server.send(404, "text/plain", ""); });
  server.begin();

  Serial.println("# ======= TH03 BOOT =======");
  Serial.printf("# MAX_RUN=%lu s  FAULT_HOLD=%lu s\n",
                (unsigned long)(MAX_RUN_MS / 1000),
                (unsigned long)(FAULT_HOLD_MS / 1000));
  Serial.println("# Serial: '1'=25%  '2'=50%  '3'=90%  'i'=scanI2C  's'=state");
  Serial.println("time_ms,state,temp_c,hum_pct,valid,btn_raw,btn_event,timer_cycle_ms");
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  server.handleClient();

  uint32_t now = millis();

  // Timer phần cứng
  if (timerFlag) {
    timerFlag = false;
    uint32_t cycleMs = timerPrevMs ? (now - timerPrevMs) : 0;
    timerPrevMs = now;

    lastSd = readSensor();
    updateState(lastSd);

    Serial.printf("%lu,%s,", (unsigned long)now, STATE_NAME[state]);
    if (!isnan(lastSd.temp)) Serial.printf("%.1f,", lastSd.temp); else Serial.print(",");
    if (!isnan(lastSd.hum))  Serial.printf("%.1f,", lastSd.hum);  else Serial.print(",");
    Serial.printf("%d,%lu,%lu,%lu\n",
                  (int)lastSd.valid,
                  (unsigned long)btnRawCount,
                  (unsigned long)btnEventCount,
                  (unsigned long)cycleMs);
  }

  // Nút vật lý: chống dội ngoài ISR.
  // Mỗi cạnh mới (kể cả bounce) làm mốc thời gian được cập nhật.
  // Chỉ tính 1 lần bấm khi tín hiệu đã ổn định DEBOUNCE_MS.
  static uint32_t lastSeenRaw = 0;
  static uint32_t lastRawChangeMs = 0;
  static bool pending = false;
  if (btnRawCount != lastSeenRaw) {
    lastSeenRaw = btnRawCount;
    lastRawChangeMs = now;
    pending = true;
  }
  if (pending && (now - lastRawChangeMs >= DEBOUNCE_MS)) {
    pending = false;
    btnEventCount++;
    Serial.printf("# BTN #%lu raw=%lu state=%s\n",
                  (unsigned long)btnEventCount,
                  (unsigned long)btnRawCount,
                  STATE_NAME[state]);
    if      (state == STATE_STOP) { state = STATE_RUN;  enterRun();  }
    else if (state == STATE_RUN)  { state = STATE_STOP; enterStop(); }
  }

  // Lệnh Serial
  while (Serial.available()) {
    char c = (char)Serial.read();
    if      (c=='1') { pumpDuty=DUTY_25; if(state==STATE_RUN) pumpOn(pumpDuty); Serial.println("# duty 25%"); }
    else if (c=='2') { pumpDuty=DUTY_50; if(state==STATE_RUN) pumpOn(pumpDuty); Serial.println("# duty 50%"); }
    else if (c=='3') { pumpDuty=DUTY_90; if(state==STATE_RUN) pumpOn(pumpDuty); Serial.println("# duty 90%"); }
    else if (c=='i') {
      for (uint8_t a=1;a<127;a++) {
        Wire.beginTransmission(a);
        if (!Wire.endTransmission()) Serial.printf("# I2C 0x%02X\n",a);
      }
    }
    else if (c=='s') {
      Serial.printf("# state=%s duty=%d%% elapsed=%lums\n",
                    STATE_NAME[state], pumpDuty*100/255,
                    (unsigned long)(state==STATE_RUN?now-runStartMs:0));
    }
  }
}
