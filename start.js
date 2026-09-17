const { spawn, execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const backendDir = path.join(__dirname, 'backend');
const isWin = process.platform === 'win32';
const venvDir = path.join(backendDir, 'venv');
const pythonExe = isWin
  ? path.join(venvDir, 'Scripts', 'python.exe')
  : path.join(venvDir, 'bin', 'python');
const uvicornExe = isWin
  ? path.join(venvDir, 'Scripts', 'uvicorn.exe')
  : path.join(venvDir, 'bin', 'uvicorn');

console.log('----------------------------------------------------');
console.log(' ⚙️  Sistem Monitoring Kondisi Bearing Motor');
console.log('----------------------------------------------------');

// 1. Cek / buat virtual environment
if (!fs.existsSync(venvDir)) {
  console.log('📦 Virtual environment belum ada. Membuat venv baru...');
  try {
    execSync('python -m venv venv', { cwd: backendDir, stdio: 'inherit' });
  } catch (err) {
    console.error('❌ Gagal membuat venv. Pastikan Python sudah terinstall.');
    process.exit(1);
  }
}

// 2. Install requirements jika uvicorn belum terinstall di venv
if (!fs.existsSync(uvicornExe)) {
  console.log('📥 Menginstall dependency dari requirements.txt...');
  try {
    execSync(`"${pythonExe}" -m pip install -r requirements.txt`, { cwd: backendDir, stdio: 'inherit' });
  } catch (err) {
    console.error('❌ Gagal menginstall dependencies.');
    process.exit(1);
  }
}

const os = require('os');

function getLocalIPs() {
  const interfaces = os.networkInterfaces();
  const addresses = [];
  for (const name of Object.keys(interfaces)) {
    for (const net of interfaces[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        addresses.push({ name, ip: net.address });
      }
    }
  }
  return addresses;
}

// 3. Deteksi Alamat IP Jaringan
const localIPs = getLocalIPs();
console.log('🌐 Alamat Akses Server:');
console.log(`   - Lokal Komputer : http://localhost:8000`);
if (localIPs.length > 0) {
  localIPs.forEach(addr => {
    console.log(`   - Jaringan (${addr.name}): http://${addr.ip}:8000  <-- Gunakan IP ini di Gateway ESP32`);
  });
} else {
  console.log(`   - Jaringan       : Tidak terdeteksi koneksi WiFi/LAN aktif`);
}
console.log('\n🚀 Menjalankan server... (Tekan Ctrl+C untuk menghentikan server)\n');

const child = spawn(uvicornExe, ['main:app', '--reload', '--host', '0.0.0.0', '--port', '8000'], {
  cwd: backendDir,
  stdio: 'inherit',
  shell: false
});

child.on('exit', (code) => {
  if (code !== 0 && code !== null) {
    console.log(`Server berhenti dengan code exit: ${code}`);
  }
});

