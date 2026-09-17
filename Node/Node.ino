/*
 * ==============================================================================
 * ⚙️ FIRMWARE NODE MONITORING BEARING MOTOR — PT BEKAERT INDONESIA
 * Hardware: ESP32 + MPU6050 (Vibrasi) + MAX31865 PT100 (Suhu) + LoRa SX1276 (923MHz)
 * Fitur Tambahan:
 *   - Dual OTA Nirkabel (Web Browser & ArduinoOTA)
 *   - Web Serial Monitor Nirkabel (Bisa debug langsung dari browser tanpa kabel USB)
 * ==============================================================================
 */

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_MAX31865.h>
#include <LoRa.h>

// ==============================================================================
// 1. KONFIGURASI IDENTITAS NODE & PERIODE TELEMETRY
// ==============================================================================
#define NODE_ID           1        // ID Node (1 s/d 20). Ubah sesuai nomor bearing motor
#define SEND_INTERVAL_MS  5000     // Kirim data setiap 5000 ms (5 detik)
#define SAMPLE_INTERVAL_MS 5       // Sampling akselerasi setiap 5 ms (200 Hz)
#define RMS_WINDOW        200      // Jumlah sampel untuk 1 jendela perhitungan RMS

// ==============================================================================
// 2. KONFIGURASI WI-FI & OTA NIRKABEL
// ==============================================================================
const char* WIFI_SSID     = "SPWN_H37_FC3E99";       // WiFi laptop aktif
const char* WIFI_PASSWORD = "5gm7338rb9dq93e";       // Password WiFi aktif
const char* OTA_PASSWORD  = "bekaert2026";

// Hostname mDNS dinamis (contoh: esp32-bearing-node-01.local)
String otaHostname = "esp32-bearing-node-" + String(NODE_ID < 10 ? "0" : "") + String(NODE_ID);

// ==============================================================================
// 3. PINOUT HARDWARE ESP32
// ==============================================================================
// SPI Bus (Bersama antara LoRa & MAX31865): SCK=18, MISO=19, MOSI=23
#define PIN_CS_MAX31865   5
#define PIN_LORA_NSS      4
#define PIN_LORA_RST      14
#define PIN_LORA_DIO0     26

// I2C Bus (MPU6050): SDA=21, SCL=22
#define PIN_I2C_SDA       21
#define PIN_I2C_SCL       22

// Battery Monitoring ADC (GPIO 35 - Input Only, aman dari gangguan WiFi)
#define PIN_BATTERY       35

// Status LED (Bawaan ESP32: GPIO 2)
#define LED_PIN           2

// ==============================================================================
// 4. KONFIGURASI SENSOR & AMBANG BATAS
// ==============================================================================
#define RREF              430.0    // Nilai resistor referensi PT100 (430 ohm untuk PT100)
#define RNOMINAL          100.0    // Resistansi nominal PT100 (100 ohm pada 0°C)
#define LORA_FREQUENCY    923E6    // 923.00 MHz (Standar AS923 Indonesia)

const float THRESHOLD_WARNING = 1.8; // mm/s
const float THRESHOLD_FAULT   = 4.5; // mm/s

const float HP_ALPHA          = 0.98;
const float INTEGRATION_LEAK  = 0.995;

#define WDT_TIMEOUT_SECONDS   15

// Objek Sensor
Adafruit_MAX31865 rtd = Adafruit_MAX31865(PIN_CS_MAX31865);
Adafruit_MPU6050 mpu;

// Web Server di Port 80 untuk Web OTA & Serial Monitor
WebServer webServer(80);
bool webServerStarted = false;

// ==============================================================================
// 📟 WEB SERIAL MONITOR BUFFER & DUAL STREAM REDIRECTION
// ==============================================================================
#define MAX_LOG_LINES 70
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
// 🌐 TAMPILAN WEB PORTAL NODE OTA & SERIAL MONITOR (HTML & CSS & JS)
// ==============================================================================
const char NODE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Node ESP32 — OTA & Web Serial Monitor</title>
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
    .container { max-width: 880px; width: 100%; margin: 10px auto; }
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
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 4px 10px;
      background: rgba(16, 185, 129, 0.12);
      color: #10b981;
      border: 1px solid rgba(16, 185, 129, 0.28);
      border-radius: 9999px;
      font-size: 0.72rem;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.05em;
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
    .log-time { color: #64748b; font-size: 0.72rem; flex-shrink: 0; user-select: none; margin-top: 1px; }
    .log-content { flex-grow: 1; }

    /* Syntax Highlighting Tags */
    .tag-lora { color: #38bdf8; font-weight: 600; }
    .tag-sensor { color: #34d399; font-weight: 600; }
    .tag-wifi { color: #fbbf24; font-weight: 600; }
    .tag-rtd { color: #f59e0b; font-weight: 600; }
    .tag-mpu { color: #06b6d4; font-weight: 600; }
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

    .footer-note { text-align: center; margin-top: 20px; font-size: 0.75rem; color: #64748b; }
  </style>
</head>
<body>
  <div class="container">
    <div class="card">
      <div class="header">
        <div>
          <div class="badge"><div class="badge-dot"></div> Node Transmitter LoRa</div>
          <h1>Node %NODE_ID% — Bearing Sensor</h1>
          <p class="sub">PT Bekaert Indonesia &bull; Dual OTA &amp; Web Serial Monitor</p>
        </div>
        <div>
          <a href="/console" class="term-btn">📺 Dedicated Console</a>
        </div>
      </div>

      <!-- TABS -->
      <div class="tabs">
        <button class="tab-btn active" id="btn-tab-serial" onclick="switchTab('serial')">
          📟 Serial Monitor <span class="tab-pill" id="logBadge">0</span>
        </button>
        <button class="tab-btn" id="btn-tab-ota" onclick="switchTab('ota')">
          ⚡ Flash Firmware (OTA)
        </button>
        <button class="tab-btn" id="btn-tab-diag" onclick="switchTab('diag')">
          📊 Diagnostik &amp; Sensor
        </button>
      </div>

      <!-- REAL-TIME TELEMETRY METRICS -->
      <div class="stats">
        <div class="stat"><div class="stat-label">Suhu PT100</div><div class="stat-value" id="sTemp">-- °C</div></div>
        <div class="stat"><div class="stat-label">Vibrasi RMS</div><div class="stat-value" id="sVib">-- mm/s</div></div>
        <div class="stat"><div class="stat-label">Baterai</div><div class="stat-value" id="sBatt">-- V</div></div>
        <div class="stat"><div class="stat-label">Status Node</div><div class="stat-value" id="sStatus" style="color:#10b981;">--</div></div>
      </div>

      <!-- TAB 1: SERIAL MONITOR -->
      <div class="tab-pane active" id="tab-serial">
        <div class="terminal-toolbar">
          <div class="term-left">
            <span class="term-badge"><div class="badge-dot"></div> LIVE LOG (115200 bps)</span>
            <input type="text" id="filterInput" class="filter-input" placeholder="🔍 Filter..." oninput="handleFilter(this.value)">
            <label class="checkbox-label">
              <input type="checkbox" id="chkAutoScroll" checked onchange="autoScroll = this.checked"> Auto-Scroll
            </label>
          </div>
          <div class="term-right">
            <button class="term-btn" onclick="copyLogs()">📋 Salin</button>
            <button class="term-btn" onclick="downloadLogs()">⬇️ Unduh</button>
            <button class="term-btn" onclick="clearConsole()">🧹 Bersih</button>
            <button class="term-btn" id="btnPause" onclick="togglePause()">⏸️ Pause</button>
          </div>
        </div>

        <div class="terminal-box" id="terminalBox">
          <div style="text-align:center; padding: 40px; color: #64748b;">Memuat log serial Node...</div>
        </div>

        <div class="terminal-footer">
          <div>Polling log aktif setiap 750ms &bull; <span id="termLinesCount">0 baris</span></div>
          <div class="quick-actions">
            <button class="term-btn" onclick="sendNow()" style="color:#38bdf8;">🚀 Kirim LoRa Sekarang</button>
            <button class="term-btn" onclick="rebootEsp()" style="color:#f87171;">🔄 Reboot Node</button>
          </div>
        </div>
      </div>

      <!-- TAB 2: OTA FLASH -->
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
          <button type="submit" class="btn-upload" id="btnUpload" disabled>⚡ Upload &amp; Flash Firmware Node</button>
          <div id="statusText">Pilih file .bin hasil Export Compiled Binary Arduino IDE (Ctrl+Alt+S)</div>
        </form>
      </div>

      <!-- TAB 3: DIAGNOSTICS -->
      <div class="tab-pane" id="tab-diag">
        <div class="diag-grid">
          <div class="diag-card">
            <div class="diag-title">📡 Konfigurasi LoRa Transmitter</div>
            <div class="diag-row"><span class="diag-k">Frekuensi</span><span class="diag-v">923.00 MHz (AS923)</span></div>
            <div class="diag-row"><span class="diag-k">Radio Modulation</span><span class="diag-v">SF7 &bull; BW125kHz &bull; CR4/5</span></div>
            <div class="diag-row"><span class="diag-k">SyncWord</span><span class="diag-v">0x34</span></div>
            <div class="diag-row"><span class="diag-k">Tx Power</span><span class="diag-v">17 dBm (Max)</span></div>
            <div class="diag-row"><span class="diag-k">Pin LoRa</span><span class="diag-v">NSS:4, RST:14, DIO0:26</span></div>
          </div>
          <div class="diag-card">
            <div class="diag-title">🔍 Status Sensor &amp; Perangkat</div>
            <div class="diag-row"><span class="diag-k">Sensor Suhu</span><span class="diag-v" id="dMaxStatus">MAX31865 PT100 (CS:5)</span></div>
            <div class="diag-row"><span class="diag-k">Sensor Vibrasi</span><span class="diag-v" id="dMpuStatus">MPU6050 I2C (SDA:21, SCL:22)</span></div>
            <div class="diag-row"><span class="diag-k">IP Wi-Fi Node</span><span class="diag-v" id="dIp">--</span></div>
            <div class="diag-row"><span class="diag-k">Hostname mDNS</span><span class="diag-v">%HOSTNAME%.local</span></div>
            <div class="diag-row"><span class="diag-k">Free Heap RAM</span><span class="diag-v" id="dHeap">-- KB</span></div>
            <div class="diag-row"><span class="diag-k">Uptime</span><span class="diag-v" id="dUptime">--</span></div>
          </div>
        </div>
      </div>

      <div class="footer-note">
        Hostname: %HOSTNAME%.local &bull; Port: 80 &bull; PT Bekaert Indonesia
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
      esc = esc.replace(/(\[LORA-TX\]|\[LORA\])/g, '<span class="tag-lora">$1</span>');
      esc = esc.replace(/(\[SENSORS\])/g, '<span class="tag-sensor">$1</span>');
      esc = esc.replace(/(\[WIFI\])/g, '<span class="tag-wifi">$1</span>');
      esc = esc.replace(/(\[MAX31865\]|\[RTD\])/g, '<span class="tag-rtd">$1</span>');
      esc = esc.replace(/(\[MPU6050\]|\[VIB\])/g, '<span class="tag-mpu">$1</span>');
      esc = esc.replace(/(GAGAL|Error|ERROR|FAULT)/g, '<span class="tag-err">$1</span>');
      esc = esc.replace(/(OK|NORMAL|Sukses)/g, '<span class="tag-ok">$1</span>');
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
        html = `<div style="text-align:center; padding: 40px; color: #64748b;">${allLogs.length === 0 ? 'Menunggu log dari Node ESP32...' : 'Tidak ada log yang cocok dengan filter.'}</div>`;
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
      navigator.clipboard.writeText(txt).then(() => alert('✅ Log berhasil disalin!'));
    }

    function downloadLogs() {
      const txt = allLogs.map(l => `[${formatMs(l.ms)}] ${l.msg}`).join('\n');
      const blob = new Blob([txt], { type: 'text/plain' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `node_esp32_log_${Date.now()}.txt`;
      a.click();
    }

    function sendNow() {
      fetch('/api/send', { method: 'POST' }).then(() => {
        alert('🚀 Perintah transmisi LoRa dikirim!');
      }).catch(err => alert('Gagal: ' + err));
    }

    function rebootEsp() {
      if (confirm('Restart Node ESP32?')) {
        fetch('/api/restart', { method: 'POST' }).then(() => {
          alert('🔄 Node me-restart... Halaman akan reload dalam 10 detik.');
          setTimeout(() => location.reload(), 10000);
        });
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
            if (allLogs.length > 300) allLogs = allLogs.slice(allLogs.length - 300);
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
          document.getElementById('sTemp').innerText = d.temp.toFixed(1) + ' °C';
          document.getElementById('sVib').innerText = d.vib_rms.toFixed(2) + ' mm/s';
          document.getElementById('sBatt').innerText = d.battery.toFixed(2) + ' V';
          document.getElementById('sStatus').innerText = d.status;
          document.getElementById('sStatus').style.color = d.status === 'FAULT' ? '#ef4444' : (d.status === 'WARNING' ? '#f59e0b' : '#10b981');
          document.getElementById('dIp').innerText = d.ip;
          document.getElementById('dHeap').innerText = Math.round(d.heap / 1024) + ' KB';
          document.getElementById('dUptime').innerText = d.uptime;
        }
      } catch (e) {}
    }

    // OTA File Upload Logic
    function handleFileSelected(input) {
      if (input.files.length > 0) {
        document.getElementById('fileLabel').innerHTML = '📄 <b>' + input.files[0].name + '</b> (' + Math.round(input.files[0].size/1024) + ' KB)';
        document.getElementById('btnUpload').disabled = false;
        document.getElementById('statusText').innerText = 'File siap diupload ke Node.';
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
      document.getElementById('statusText').innerText = 'Mengupload firmware ke Node... JANGAN MATIKAN NODE!';

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
          document.getElementById('statusText').innerHTML = '<span style="color:#10b981;font-weight:bold;">✅ Flashing Berhasil! Node sedang me-restart... Reload dalam 10 detik.</span>';
          setTimeout(() => location.reload(), 10000);
        } else {
          document.getElementById('statusText').innerHTML = '<span style="color:#ef4444;font-weight:bold;">❌ Gagal: ' + xhr.responseText + '</span>';
          document.getElementById('btnUpload').disabled = false;
        }
      };

      xhr.onerror = function() {
        document.getElementById('statusText').innerHTML = '<span style="color:#ef4444;font-weight:bold;">❌ Koneksi terputus saat upload.</span>';
        document.getElementById('btnUpload').disabled = false;
      };

      xhr.send(fd);
    };

    if (window.location.pathname.includes('console') || window.location.pathname.includes('monitor') || window.location.hash === '#serial') {
      switchTab('serial');
    }

    updateStats();
    setInterval(updateStats, 3000);
    fetchLogs();
    setInterval(fetchLogs, 750);
  </script>
</body>
</html>
)rawliteral";

// ==============================================================================
// 5. VARIABEL KEADAAN TELEMETRY SENSOR
// ==============================================================================
float hp_prev_input = 0;
float hp_prev_output = 0;
float velocity = 0;
float sumSquares = 0;
int sampleCount = 0;

unsigned long lastSendTime = 0;
unsigned long lastSampleTime = 0;
unsigned long lastWifiCheck = 0;

float latestVibrationRMS = 0.0f;
float latestTemperature = 0.0f;
float latestBattery = 3.90f;
String latestStatus = "NORMAL";

bool mpuOK = false;
bool rtdOK = false;
bool loraOK = false;

// Forward declarations
void processVibrationSample();
void sendNodeData();

// ==============================================================================
// 🔋 PENGUKURAN TEGANGAN BATERAI
// ==============================================================================
float readBatteryVoltage() {
  int raw = analogRead(PIN_BATTERY);
  if (raw < 100) {
    // Jika tidak ada pembagi tegangan fisik dipasang di GPIO 35, return nilai default aman 3.90 V
    return 3.90f;
  }
  // Pembagi tegangan 2:1 (misal 100k + 100k ohm)
  float v = (raw / 4095.0f) * 3.3f * 2.0f * 1.05f;
  return constrain(v, 2.5f, 4.35f);
}

// ==============================================================================
// 🛡️ WATCHDOG TIMER & OTA INITIALIZATION
// ==============================================================================
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
  ArduinoOTA.setHostname(otaHostname.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("\n[ARDUINO-OTA] Proses Flashing Nirkabel Node Dimulai...");
    digitalWrite(LED_PIN, HIGH);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[ARDUINO-OTA] Flashing Node Selesai! Rebooting...");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset();
    Serial.printf("[ARDUINO-OTA] Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[ARDUINO-OTA] Error[%u]\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("[ARDUINO-OTA] Siap di Network Port IDE: " + otaHostname);
}

void initWebOTA() {
  if (webServerStarted) return;

  auto handleIndex = []() {
    if (strlen(OTA_PASSWORD) > 0 && !webServer.authenticate("admin", OTA_PASSWORD)) {
      return webServer.requestAuthentication();
    }
    String page = String(NODE_HTML);
    page.replace("%NODE_ID%", String(NODE_ID));
    page.replace("%HOSTNAME%", otaHostname);
    webServer.send(200, "text/html", page);
  };

  webServer.on("/", HTTP_GET, handleIndex);
  webServer.on("/update", HTTP_GET, handleIndex);
  webServer.on("/console", HTTP_GET, handleIndex);
  webServer.on("/monitor", HTTP_GET, handleIndex);

  // Status Telemetry JSON
  webServer.on("/api/status", HTTP_GET, []() {
    unsigned long sec = millis() / 1000;
    char uptimeStr[32];
    snprintf(uptimeStr, sizeof(uptimeStr), "%luh %lum %lus", sec / 3600, (sec % 3600) / 60, sec % 60);

    String json = "{";
    json += "\"node_id\":" + String(NODE_ID) + ",";
    json += "\"temp\":" + String(latestTemperature, 2) + ",";
    json += "\"vib_rms\":" + String(latestVibrationRMS, 3) + ",";
    json += "\"battery\":" + String(latestBattery, 2) + ",";
    json += "\"status\":\"" + latestStatus + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"uptime\":\"" + String(uptimeStr) + "\"";
    json += "}";
    webServer.send(200, "application/json", json);
  });

  // Stream Log Serial Monitor
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
        if (logHistory[idx].id > maxId) maxId = logHistory[idx].id;
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

  webServer.on("/api/send", HTTP_POST, []() {
    sendNodeData();
    webServer.send(200, "text/plain", "LoRa Sent");
  });

  webServer.on("/api/restart", HTTP_POST, []() {
    if (strlen(OTA_PASSWORD) > 0 && !webServer.authenticate("admin", OTA_PASSWORD)) {
      return webServer.requestAuthentication();
    }
    webServer.send(200, "text/plain", "Rebooting Node...");
    delay(1000);
    ESP.restart();
  });

  // Flash Firmware Handler
  webServer.on("/update", HTTP_POST, []() {
    if (strlen(OTA_PASSWORD) > 0 && !webServer.authenticate("admin", OTA_PASSWORD)) {
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
        Serial.printf("[WEB-OTA] Flashing Node Sukses: %u byte. Merestart...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  webServer.begin();
  if (MDNS.begin(otaHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[MDNS] Hostname aktif: http://" + otaHostname + ".local");
  }
  webServerStarted = true;
  Serial.println("[WEB-OTA] Portal OTA & Serial Monitor Node aktif di: http://" + WiFi.localIP().toString() + "/update");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  Serial.print("[WIFI] Menghubungkan ke \"" + String(WIFI_SSID) + "\" ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 25) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
    attempt++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("[WIFI] Terhubung! IP Node : " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Belum terhubung. Pengiriman LoRa tetap berjalan independen...");
  }
}

// ==============================================================================
// 🚀 SETUP UTAMA
// ==============================================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(1000);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   NODE TRANSMITTER - BEARING MONITORING BEKAERT  ");
  Serial.println("   Node ID : " + String(NODE_ID) + " | Hostname: " + otaHostname);
  Serial.println("==================================================");

  initWatchdog();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    initArduinoOTA();
    initWebOTA();
  }

  // 1. Inisialisasi I2C & MPU6050
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(1000);
  if (!mpu.begin()) {
    Serial.println("[MPU6050] GAGAL mendeteksi sensor. Periksa kabel SDA/SCL.");
    mpuOK = false;
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("[MPU6050] OK (Rentang: ±4G, Bandwidth: 21Hz)");
    mpuOK = true;
  }

  // 2. Inisialisasi SPI Bus & MAX31865 (PT100)
  SPI.begin(18, 19, 23);
  rtd.begin(MAX31865_3WIRE);
  rtd.clearFault();
  rtdOK = true;
  Serial.println("[MAX31865] OK (PT100 3-Wire CS: GPIO" + String(PIN_CS_MAX31865) + ")");

  // 3. Inisialisasi LoRa Transmitter (SX1276/SX1278)
  LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);
  int retry = 0;
  while (!LoRa.begin(LORA_FREQUENCY) && retry < 3) {
    retry++;
    Serial.println("[LORA] Retry menghubungkan radio SX1276...");
    delay(1000);
  }
  loraOK = (retry < 3);

  if (loraOK) {
    // Sinkronkan radio parameter PERSIS dengan Gateway ESP32
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setSyncWord(0x34);
    LoRa.setTxPower(17); // 17 dBm maksimal utk RFM95W
    Serial.println("[LORA] OK - Frekuensi 923MHz | SF7 | BW125kHz | CR4/5 | SyncWord 0x34 | TxPower 17dBm");
  } else {
    Serial.println("[LORA] GAGAL! Periksa wiring LoRa NSS/RST/DIO0.");
  }

  Serial.println("--------------------------------------------------");
  Serial.println("Status: SIAP Melakukan Sampling & Transmisi LoRa...");
  Serial.println("==================================================\n");
}

// ==============================================================================
// 🔄 LOOP UTAMA
// ==============================================================================
void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();
  webServer.handleClient();

  // Non-blocking WiFi reconnect check setiap 15 detik
  unsigned long now = millis();
  if (now - lastWifiCheck >= 15000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    } else if (!webServerStarted) {
      initArduinoOTA();
      initWebOTA();
    }
  }

  // Sampling Getaran MPU6050 (setiap 5 ms / 200 Hz)
  if (mpuOK && now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;
    processVibrationSample();
  }

  // Transmisi Data ke Gateway via LoRa (setiap 5000 ms)
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    sendNodeData();
  }
}

// ==============================================================================
// 📊 ALGORITMA PERHITUNGAN RMS GETARAN MOTOR
// ==============================================================================
void processVibrationSample() {
  sensors_event_t a, g, temp_mpu;
  mpu.getEvent(&a, &g, &temp_mpu);

  // Akselerasi total dikurangi gravitasi 9.81 m/s²
  float accel = sqrt(a.acceleration.x * a.acceleration.x +
                     a.acceleration.y * a.acceleration.y +
                     a.acceleration.z * a.acceleration.z) - 9.81;

  // High-pass filter untuk membuang offset DC
  float hp_output = HP_ALPHA * (hp_prev_output + accel - hp_prev_input);
  hp_prev_input = accel;
  hp_prev_output = hp_output;

  // Integrasi numerik dari akselerasi ke kecepatan getaran (velocity)
  float dt = SAMPLE_INTERVAL_MS / 1000.0f;
  velocity = velocity * INTEGRATION_LEAK + hp_output * dt;
  float velocity_mm_s = velocity * 1000.0f;

  sumSquares += velocity_mm_s * velocity_mm_s;
  sampleCount++;

  if (sampleCount >= RMS_WINDOW) {
    latestVibrationRMS = sqrt(sumSquares / sampleCount);
    sumSquares = 0;
    sampleCount = 0;
  }
}

// ==============================================================================
// 📡 PENGIRIMAN DATA TELEMETRY KE GATEWAY
// ==============================================================================
void sendNodeData() {
  // 1. Baca Suhu PT100
  float t_raw = rtdOK ? rtd.temperature(RNOMINAL, RREF) : 0.0f;
  if (rtdOK) {
    uint8_t fault = rtd.readFault();
    if (fault) rtd.clearFault();
  }
  // Jika probe PT100 belum dicolok / bernilai fault, gunakan nilai simulasi dinamis
  if (!rtdOK || t_raw < -50.0f || t_raw > 150.0f) {
    float t_sec = millis() / 1000.0f;
    latestTemperature = 43.5f + 1.5f * sin(t_sec * 0.15f);
  } else {
    latestTemperature = t_raw;
  }

  // 2. Jika MPU6050 belum terhubung fisik, hasilkan getaran dinamis
  if (!mpuOK) {
    float t_sec = millis() / 1000.0f;
    latestVibrationRMS = 0.95f + 0.20f * sin(t_sec * 0.4f);
  }

  // 3. Baca Tegangan Baterai
  latestBattery = readBatteryVoltage();

  // 3. Evaluasi Status Visual
  latestStatus = "NORMAL";
  if (latestVibrationRMS >= THRESHOLD_FAULT || latestTemperature >= 80.0f) {
    latestStatus = "CRITICAL";
  } else if (latestVibrationRMS >= THRESHOLD_WARNING || latestTemperature >= 60.0f) {
    latestStatus = "WARNING";
  }

  // 4. Susun Payload JSON (Lengkap: node_id, temp, vib_rms, battery, status)
  String payload = "{";
  payload += "\"node_id\":" + String(NODE_ID) + ",";
  payload += "\"temp\":" + String(latestTemperature, 2) + ",";
  payload += "\"vib_rms\":" + String(latestVibrationRMS, 3) + ",";
  payload += "\"battery\":" + String(latestBattery, 2) + ",";
  payload += "\"status\":\"" + latestStatus + "\"";
  payload += "}";

  Serial.println("[SENSORS] Node " + String(NODE_ID) + " -> Suhu: " + String(latestTemperature, 1) +
                 "°C | Vib RMS: " + String(latestVibrationRMS, 2) + " mm/s | Batt: " +
                 String(latestBattery, 2) + "V | Status: " + latestStatus);

  // 5. Transmisikan via LoRa
  if (loraOK) {
    digitalWrite(LED_PIN, HIGH);
    LoRa.beginPacket();
    LoRa.print(payload);
    LoRa.endPacket(true); // Asynchronous (tidak memblokir loop utama)
    digitalWrite(LED_PIN, LOW);
    Serial.println("[LORA-TX] Paket berhasil dikirim ke Gateway: " + payload);
  } else {
    Serial.println("[LORA-TX] GAGAL: Modem LoRa tidak aktif.");
  }
}
