"""
Konfigurasi Sistem Monitoring Kondisi Bearing Motor - PT Bekaert Indonesia
Ubah nilai-nilai di bawah ini sesuai kondisi lapangan.
"""
import os
from pathlib import Path
from dotenv import load_dotenv

# Muat variabel dari file .env di root proyek (satu level di atas folder backend)
_env_path = Path(__file__).resolve().parent.parent / ".env"
if _env_path.exists():
    load_dotenv(dotenv_path=_env_path)

# --- Pengaturan MySQL ---
# Gunakan MySQL bila USE_MYSQL=True. Detail koneksi diambil dari variabel lingkungan.
USE_MYSQL = bool(int(os.getenv("USE_MYSQL", "1")))  # 1 = gunakan MySQL, 0 = SQLite
MYSQL_HOST = os.getenv("MYSQL_HOST", "localhost")
MYSQL_PORT = int(os.getenv("MYSQL_PORT", "3307"))
MYSQL_DATABASE = os.getenv("MYSQL_DATABASE", "MotorConditionMonitoring")
MYSQL_USER = os.getenv("MYSQL_USER", "admin")
MYSQL_PASSWORD = os.getenv("MYSQL_PASSWORD", "bekaert2026")
# Opsi TLS (SSL). Jika MySQL server mengharuskan TLS, set MYSQL_TLS=1
MYSQL_TLS = bool(int(os.getenv("MYSQL_TLS", "1")))

# --- Server ---
# Host 0.0.0.0 supaya bisa diakses Gateway ESP32 lewat WiFi jaringan lokal yang sama
HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "8000"))

# Kunci sederhana untuk memvalidasi pengirim data (opsional tapi disarankan).
# Gateway ESP32 harus mengirim header "X-API-Key" dengan nilai yang sama.
# Kosongkan string ini ("") jika tidak ingin memakai validasi kunci.
API_KEY = os.getenv("API_KEY", "bekaert-isc-2026")

# --- Database ---
# Jika menggunakan MySQL, DB_PATH tidak dipakai.
# Untuk fallback SQLite, tetap gunakan path file.
DB_PATH = os.path.join(os.path.dirname(__file__), "bearing_monitoring.db")

# --- Topologi Jaringan ---
TOTAL_NODES = 20

# Node dianggap OFFLINE jika tidak mengirim data selama sekian detik
OFFLINE_THRESHOLD_SECONDS = 300  # 5 menit

# --- Ambang Batas Visualisasi (bukan deteksi fault otomatis) ---
# Nilai ini HANYA untuk pewarnaan status di dashboard (normal/warning/critical),
# sesuai dengan lingkup sistem sebagai MONITORING, bukan fault detection/RUL.
# Sesuaikan dengan standar/spesifikasi motor yang digunakan di ISC machine.
THRESHOLDS = {
    "temperature": {"warning": 60.0, "critical": 80.0},      # derajat Celsius (PT100)
    "vibration_rms": {"warning": 7.5, "critical": 8.5},      # satuan g (MPU6050, sesuaikan kalibrasi)
}


# --- Penamaan Node (opsional, bisa disesuaikan dengan lokasi fisik bearing) ---
NODE_NAMES = {i: f"Node {i:02d}" for i in range(1, TOTAL_NODES + 1)}
