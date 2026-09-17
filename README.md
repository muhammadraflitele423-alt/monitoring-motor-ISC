# Sistem Monitoring Kondisi Bearing Motor — Mesin ISC (PT Bekaert Indonesia)

Dashboard web untuk memantau kondisi bearing motor secara real-time (suhu & getaran) dari 20 node ESP32+LoRa (star topology). Gateway ESP32 mengumpulkan data dari LoRa lalu mengirimkannya ke laptop lewat **WiFi (HTTP POST)** — tanpa kabel.

> **Catatan lingkup**: sistem ini bersifat *monitoring*. Indikator normal/warning/critical hanyalah pewarnaan berbasis ambang batas untuk membantu operator membaca kondisi lebih cepat — bukan deteksi kerusakan otomatis atau estimasi *remaining useful life*.

## Alur Data

```
20x Node (ESP32 + LoRa SX1278)  --LoRa (wireless)-->  Gateway (ESP32 + WiFi)
                                                              |
                                                    HTTP POST /api/ingest (WiFi)
                                                              v
                                            Laptop: FastAPI + SQLite + Dashboard Web
```

## Struktur Project

```
bearing_monitoring_system/
├── Node/
│   └── Node.ino           # Firmware Node Sensor (ESP32 + MPU6050 + MAX31865 + LoRa + Web OTA/Serial)
├── GatewayESP32/
│   └── GatewayESP32.ino   # Firmware Gateway (ESP32 + LoRa RX + WiFi POST + Web OTA/Serial)
├── backend/
│   ├── main.py            # Aplikasi FastAPI (REST API + endpoint ingest + WebSocket)
│   ├── database.py        # Model & akses SQLite (SQLAlchemy)
│   ├── config.py          # Konfigurasi (host/port, API key, ambang batas, dst.)
│   └── requirements.txt
└── frontend/
    ├── index.html
    └── static/
        ├── style.css
        └── dashboard.js
```

## 1. Persiapan di VS Code

1. Buka folder `bearing_monitoring_system` di VS Code.
2. Install ekstensi **Python** (Microsoft) jika belum ada.
3. Buka terminal VS Code (`` Ctrl+` ``):

   ```bash
   cd backend
   python -m venv venv
   ```

4. Aktifkan virtual environment:
   - Windows PowerShell: `venv\Scripts\Activate.ps1`
   - Windows CMD: `venv\Scripts\activate.bat`
   - Mac/Linux: `source venv/bin/activate`

5. Install dependency:

   ```bash
   pip install -r requirements.txt
   ```

6. `Ctrl+Shift+P` → "Python: Select Interpreter" → pilih `backend/venv/...` supaya VS Code mengenali package yang sudah di-install.

## 2. Cari Alamat IP Laptop di Jaringan WiFi

Gateway ESP32 perlu tahu alamat IP laptop untuk mengirim data. Laptop dan Gateway **harus terhubung ke jaringan WiFi yang sama**.

- Windows: `ipconfig` di cmd → lihat "IPv4 Address" (contoh: `192.168.1.10`)
- Mac/Linux: `ifconfig` atau `ip addr` → lihat alamat di interface WiFi (mis. `en0` / `wlan0`)

## 3. Jalankan Server

Dari folder `backend` (venv aktif):

```bash
uvicorn main:app --reload --host 0.0.0.0 --port 8000
```

Buka `http://localhost:8000` di laptop untuk melihat dashboard, atau `http://<IP-laptop>:8000` dari perangkat lain di jaringan yang sama.

> Kalau laptop punya firewall aktif, pastikan port 8000 diizinkan untuk koneksi masuk dari jaringan lokal (Windows Defender Firewall → Allow an app).

## 4. Format Data & Endpoint dari Gateway ESP32

Gateway mengirim **HTTP POST** ke:

```
http://<IP-laptop>:8000/api/ingest
Header: Content-Type: application/json
Header: X-API-Key: bekaert-isc-2026   (samakan dengan config.py, atau kosongkan API_KEY untuk nonaktifkan)
```

Body JSON contoh (satu node per request):

```json
{"node_id": 1, "temp": 45.2, "vib_rms": 1.02, "battery": 3.85, "rssi": -72}
```

Field wajib: `node_id`, `temp`, `vib_rms`, `battery`. Field `rssi` opsional.

### Contoh Firmware Gateway ESP32 (WiFi + HTTP POST)

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* ssid = "NAMA_WIFI";
const char* password = "PASSWORD_WIFI";
const char* serverUrl = "http://192.168.1.10:8000/api/ingest"; // ganti sesuai IP laptop
const char* apiKey = "bekaert-isc-2026"; // samakan dengan config.py

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi terhubung, IP Gateway: " + WiFi.localIP().toString());

  // Inisialisasi LoRa di sini (LoRa.begin, dst.)
}

void sendReading(int nodeId, float temp, float vibRms, float battery, int rssi) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", apiKey);

  StaticJsonDocument<200> doc;
  doc["node_id"] = nodeId;
  doc["temp"] = temp;
  doc["vib_rms"] = vibRms;
  doc["battery"] = battery;
  doc["rssi"] = rssi;

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("Server merespons: %d\n", httpCode);
  } else {
    Serial.printf("Gagal kirim: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

void loop() {
  // Setelah menerima & mem-parsing paket LoRa dari salah satu node:
  // sendReading(nodeId, temp, vibRms, battery, LoRa.packetRssi());
}
```

## 5. Pembaruan Firmware Nirkabel (Dual OTA)

Firmware Gateway ESP32 mendukung 2 cara flash nirkabel tanpa perlu colok kabel USB:

### Opsi A: Web Browser OTA (Paling Mudah)
1. Di Arduino IDE: Menu **Sketch** -> **Export Compiled Binary** (`Ctrl+Alt+S`).
2. File `.bin` akan terbentuk di dalam folder sketch (contoh: `GatewayESP32.ino.bin`).
3. Buka browser di laptop/HP: `http://<IP_ESP32>/update` atau `http://esp32-bearing-gateway.local/update`.
4. Masukkan autentikasi jika diminta (User: `admin`, Password: `bekaert2026`).
5. Klik **Browse File**, pilih file `.bin` tadi, lalu klik **⚡ Upload & Flash Firmware**.
6. Tunggu progress bar mencapai 100%. ESP32 akan reboot otomatis dengan firmware baru.

### Opsi B: Arduino IDE (Network Port)
1. Pastikan laptop dan ESP32 di WiFi yang sama.
2. Di Arduino IDE: Buka menu **Tools** -> **Port** -> pilih **esp32-bearing-gateway** di bawah *Network Ports*.
3. Klik tombol **Upload** panah kanan seperti biasa. Masukkan password `bekaert2026` saat diminta.

### Opsi C: Web Serial Monitor (Debugging Nirkabel Langsung dari Browser)
Tanpa perlu mencolokkan kabel USB atau membuka Serial Monitor Arduino IDE di laptop:
1. Buka browser: `http://<IP_ESP32>/console` atau klik tab **📟 Serial Monitor** di `http://<IP_ESP32>/update` (atau klik tombol **📟 Gateway OTA** di dashboard utama).
2. Anda dapat memantau log serial secara langsung (115200 bps):
   - Paket LoRa yang diterima dari node sensor (RSSI, SNR, suhu, vibrasi, baterai).
   - Status pengiriman HTTP POST ke backend FastAPI (`HTTP 200` / error).
   - Status antrean buffer data offline.
   - Status koneksi Wi-Fi & Watchdog Timer.
3. Fitur yang tersedia di Web Console:
   - **Auto-Scroll**: Mengikuti baris log terbaru secara otomatis.
   - **Filter Pencarian**: Menyaring kata kunci tertentu (misalnya `LORA`, `HTTP`, `WIFI`, `ERROR`).
   - **Salin & Unduh Log**: Menyalin seluruh riwayat log ke clipboard atau mengunduh sebagai file `.txt`.
   - **Flush Buffer**: Memaksa Gateway mengirim sisa paket offline yang belum terkirim.
   - **Reboot ESP32**: Me-restart Gateway ESP32 secara nirkabel dengan 1 klik konfirmasi.

### Opsi D: Web Serial Monitor & Dual OTA pada Node ESP32
Node ESP32 di lapangan juga dilengkapi dengan portal Web OTA dan Web Serial Monitor nirkabel:
1. Buka browser: `http://<IP_NODE>/console` atau `http://esp32-bearing-node-01.local/console` (ganti `01` sesuai `NODE_ID`).
2. Fitur yang tersedia di Web Console Node:
   - **Live Sensor Telemetry**: Melihat suhu PT100, vibrasi RMS (mm/s), dan tegangan baterai (V) secara real-time.
   - **Serial Monitor Log**: Memantau sampling MPU6050, pembacaan MAX31865, dan konfirmasi transmisi paket LoRa (`[LORA-TX] Paket berhasil dikirim`).
   - **Wireless Flash OTA**: Mengupload firmware baru tanpa perlu mencabut modul sensor dari mesin motor bearing.
   - **Tombol Uji LoRa**: `🚀 Kirim LoRa Sekarang` untuk memicu transmisi paket uji secara manual.

## 6. Menyesuaikan Ambang Batas / Jumlah Node / API Key

Semua bisa diatur di `backend/config.py`:

- `API_KEY` — kunci validasi sederhana; kosongkan (`""`) untuk menonaktifkan validasi
- `TOTAL_NODES` — jumlah node (default 20)
- `THRESHOLDS` — ambang batas warning/critical untuk suhu, vibrasi, tegangan baterai
- `NODE_NAMES` — penamaan node (bisa diganti sesuai lokasi fisik bearing, mis. "Motor Capstan 1")
- `OFFLINE_THRESHOLD_SECONDS` — batas waktu sebelum node dianggap offline

## 7. Menguji Tanpa Hardware (Simulasi)

Kirim data uji langsung dari terminal/PowerShell tanpa perlu ESP32:

```bash
curl -X POST http://localhost:8000/api/ingest \
  -H "Content-Type: application/json" \
  -H "X-API-Key: bekaert-isc-2026" \
  -d "{\"node_id\":1,\"temp\":45.2,\"vib_rms\":1.02,\"battery\":3.85,\"rssi\":-72}"
```

Kalau berhasil, kartu Node 01 di dashboard akan langsung update lewat WebSocket tanpa refresh halaman.

## Troubleshooting

| Masalah | Kemungkinan Penyebab |
|---|---|
| ESP32 gagal `http.POST` | Cek WiFi ESP32 tersambung, IP laptop benar, laptop & ESP32 satu jaringan yang sama |
| Server balas `401 Unauthorized` | Header `X-API-Key` di firmware tidak sama dengan `API_KEY` di `config.py` |
| Dashboard tidak update otomatis | Cek console browser (F12) untuk error WebSocket, pastikan tidak diblok firewall lokal |
| Data node tidak lengkap (`422`) | Pastikan JSON dari Gateway memiliki 4 field wajib dengan tipe data numerik yang benar |
| `ModuleNotFoundError` saat run | Pastikan virtual environment sudah aktif dan `pip install -r requirements.txt` sudah dijalankan |
