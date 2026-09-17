/*
 * GATEWAY FIRMWARE — ESP32 + LoRa SX1276 (RX) + WiFi + Dual OTA (Web & ArduinoOTA)
 * Terima data dari node via LoRa, teruskan ke FastAPI dashboard lokal via HTTP POST.
 * Mendukung 2 metode pemograman nirkabel (OTA):
 *   1. Web Browser OTA : Buka http://<IP_ESP32>/update di browser, upload file .bin langsung!
 *   2. ArduinoOTA      : Langsung upload dari Arduino IDE via Network Port.
 *
 * Pin map (Gateway ESP32):
 *   SPI       : SCK=18, MISO=19, MOSI=23
 *   LoRa NSS  = GPIO4
 *   LoRa RST  = GPIO14
 *   LoRa DIO0 = GPIO26
 *
 * Library bawaan/eksternal:
 *   - LoRa by Sandeep Mistry
 *   - ArduinoJson (v6/v7)
 *   - ArduinoOTA, WebServer, Update, ESPmDNS (Bawaan ESP32 Core)
 */

#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

// WDT Timeout (15 detik)
#define WDT_TIMEOUT_SECONDS 15

// ==============================================================================
// ⚙️ KONFIGURASI JARINGAN, OTA & SERVER
// ==============================================================================

// 1. WiFi SSID & Password (Samakan dengan jaringan WiFi laptop)
const char* WIFI_SSID     = "SPWN_H37_FC3E99";
const char* WIFI_PASSWORD = "5gm7338rb9dq93e";

// 2. Target Server Ingest API
// Ganti IP_LAPTOP dengan IP yang muncul saat Anda menjalankan `npm start` di laptop.
// Contoh: "http://192.168.100.119:8000/api/ingest"
const char* SERVER_URL    = "http://192.168.100.119:8000/api/ingest";

// 3. API Key (Harus sama dengan API_KEY di backend/config.py)
const char* API_KEY       = "bekaert-isc-2026";

// 4. Konfigurasi OTA (Nirkabel Tanpa Kabel USB)
const char* OTA_HOSTNAME  = "esp32-bearing-gateway"; // http://esp32-bearing-gateway.local
const char* OTA_USERNAME  = "admin";
const char* OTA_PASSWORD  = "bekaert2026";

// ==============================================================================
// 📡 KONFIGURASI LORA SX1276 & INDIKATOR LED (GPIO 2)
// ==============================================================================
#define LED_PIN       2
#define LORA_FREQ     923E6
#define PIN_LORA_NSS   4
#define PIN_LORA_RST   14
#define PIN_LORA_DIO0  26

unsigned long lastLedBlink = 0;
unsigned long lastWifiCheck = 0;
bool ledState = LOW;

// Web Server untuk Web Browser OTA di Port 80
WebServer webServer(80);
bool webServerStarted = false;

// Forward declaration untuk kontrol buffer offline
void flushOfflineBuffer();
extern int bufferCount;

// ==============================================================================
// 📟 WEB SERIAL MONITOR BUFFER & STREAM REDIRECTION
// ==============================================================================
#define MAX_LOG_LINES 80
#define MAX_LOG_LINE_LEN 160

struct LogEntry {
  uint32_t id;
  uint32_t ms;
  char text[MAX_LOG_LINE_LEN];
};

static LogEntry logHistory[MAX_LOG_LINES];
static int logHead = 0;
static int logCount = 0;
static uint32_t nextLogId = 1;
static char currentLogLine[MAX_LOG_LINE_LEN];
static size_t currentLogLineLen = 0;

void appendCharToWebLog(char c) {
  if (c == '\r') return;
  if (c == '\n') {
    if (currentLogLineLen > 0) {
      currentLogLine[currentLogLineLen] = '\0';
      logHistory[logHead].id = nextLogId++;
      logHistory[logHead].ms = millis();
      strncpy(logHistory[logHead].text, currentLogLine, MAX_LOG_LINE_LEN - 1);
      logHistory[logHead].text[MAX_LOG_LINE_LEN - 1] = '\0';
      logHead = (logHead + 1) % MAX_LOG_LINES;
      if (logCount < MAX_LOG_LINES) logCount++;
      currentLogLineLen = 0;
    }
  } else {
    if (currentLogLineLen < MAX_LOG_LINE_LEN - 1) {
      currentLogLine[currentLogLineLen++] = c;
    }
  }
}

String escapeJsonString(const char* input) {
  String out = "";
  while (*input) {
    if (*input == '"') out += "\\\"";
    else if (*input == '\\') out += "\\\\";
    else if (*input == '\t') out += "\\t";
    else if (*input == '\n') out += "\\n";
    else if ((unsigned char)*input >= 32) out += *input;
    input++;
  }
  return out;
}

class DualSerialStream : public Print {
private:
  HardwareSerial* _hwSerial;
public:
  DualSerialStream(HardwareSerial* hw) : _hwSerial(hw) {}

  void begin(unsigned long baud) {
    if (_hwSerial) _hwSerial->begin(baud);
  }
  void flush() {
    if (_hwSerial) _hwSerial->flush();
  }
  size_t write(uint8_t c) override {
    if (_hwSerial) _hwSerial->write(c);
    appendCharToWebLog((char)c);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (_hwSerial) _hwSerial->write(buffer, size);
    for (size_t i = 0; i < size; i++) {
      appendCharToWebLog((char)buffer[i]);
    }
    return size;
  }
};

static DualSerialStream DualSerial(&Serial);
#define Serial DualSerial

// ==============================================================================
// 🌐 TAMPILAN WEB PORTAL OTA & SERIAL MONITOR (HTML & CSS & JS)
// ==============================================================================
const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Gateway ESP32 — OTA & Web Serial Monitor</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
      background: #070b14;
      color: #f1f5f9;
      display: flex;
      justify-content: center;
      align-items: flex-start;
      min-height: 100vh;
      padding: 16px;
    }
    .container {
      max-width: 880px;
      width: 100%;
      margin: 10px auto;
    }
    .card {
      background: #111827;
      border: 1px solid #1f293d;
      border-radius: 14px;
      padding: 24px;
      box-shadow: 0 20px 40px rgba(0,0,0,0.6);
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 12px;
      margin-bottom: 18px;
      padding-bottom: 14px;
      border-bottom: 1px solid #1f293d;
    }
    .header-left { display: flex; flex-direction: column; gap: 4px; }
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 4px 10px;
      background: rgba(6, 182, 212, 0.12);
      color: #06b6d4;
      border: 1px solid rgba(6, 182, 212, 0.28);
      border-radius: 9999px;
      font-size: 0.72rem;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      width: fit-content;
    }
    .badge-dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: #10b981;
      box-shadow: 0 0 8px #10b981;
      animation: pulse 1.8s infinite;
    }
    @keyframes pulse {
      0%, 100% { opacity: 1; transform: scale(1); }
      50% { opacity: 0.4; transform: scale(0.85); }
    }
    h1 { font-size: 1.35rem; font-weight: 700; color: #fff; }
    p.sub { font-size: 0.82rem; color: #94a3b8; }

    /* TABS */
    .tabs {
      display: flex;
      gap: 8px;
      background: #090d16;
      padding: 6px;
      border-radius: 10px;
      border: 1px solid #1e293b;
      margin-bottom: 18px;
    }
    .tab-btn {
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      padding: 9px 12px;
      background: transparent;
      border: none;
      color: #94a3b8;
      font-size: 0.85rem;
      font-weight: 600;
      border-radius: 7px;
      cursor: pointer;
      transition: all 0.2s;
    }
    .tab-btn:hover { color: #f1f5f9; background: rgba(255,255,255,0.04); }
    .tab-btn.active { background: #0284c7; color: #ffffff; box-shadow: 0 2px 8px rgba(2,132,199,0.3); }
    .tab-pill {
      background: rgba(255,255,255,0.2);
      color: #fff;
      padding: 1px 7px;
      border-radius: 9999px;
      font-size: 0.7rem;
      font-weight: 700;
    }

    /* STATS ROW */
    .stats {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
      gap: 10px;
      margin-bottom: 18px;
    }
    .stat {
      background: #090d16;
      border: 1px solid #1e293b;
      padding: 10px 12px;
      border-radius: 8px;
    }
    .stat-label { font-size: 0.68rem; color: #64748b; text-transform: uppercase; font-weight: 700; }
    .stat-value { font-size: 0.95rem; color: #38bdf8; font-weight: 700; margin-top: 3px; }

    /* TAB CONTENT PANELS */
    .tab-pane { display: none; }
    .tab-pane.active { display: block; }

    /* DROPZONE & OTA */
    .dropzone {
      border: 2px dashed #334155;
      border-radius: 10px;
      padding: 26px 16px;
      text-align: center;
      cursor: pointer;
      background: rgba(9, 13, 22, 0.6);
      transition: all 0.2s;
    }
    .dropzone:hover { border-color: #38bdf8; background: rgba(56, 189, 248, 0.04); }
    .dropzone p { font-size: 0.9rem; color: #cbd5e1; margin-bottom: 8px; }
    .btn-browse {
      display: inline-block;
      padding: 7px 16px;
      background: #1e293b;
      color: #f1f5f9;
      border-radius: 6px;
      font-size: 0.82rem;
      font-weight: 600;
      cursor: pointer;
    }
    input[type=file] { display: none; }
    .progress-wrap { margin-top: 18px; display: none; }
    .progress-bar { height: 10px; background: #1e293b; border-radius: 9999px; overflow: hidden; }
    .progress-fill { height: 100%; width: 0%; background: linear-gradient(90deg, #06b6d4, #10b981); transition: width 0.15s ease; }
    .btn-upload {
      width: 100%;
      margin-top: 18px;
      padding: 12px;
      background: #0284c7;
      color: #fff;
      border: none;
      border-radius: 8px;
      font-weight: 700;
      font-size: 0.95rem;
      cursor: pointer;
      transition: background 0.2s;
    }
    .btn-upload:hover { background: #0369a1; }
    .btn-upload:disabled { opacity: 0.4; cursor: not-allowed; }
    #statusText { margin-top: 14px; text-align: center; font-size: 0.85rem; color: #94a3b8; }

    /* SERIAL MONITOR TERMINAL */
    .terminal-toolbar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 10px;
      background: #090d16;
      border: 1px solid #1e293b;
      border-bottom: none;
      border-top-left-radius: 10px;
      border-top-right-radius: 10px;
      padding: 10px 14px;
    }
    .term-left { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
    .term-right { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
    .term-badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      font-family: 'Consolas', 'Courier New', monospace;
      font-size: 0.75rem;
      color: #10b981;
      font-weight: 700;
    }
    .filter-input {
      background: #111827;
      border: 1px solid #334155;
      color: #f1f5f9;
      padding: 5px 10px;
      border-radius: 6px;
      font-size: 0.78rem;
      width: 170px;
    }
    .filter-input:focus { outline: none; border-color: #38bdf8; }
    .term-btn {
      background: #1e293b;
      color: #cbd5e1;
      border: 1px solid #334155;
      padding: 5px 10px;
      border-radius: 6px;
      font-size: 0.75rem;
      font-weight: 600;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      gap: 4px;
      transition: all 0.15s;
    }
    .term-btn:hover { background: #334155; color: #fff; }
    .term-btn.active { background: #0284c7; color: #fff; border-color: #0284c7; }
    .checkbox-label {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      font-size: 0.76rem;
      color: #94a3b8;
      cursor: pointer;
      user-select: none;
    }

    .terminal-box {
      background: #050811;
      border: 1px solid #1e293b;
      border-bottom-left-radius: 10px;
      border-bottom-right-radius: 10px;
      height: 440px;
      overflow-y: auto;
      padding: 12px 14px;
      font-family: 'Consolas', 'JetBrains Mono', 'Courier New', monospace;
      font-size: 0.8rem;
      line-height: 1.5;
      color: #e2e8f0;
      scroll-behavior: smooth;
    }
    .terminal-box::-webkit-scrollbar { width: 8px; }
    .terminal-box::-webkit-scrollbar-track { background: #090d16; }
    .terminal-box::-webkit-scrollbar-thumb { background: #334155; border-radius: 4px; }

    .log-line {
      display: flex;
      align-items: flex-start;
      gap: 10px;
      word-break: break-all;
      margin-bottom: 2px;
      padding: 1px 4px;
      border-radius: 3px;
    }
    .log-line:hover { background: rgba(255,255,255,0.03); }
    .log-time {
      color: #64748b;
      font-size: 0.72rem;
      user-select: none;
      flex-shrink: 0;
      margin-top: 1px;
    }
    .log-content { flex-grow: 1; }

    /* Syntax Highlighting Tags */
    .tag-lora { color: #38bdf8; font-weight: 600; }
    .tag-http { color: #34d399; font-weight: 600; }
    .tag-wifi { color: #fbbf24; font-weight: 600; }
    .tag-buffer { color: #c084fc; font-weight: 600; }
    .tag-parse { color: #60a5fa; font-weight: 600; }
    .tag-ota { color: #fb923c; font-weight: 600; }
    .tag-err { color: #f87171; font-weight: 700; background: rgba(239, 68, 68, 0.14); padding: 0 4px; border-radius: 3px; }
    .tag-ok { color: #4ade80; font-weight: 600; }

    .terminal-footer {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 12px;
      font-size: 0.75rem;
      color: #64748b;
    }
    .quick-actions { display: flex; gap: 8px; }

    /* DIAGNOSTICS TAB */
    .diag-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 12px;
    }
    .diag-card {
      background: #090d16;
      border: 1px solid #1e293b;
      padding: 14px;
      border-radius: 8px;
    }
    .diag-title { font-size: 0.8rem; font-weight: 700; color: #38bdf8; margin-bottom: 10px; text-transform: uppercase; }
    .diag-row { display: flex; justify-content: space-between; margin-bottom: 6px; font-size: 0.82rem; }
    .diag-k { color: #94a3b8; }
    .diag-v { color: #f1f5f9; font-family: 'Consolas', monospace; font-weight: 600; }

    .footer-note {
      text-align: center;
      margin-top: 20px;
      font-size: 0.75rem;
      color: #64748b;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="card">
      <div class="header">
        <div class="header-left">
          <div class="badge"><div class="badge-dot"></div> Gateway LoRa Online</div>
          <h1>Gateway ESP32 — ISC Bekaert</h1>
          <p class="sub">PT Bekaert Indonesia &bull; Bearing Monitoring Wireless OTA &amp; Web Serial Console</p>
        </div>
        <div>
          <a href="/console" class="term-btn" title="Buka Console Langsung">📺 Dedicated Console</a>
        </div>
      </div>

      <!-- NAVIGATION TABS -->
      <div class="tabs">
        <button class="tab-btn active" id="btn-tab-serial" onclick="switchTab('serial')">
          📟 Serial Monitor <span class="tab-pill" id="logBadge">0</span>
        </button>
        <button class="tab-btn" id="btn-tab-ota" onclick="switchTab('ota')">
          ⚡ Flash Firmware (OTA)
        </button>
        <button class="tab-btn" id="btn-tab-diag" onclick="switchTab('diag')">
          📊 Diagnostik
        </button>
      </div>

      <!-- STATUS SUMMARY BAR -->
      <div class="stats">
        <div class="stat"><div class="stat-label">IP Gateway</div><div class="stat-value" id="sIp">--</div></div>
        <div class="stat"><div class="stat-label">Sinyal WiFi</div><div class="stat-value" id="sRssi">--</div></div>
        <div class="stat"><div class="stat-label">Free RAM</div><div class="stat-value" id="sHeap">--</div></div>
        <div class="stat"><div class="stat-label">Uptime</div><div class="stat-value" id="sUptime">--</div></div>
      </div>

      <!-- TAB 1: WEB SERIAL MONITOR -->
      <div class="tab-pane active" id="tab-serial">
        <div class="terminal-toolbar">
          <div class="term-left">
            <span class="term-badge"><div class="badge-dot"></div> LIVE LOG (115200 bps)</span>
            <input type="text" id="filterInput" class="filter-input" placeholder="🔍 Filter (LORA, HTTP...)" oninput="handleFilter(this.value)">
            <label class="checkbox-label">
              <input type="checkbox" id="chkAutoScroll" checked onchange="autoScroll = this.checked"> Auto-Scroll
            </label>
          </div>
          <div class="term-right">
            <button class="term-btn" onclick="copyLogs()" title="Salin semua log ke clipboard">📋 Salin</button>
            <button class="term-btn" onclick="downloadLogs()" title="Unduh log ke file text">⬇️ Unduh</button>
            <button class="term-btn" onclick="clearConsole()" title="Bersihkan tampilan log">🧹 Bersih</button>
            <button class="term-btn" id="btnPause" onclick="togglePause()">⏸️ Pause</button>
          </div>
        </div>

        <div class="terminal-box" id="terminalBox">
          <div style="text-align:center; padding: 40px; color: #64748b;">Memuat log dari Gateway ESP32...</div>
        </div>

        <div class="terminal-footer">
          <div id="termStatus">Polling log aktif setiap 750ms &bull; <span id="termLinesCount">0 baris</span></div>
          <div class="quick-actions">
            <button class="term-btn" onclick="flushBuffer()" title="Paksa kirim antrean data offline">📦 Flush Buffer</button>
            <button class="term-btn" onclick="rebootEsp()" style="color:#f87171;" title="Restart ESP32 nirkabel">🔄 Reboot ESP32</button>
          </div>
        </div>
      </div>

      <!-- TAB 2: WIRELESS OTA FLASH -->
      <div class="tab-pane" id="tab-ota">
        <form id="uploadForm">
          <div class="dropzone" onclick="document.getElementById('firmwareFile').click()">
            <p id="fileLabel">📁 Klik untuk memilih file firmware binary (<b>.bin</b>)</p>
            <span class="btn-browse">Pilih File .bin</span>
            <input type="file" id="firmwareFile" accept=".bin" onchange="handleFileSelected(this)">
          </div>
          <div class="progress-wrap" id="progWrap">
            <div class="progress-bar"><div class="progress-fill" id="progFill"></div></div>
          </div>
          <button type="submit" class="btn-upload" id="btnUpload" disabled>⚡ Upload &amp; Flash Firmware</button>
          <div id="statusText">Pilih file .bin hasil Export Compiled Binary dari Arduino IDE (Ctrl+Alt+S)</div>
        </form>
      </div>

      <!-- TAB 3: DIAGNOSTICS -->
      <div class="tab-pane" id="tab-diag">
        <div class="diag-grid">
          <div class="diag-card">
            <div class="diag-title">📡 Konfigurasi Jaringan &amp; LoRa</div>
            <div class="diag-row"><span class="diag-k">Hostname</span><span class="diag-v">%HOSTNAME%.local</span></div>
            <div class="diag-row"><span class="diag-k">Frekuensi LoRa</span><span class="diag-v" id="dFreq">923.00 MHz</span></div>
            <div class="diag-row"><span class="diag-k">Pin LoRa SX1276</span><span class="diag-v">NSS:4, RST:14, DIO0:26</span></div>
            <div class="diag-row"><span class="diag-k">SPI Bus</span><span class="diag-v">SCK:18, MISO:19, MOSI:23</span></div>
          </div>
          <div class="diag-card">
            <div class="diag-title">⚙️ Target Server &amp; Failover</div>
            <div class="diag-row"><span class="diag-k">Target Ingest API</span><span class="diag-v" id="dServer" style="font-size:0.75rem;">--</span></div>
            <div class="diag-row"><span class="diag-k">Auth API Key</span><span class="diag-v">bekaert-isc-2026</span></div>
            <div class="diag-row"><span class="diag-k">Offline Buffer</span><span class="diag-v" id="dBuffer">0 / 15 paket</span></div>
            <div class="diag-row"><span class="diag-k">Hardware Watchdog</span><span class="diag-v">15 Detik (Aktif)</span></div>
          </div>
        </div>
      </div>

      <div class="footer-note">
        Hostname: %HOSTNAME%.local &bull; Port: 80 &bull; Dual OTA &amp; Web Serial Monitor &bull; PT Bekaert Indonesia
      </div>
    </div>
  </div>

  <script>
    let lastLogId = 0;
    let isPolling = true;
    let autoScroll = true;
    let allLogs = [];
    let filterKeyword = '';

    function switchTab(tabId) {
      document.querySelectorAll('.tab-pane').forEach(el => el.classList.remove('active'));
      document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
      const targetPane = document.getElementById('tab-' + tabId);
      const targetBtn = document.getElementById('btn-tab-' + tabId);
      if (targetPane) targetPane.classList.add('active');
      if (targetBtn) targetBtn.classList.add('active');
      if (tabId === 'serial') {
        renderLogs();
        const box = document.getElementById('terminalBox');
        if (autoScroll && box) box.scrollTop = box.scrollHeight;
      }
    }

    function formatMs(ms) {
      const sec = Math.floor(ms / 1000);
      const h = String(Math.floor(sec / 3600)).padStart(2, '0');
      const m = String(Math.floor((sec % 3600) / 60)).padStart(2, '0');
      const s = String(sec % 60).padStart(2, '0');
      return `${h}:${m}:${s}`;
    }

    function colorizeLog(text) {
      let esc = text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
      esc = esc.replace(/(\[LORA\])/g, '<span class="tag-lora">$1</span>');
      esc = esc.replace(/(\[HTTP\])/g, '<span class="tag-http">$1</span>');
      esc = esc.replace(/(\[WIFI\])/g, '<span class="tag-wifi">$1</span>');
      esc = esc.replace(/(\[BUFFER\])/g, '<span class="tag-buffer">$1</span>');
      esc = esc.replace(/(\[PARSE\])/g, '<span class="tag-parse">$1</span>');
      esc = esc.replace(/(\[INIT\]|\[WDT\]|\[MDNS\])/g, '<span class="tag-parse">$1</span>');
      esc = esc.replace(/(\[ARDUINO-OTA\]|\[WEB-OTA\])/g, '<span class="tag-ota">$1</span>');
      esc = esc.replace(/(GAGAL|Error|ERROR|TIMEOUT)/g, '<span class="tag-err">$1</span>');
      esc = esc.replace(/(OK|Terhubung!|Sukses)/g, '<span class="tag-ok">$1</span>');
      return esc;
    }

    function renderLogs() {
      const box = document.getElementById('terminalBox');
      const q = filterKeyword.toLowerCase().trim();
      let html = '';
      let visibleCount = 0;

      for (const item of allLogs) {
        if (q && !item.msg.toLowerCase().includes(q)) continue;
        visibleCount++;
        html += `<div class="log-line">
          <span class="log-time">[${formatMs(item.ms)}]</span>
          <span class="log-content">${colorizeLog(item.msg)}</span>
        </div>`;
      }

      if (visibleCount === 0) {
        html = `<div style="text-align:center; padding: 40px; color: #64748b;">${allLogs.length === 0 ? 'Menunggu log dari Gateway ESP32...' : 'Tidak ada log yang cocok dengan filter.'}</div>`;
      }

      box.innerHTML = html;
      if (autoScroll) {
        box.scrollTop = box.scrollHeight;
      }
      document.getElementById('termLinesCount').innerText = `${allLogs.length} baris`;
      document.getElementById('logBadge').innerText = allLogs.length;
    }

    function handleFilter(val) {
      filterKeyword = val;
      renderLogs();
    }

    function togglePause() {
      isPolling = !isPolling;
      const btn = document.getElementById('btnPause');
      if (isPolling) {
        btn.innerText = '⏸️ Pause';
        btn.classList.remove('active');
      } else {
        btn.innerText = '▶️ Lanjut';
        btn.classList.add('active');
      }
    }

    function clearConsole() {
      allLogs = [];
      renderLogs();
      fetch('/api/logs/clear', { method: 'POST' }).catch(()=>{});
    }

    function copyLogs() {
      const txt = allLogs.map(l => `[${formatMs(l.ms)}] ${l.msg}`).join('\n');
      navigator.clipboard.writeText(txt).then(() => {
        alert('✅ Semua log berhasil disalin ke clipboard!');
      }).catch(() => {
        alert('Gagal menyalin log.');
      });
    }

    function downloadLogs() {
      const txt = allLogs.map(l => `[${formatMs(l.ms)}] ${l.msg}`).join('\n');
      const blob = new Blob([txt], { type: 'text/plain' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `gateway_esp32_log_${Date.now()}.txt`;
      a.click();
    }

    function flushBuffer() {
      fetch('/api/flush', { method: 'POST' }).then(() => {
        alert('📦 Perintah Flush Buffer berhasil dikirim ke Gateway ESP32.');
      }).catch(err => alert('Gagal mengirim perintah flush: ' + err));
    }

    function rebootEsp() {
      if (confirm('Apakah Anda yakin ingin me-restart Gateway ESP32?')) {
        fetch('/api/restart', { method: 'POST' }).then(() => {
          alert('🔄 Gateway ESP32 sedang me-restart... Halaman akan reload dalam 10 detik.');
          setTimeout(() => location.reload(), 10000);
        }).catch(err => alert('Gagal mengirim perintah restart: ' + err));
      }
    }

    async function fetchLogs() {
      if (!isPolling) return;
      try {
        const res = await fetch(`/api/logs?since=${lastLogId}`);
        if (res.ok) {
          const data = await res.json();
          if (data.logs && data.logs.length > 0) {
            allLogs = allLogs.concat(data.logs);
            if (allLogs.length > 300) {
              allLogs = allLogs.slice(allLogs.length - 300);
            }
            lastLogId = data.last_id;
            renderLogs();
          }
        }
      } catch (err) {}
    }

    async function updateStats() {
      try {
        const r = await fetch('/api/status');
        if (r.ok) {
          const d = await r.json();
          document.getElementById('sIp').innerText = d.ip;
          document.getElementById('sRssi').innerText = d.rssi + ' dBm';
          document.getElementById('sHeap').innerText = Math.round(d.heap / 1024) + ' KB';
          document.getElementById('sUptime').innerText = d.uptime;
          if (document.getElementById('dFreq')) document.getElementById('dFreq').innerText = d.lora_freq || '923.00 MHz';
          if (document.getElementById('dServer')) document.getElementById('dServer').innerText = d.server_url || '--';
          if (document.getElementById('dBuffer')) document.getElementById('dBuffer').innerText = (d.buffer_count || 0) + ' / 15 paket';
        }
      } catch (e) {}
    }

    // OTA File Upload Logic
    function handleFileSelected(input) {
      if (input.files.length > 0) {
        document.getElementById('fileLabel').innerHTML = '📄 <b>' + input.files[0].name + '</b> (' + Math.round(input.files[0].size/1024) + ' KB)';
        document.getElementById('btnUpload').disabled = false;
        document.getElementById('statusText').innerText = 'File siap diupload. Klik tombol di atas.';
      }
    }

    document.getElementById('uploadForm').onsubmit = function(e) {
      e.preventDefault();
      const file = document.getElementById('firmwareFile').files[0];
      if (!file) return;

      const fd = new FormData();
      fd.append('update', file);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);

      document.getElementById('progWrap').style.display = 'block';
      document.getElementById('btnUpload').disabled = true;
      document.getElementById('statusText').innerText = 'Mengupload & memflashing firmware... JANGAN MATIKAN ESP32!';

      xhr.upload.onprogress = function(evt) {
        if (evt.lengthComputable) {
          const pct = Math.round((evt.loaded / evt.total) * 100);
          document.getElementById('progFill').style.width = pct + '%';
          document.getElementById('statusText').innerText = 'Flashing: ' + pct + '% (' + Math.round(evt.loaded/1024) + '/' + Math.round(evt.total/1024) + ' KB)';
        }
      };

      xhr.onload = function() {
        if (xhr.status === 200) {
          document.getElementById('progFill').style.width = '100%';
          document.getElementById('statusText').innerHTML = '<span style="color:#10b981;font-weight:bold;">✅ Flashing Berhasil! ESP32 sedang restart... Halaman akan reload dalam 10 detik.</span>';
          setTimeout(function() { window.location.reload(); }, 10000);
        } else {
          document.getElementById('statusText').innerHTML = '<span style="color:#ef4444;font-weight:bold;">❌ Gagal: ' + xhr.responseText + '</span>';
          document.getElementById('btnUpload').disabled = false;
        }
      };

      xhr.onerror = function() {
        document.getElementById('statusText').innerHTML = '<span style="color:#ef4444;font-weight:bold;">❌ Koneksi terputus saat proses upload.</span>';
        document.getElementById('btnUpload').disabled = false;
      };

      xhr.send(fd);
    };

    // Auto open Serial tab if URL has /console or /monitor
    if (window.location.pathname.includes('console') || window.location.pathname.includes('monitor') || window.location.hash === '#serial') {
      switchTab('serial');
    }

    updateStats();
    setInterval(updateStats, 4000);
    fetchLogs();
    setInterval(fetchLogs, 750);
  </script>
</body>
</html>
)rawliteral";

void initWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = (1 << 0),
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  esp_task_wdt_add(NULL);
#endif
  Serial.println("[WDT] Hardware Watchdog Timer aktif (" + String(WDT_TIMEOUT_SECONDS) + " detik)");
}

void initArduinoOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("\n[ARDUINO-OTA] Proses Flashing Nirkabel Dimulai: " + type);
    digitalWrite(LED_PIN, HIGH);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[ARDUINO-OTA] Flashing Nirkabel Selesai! Rebooting...");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset();
    Serial.printf("[ARDUINO-OTA] Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[ARDUINO-OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Autentikasi Gagal");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Gagal Inisialisasi");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Gagal Koneksi");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Gagal Menerima Data");
    else if (error == OTA_END_ERROR) Serial.println("Gagal Menyelesaikan");
  });

  ArduinoOTA.begin();
  Serial.println("[ARDUINO-OTA] ArduinoOTA Siap! (Port IDE / Hostname: " + String(OTA_HOSTNAME) + ")");
}

void initWebOTA() {
  if (webServerStarted) return;

  // Handler Halaman Utama & Serial Monitor
  auto handleIndex = []() {
    if (strlen(OTA_PASSWORD) > 0 && !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
      return webServer.requestAuthentication();
    }
    String page = String(OTA_HTML);
    page.replace("%HOSTNAME%", OTA_HOSTNAME);
    webServer.send(200, "text/html", page);
  };

  webServer.on("/", HTTP_GET, handleIndex);
  webServer.on("/update", HTTP_GET, handleIndex);
  webServer.on("/console", HTTP_GET, handleIndex);
  webServer.on("/monitor", HTTP_GET, handleIndex);

  // API Status untuk Real-time Stats di Halaman Web OTA
  webServer.on("/api/status", HTTP_GET, []() {
    unsigned long sec = millis() / 1000;
    char uptimeStr[32];
    snprintf(uptimeStr, sizeof(uptimeStr), "%luh %lum %lus", sec / 3600, (sec % 3600) / 60, sec % 60);

    String json = "{";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"uptime\":\"" + String(uptimeStr) + "\",";
    json += "\"buffer_count\":" + String(bufferCount) + ",";
    json += "\"server_url\":\"" + String(SERVER_URL) + "\",";
    json += "\"lora_freq\":\"" + String((long)(LORA_FREQ / 1E6)) + " MHz\"";
    json += "}";
    webServer.send(200, "application/json", json);
  });

  // API Stream Log untuk Web Serial Monitor
  webServer.on("/api/logs", HTTP_GET, []() {
    uint32_t sinceId = 0;
    if (webServer.hasArg("since")) {
      sinceId = webServer.arg("since").toInt();
    }
    
    int startIdx = (logHead - logCount + MAX_LOG_LINES) % MAX_LOG_LINES;
    String json = "{\"logs\":[";
    bool first = true;
    uint32_t maxId = sinceId;

    for (int i = 0; i < logCount; i++) {
      int idx = (startIdx + i) % MAX_LOG_LINES;
      if (logHistory[idx].id > sinceId) {
        if (!first) json += ",";
        first = false;
        json += "{\"id\":";
        json += logHistory[idx].id;
        json += ",\"ms\":";
        json += logHistory[idx].ms;
        json += ",\"msg\":\"";
        json += escapeJsonString(logHistory[idx].text);
        json += "\"}";
        if (logHistory[idx].id > maxId) {
          maxId = logHistory[idx].id;
        }
      }
    }
    json += "],\"last_id\":";
    json += maxId;
    json += "}";
    webServer.send(200, "application/json", json);
  });

  webServer.on("/api/logs/clear", HTTP_POST, []() {
    logCount = 0;
    logHead = 0;
    currentLogLineLen = 0;
    webServer.send(200, "text/plain", "OK");
  });

  webServer.on("/api/flush", HTTP_POST, []() {
    flushOfflineBuffer();
    webServer.send(200, "text/plain", "Buffer Flushed");
  });

  webServer.on("/api/restart", HTTP_POST, []() {
    if (strlen(OTA_PASSWORD) > 0 && !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
      return webServer.requestAuthentication();
    }
    webServer.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });

  // Handler POST Upload Firmware Binary (.bin)
  webServer.on("/update", HTTP_POST, []() {
    if (strlen(OTA_PASSWORD) > 0 && !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
      return webServer.requestAuthentication();
    }
    webServer.sendHeader("Connection", "close");
    webServer.send(200, "text/plain", (Update.hasError()) ? "GAGAL UPDATE" : "OK");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = webServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("\n[WEB-OTA] Memulai upload firmware: %s\n", upload.filename.c_str());
      esp_task_wdt_reset();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      esp_task_wdt_reset();
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("[WEB-OTA] Flashing Sukses: %u byte ditulis. Merestart ESP32...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  webServer.begin();
  if (!MDNS.begin(OTA_HOSTNAME)) {
    Serial.println("[MDNS] Peringatan: Gagal inisialisasi mDNS");
  } else {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[MDNS] Responder aktif: http://" + String(OTA_HOSTNAME) + ".local");
  }
  webServerStarted = true;
  Serial.println("[WEB-OTA] Web Portal OTA & Serial Monitor aktif di: http://" + WiFi.localIP().toString() + "/update");
}

void blinkLed(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
}

void updateStatusLED() {
  unsigned long now = millis();
  
  // Jika WiFi Terputus -> Kedip Cepat Peringatan (200ms)
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastLedBlink >= 200) {
      lastLedBlink = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    // Jika Normal & Standby -> Heartbeat Kedip Singkat Setiap 2 Detik
    if (now - lastLedBlink >= 2000) {
      lastLedBlink = now;
      digitalWrite(LED_PIN, HIGH);
      delay(50);
      digitalWrite(LED_PIN, LOW);
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  Serial.print("[WIFI] Menghubungkan ke \"" + String(WIFI_SSID) + "\" ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    esp_task_wdt_reset(); // Reset WDT agar tidak trigger panic saat menghubungkan WiFi
    delay(500);
    Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Kedip saat mencoba sambung WiFi
    attempt++;
    if (attempt > 30) { // ~15 detik timeout awal
      Serial.println("\n[WIFI] Belum terhubung. Akan dicoba ulang di background...");
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, LOW);
    Serial.println();
    Serial.println("[WIFI] Terhubung! IP Gateway : " + WiFi.localIP().toString());
    Serial.println("[WIFI] Target Server : " + String(SERVER_URL));
  }
}

void checkWiFiReconnect() {
  unsigned long now = millis();
  if (now - lastWifiCheck >= 10000) { // Cek status WiFi setiap 10 detik secara asinkron
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Koneksi terputus! Mencoba auto-reconnect asinkron...");
      WiFi.reconnect();
    } else {
      if (!webServerStarted) {
        initWebOTA();
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(200);

  Serial.println();
  Serial.println("=========================================");
  Serial.println("   GATEWAY FIRMWARE - Bearing Monitoring");
  Serial.println("=========================================");

  initWatchdog();
  connectWiFi();
  
  if (WiFi.status() == WL_CONNECTED) {
    initArduinoOTA();
    initWebOTA();
  }

  Serial.print("[INIT] LoRa SX1276 (923MHz, NSS=GPIO" + String(PIN_LORA_NSS) + ")... ");
  LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("GAGAL! Cek wiring LoRa & antena.");
    // Indikasi LED Menyala Terus jika hardware LoRa GAGAL
    while (1) {
      digitalWrite(LED_PIN, HIGH);
    }
  }
  Serial.println("OK");
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x34);
  Serial.println("[INIT] LoRa Radio Sinkron: SF7, BW125kHz, CR4/5, SyncWord 0x34");

  Serial.println("-----------------------------------------");
  Serial.println("Status : SIAP, menunggu paket dari node...");
  Serial.println("=========================================\n");
  blinkLed(3, 100); // 3 kali kedipan tanda SIAP
}

void loop() {
  esp_task_wdt_reset(); // Feed Watchdog Timer
  ArduinoOTA.handle();   // Handle Over-The-Air Wireless Upload (Arduino IDE)
  webServer.handleClient(); // Handle Web OTA (Browser)
  updateStatusLED();
  checkWiFiReconnect();  // Non-blocking Auto-Reconnect WiFi

  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    digitalWrite(LED_PIN, HIGH); // LED Menyala saat menerima paket
    String received = "";
    while (LoRa.available()) {
      received += (char)LoRa.read();
    }

    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    Serial.println("----- Paket LoRa Masuk -----");
    Serial.println("[LORA] Ukuran paket : " + String(packetSize) + " byte");
    Serial.println("[LORA] Isi mentah   : " + received);
    Serial.println("[LORA] RSSI         : " + String(rssi) + " dBm");
    Serial.println("[LORA] SNR          : " + String(snr) + " dB");

    forwardToServer(received, rssi);
    digitalWrite(LED_PIN, LOW);
  }
}

// ==============================================================================
// 📦 BUFFER OFFLINE & KONTROL TIMEOUT
// ==============================================================================
#define MAX_OFFLINE_BUFFER 15
String offlineBuffer[MAX_OFFLINE_BUFFER];
int bufferCount = 0;

void pushOfflineBuffer(const String& payload) {
  if (bufferCount < MAX_OFFLINE_BUFFER) {
    offlineBuffer[bufferCount++] = payload;
    Serial.println("[BUFFER] Payload disimpan di memori offline (" + String(bufferCount) + "/" + String(MAX_OFFLINE_BUFFER) + ")");
  } else {
    // Geser buffer tertua (FIFO) jika buffer penuh
    for (int i = 0; i < MAX_OFFLINE_BUFFER - 1; i++) {
      offlineBuffer[i] = offlineBuffer[i + 1];
    }
    offlineBuffer[MAX_OFFLINE_BUFFER - 1] = payload;
    Serial.println("[BUFFER] Buffer penuh! Menimpa data paling lama.");
  }
}

bool sendHttpPost(const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(SERVER_URL);
  http.setTimeout(3000); // 3 detik timeout agar tidak memblokir loop
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_KEY) > 0) {
    http.addHeader("X-API-Key", API_KEY);
  }

  int httpCode = http.POST(payload);
  bool success = (httpCode >= 200 && httpCode < 300);

  if (success) {
    Serial.println("Terkirim (HTTP " + String(httpCode) + ")");
  } else {
    Serial.println("GAGAL (HTTP " + String(httpCode) + " / " + http.errorToString(httpCode) + ")");
  }
  http.end();
  return success;
}

void flushOfflineBuffer() {
  if (bufferCount == 0 || WiFi.status() != WL_CONNECTED) return;
  
  Serial.println("[BUFFER] Mengirim " + String(bufferCount) + " paket yang tertunda saat offline...");
  int sentCount = 0;
  for (int i = 0; i < bufferCount; i++) {
    if (sendHttpPost(offlineBuffer[i])) {
      sentCount++;
      delay(100); // Jedap antar paket
    } else {
      break; // Hentikan jika gagal lagi
    }
  }
  
  // Geser sisa buffer yang belum terkirim
  if (sentCount > 0) {
    for (int i = sentCount; i < bufferCount; i++) {
      offlineBuffer[i - sentCount] = offlineBuffer[i];
    }
    bufferCount -= sentCount;
    Serial.println("[BUFFER] Berhasil mengosongkan " + String(sentCount) + " paket.");
  }
}

void forwardToServer(const String& jsonPayload, int rssi) {
  Serial.print("[PARSE] Mem-parsing JSON dari node... ");
  
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument nodeDoc;
#else
  StaticJsonDocument<256> nodeDoc;
#endif

  DeserializationError err = deserializeJson(nodeDoc, jsonPayload);
  if (err) {
    Serial.println("GAGAL!");
    Serial.println("[PARSE] Error: " + String(err.c_str()));
    Serial.println("[PARSE] Paket dibuang, tidak diteruskan ke server.\n");
    return;
  }
  Serial.println("OK");

  // --- Membaca field dengan fleksibilitas nama kunci ---
  int nodeId = nodeDoc["node_id"] | nodeDoc["id"] | 0;
  if (nodeId < 1 || nodeId > 20) {
    Serial.println("[PARSE] GAGAL: Node ID (" + String(nodeId) + ") di luar rentang valid (1-20). Paket dibuang.\n");
    return;
  }
  float temp = nodeDoc["temp"] | nodeDoc["temp_c"] | 0.0f;
  if (temp < -50.0f || temp > 150.0f) temp = 0.0f;
  float vibRms = nodeDoc["vib_rms"] | nodeDoc["vibration_rms"] | 0.0f;
  if (vibRms < 0.0f || vibRms > 50.0f) vibRms = 0.0f;
  float battery = nodeDoc["battery"] | nodeDoc["battery_v"] | 3.90f;
  if (battery < 0.0f || battery > 10.0f) battery = 3.90f;

  // --- Susun payload sesuai skema backend: node_id, temp, vib_rms, battery, rssi ---
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument outDoc;
#else
  StaticJsonDocument<256> outDoc;
#endif

  outDoc["node_id"] = nodeId;
  outDoc["temp"]    = temp;
  outDoc["vib_rms"] = vibRms;
  outDoc["battery"] = battery;
  outDoc["rssi"]    = rssi;

  String outPayload;
  serializeJson(outDoc, outPayload);
  Serial.println("[HTTP] Payload final : " + outPayload);

  Serial.print("[HTTP] Mengirim POST ke " + String(SERVER_URL) + "... ");
  if (!sendHttpPost(outPayload)) {
    pushOfflineBuffer(outPayload);
  } else {
    // Jika pengiriman sukses, coba kirimkan sisa buffer offline jika ada
    flushOfflineBuffer();
  }
  Serial.println("-----------------------------\n");
}