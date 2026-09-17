const STATUS_LABEL = { normal: "NORMAL", warning: "WARNING", critical: "CRITICAL", offline: "OFFLINE" };

let nodesById = {};
let tempChart, vibChart;
let activeNodeId = null;
let activeRangeHours = 1;
let currentFilter = "all";
let searchQuery = "";
let isSoundEnabled = false;
let audioContext = null;

const nodeGrid = document.getElementById("nodeGrid");
const connDot = document.getElementById("connDot");
const connLabel = document.getElementById("connLabel");
const lastUpdateEl = document.getElementById("lastUpdate");
const anomalyBanner = document.getElementById("anomalyBanner");
const searchInput = document.getElementById("searchInput");
const btnSoundToggle = document.getElementById("btnSoundToggle");
const soundIcon = document.getElementById("soundIcon");
const soundText = document.getElementById("soundText");

function formatTime(iso) {
  if (!iso) return "—";
  let str = iso;
  if (!str.endsWith("Z") && !str.includes("+") && !str.includes("-", 10)) {
    str += "Z";
  }
  const d = new Date(str);
  return d.toLocaleTimeString("id-ID", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function metricClass(status) {
  if (status === "critical") return "crit";
  if (status === "warning") return "warn";
  return "";
}

/* ---------- Sound Alert Siren ---------- */
function triggerAudioAlarm() {
  if (!isSoundEnabled) return;
  try {
    if (!audioContext) {
      audioContext = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (audioContext.state === "suspended") {
      audioContext.resume();
    }
    const osc = audioContext.createOscillator();
    const gain = audioContext.createGain();
    osc.type = "sawtooth";
    osc.frequency.setValueAtTime(880, audioContext.currentTime); // A5 note
    osc.frequency.exponentialRampToValueAtTime(440, audioContext.currentTime + 0.4);
    gain.gain.setValueAtTime(0.3, audioContext.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.01, audioContext.currentTime + 0.4);
    osc.connect(gain);
    gain.connect(audioContext.destination);
    osc.start();
    osc.stop(audioContext.currentTime + 0.4);
  } catch (e) {
    console.error("Audio alarm error:", e);
  }
}

if (btnSoundToggle) {
  btnSoundToggle.addEventListener("click", () => {
    isSoundEnabled = !isSoundEnabled;
    if (isSoundEnabled) {
      btnSoundToggle.classList.add("active");
      soundIcon.textContent = "🔊";
      soundText.textContent = "Sound On";
      triggerAudioAlarm(); // Test beep
    } else {
      btnSoundToggle.classList.remove("active");
      soundIcon.textContent = "🔇";
      soundText.textContent = "Sound Off";
    }
  });
}

/* ---------- Filtering & Render Node Cards ---------- */
function renderNodeCard(node) {
  const status = node.online === false ? "offline" : node.status;
  const card = document.createElement("div");
  card.className = `node-card status-${status}`;
  card.tabIndex = 0;
  card.dataset.nodeId = node.node_id;

  const rssiText = node.rssi !== null && node.rssi !== undefined ? `${node.rssi} dBm` : "—";
  const tempText = node.temperature !== null && node.temperature !== undefined ? node.temperature.toFixed(1) : "—";
  const vibText = node.vibration_rms !== null && node.vibration_rms !== undefined ? node.vibration_rms.toFixed(2) : "—";

  card.innerHTML = `
    <div class="node-card-header">
      <span class="node-name">${node.node_name}</span>
      <span class="badge ${status.toUpperCase()}">${STATUS_LABEL[status] || status.toUpperCase()}</span>
    </div>
    <div class="node-metrics-big">
      <div class="big-metric">
        <span class="big-metric-value ${metricClass(status)}">${tempText}<span class="big-metric-unit">&deg;C</span></span>
        <span class="big-metric-label">Suhu Motor</span>
      </div>
      <div class="big-metric-divider"></div>
      <div class="big-metric">
        <span class="big-metric-value ${metricClass(status)}">${vibText}<span class="big-metric-unit">mm/s</span></span>
        <span class="big-metric-label">Vibrasi RMS</span>
      </div>
    </div>
    <div class="node-submetrics">
      <span>📶 ${rssiText}</span>
    </div>
    <div class="node-footer">
      <span>${status === "offline" ? "⚪ OFFLINE" : "🟢 LIVE TELEMETRY"}</span>
      <span>${formatTime(node.timestamp)}</span>
    </div>
  `;

  card.addEventListener("click", () => openModal(node.node_id));
  card.addEventListener("keypress", (e) => {
    if (e.key === "Enter") openModal(node.node_id);
  });
  return card;
}

function renderAllNodes() {
  nodeGrid.innerHTML = "";
  let nodes = Object.values(nodesById).sort((a, b) => a.node_id - b.node_id);
  
  updateSummary(nodes);

  // Filter pencarian teks
  if (searchQuery.trim() !== "") {
    const q = searchQuery.toLowerCase();
    nodes = nodes.filter(n => 
      n.node_name.toLowerCase().includes(q) || 
      n.node_id.toString() === q
    );
  }

  // Filter status pills
  if (currentFilter !== "all") {
    nodes = nodes.filter(n => {
      const s = n.online === false ? "offline" : n.status;
      return s === currentFilter;
    });
  }

  if (nodes.length === 0) {
    nodeGrid.innerHTML = `<div style="grid-column: 1/-1; text-align: center; padding: 40px; color: var(--text-dim); font-family: var(--mono);">
      Tidak ada node yang sesuai dengan filter / pencarian "${searchQuery}".
    </div>`;
    return;
  }

  nodes.forEach((node) => nodeGrid.appendChild(renderNodeCard(node)));
}

function updateSummary(nodes) {
  const counts = { normal: 0, warning: 0, critical: 0, offline: 0 };
  let hasCritical = false;
  let hasWarning = false;

  nodes.forEach((n) => {
    const s = n.online === false ? "offline" : n.status;
    counts[s] = (counts[s] || 0) + 1;
    if (s === "critical") hasCritical = true;
    if (s === "warning") hasWarning = true;
  });

  const statTotal = document.getElementById("statTotal");
  const statNormal = document.getElementById("statNormal");
  const statWarning = document.getElementById("statWarning");
  const statCritical = document.getElementById("statCritical");
  const statOffline = document.getElementById("statOffline");

  if (statTotal) statTotal.textContent = nodes.length;
  if (statNormal) statNormal.textContent = counts.normal;
  if (statWarning) statWarning.textContent = counts.warning;
  if (statCritical) statCritical.textContent = counts.critical;
  if (statOffline) statOffline.textContent = counts.offline;

  // Anomaly banner display & trigger audio
  if (anomalyBanner) {
    if (hasCritical) {
      anomalyBanner.style.display = "block";
      anomalyBanner.style.background = "rgba(239, 68, 68, 0.16)";
      anomalyBanner.style.borderColor = "var(--red)";
      anomalyBanner.style.color = "#fca5a5";
      anomalyBanner.textContent = "⚠️ PERINGATAN KRITIS: TERDETEKSI SUHU / VIBRASI KRITIS PADA BEARING MOTOR!";
      triggerAudioAlarm();
    } else if (hasWarning) {
      anomalyBanner.style.display = "block";
      anomalyBanner.style.background = "rgba(245, 158, 11, 0.16)";
      anomalyBanner.style.borderColor = "var(--amber)";
      anomalyBanner.style.color = "#fde68a";
      anomalyBanner.textContent = "⚠️ PERINGATAN: TERDETEKSI BEARING MOTOR DI LUAR AMBANG BATAS NORMAL (WARNING)!";
    } else {
      anomalyBanner.style.display = "none";
    }
  }
}

/* ---------- Search & Filter Listeners ---------- */
if (searchInput) {
  searchInput.addEventListener("input", (e) => {
    searchQuery = e.target.value;
    renderAllNodes();
  });
}

document.querySelectorAll(".filter-pill, .summary-item").forEach((pill) => {
  pill.addEventListener("click", () => {
    const filter = pill.dataset.filter;
    if (!filter) return;
    currentFilter = filter;
    
    document.querySelectorAll(".filter-pill").forEach(p => {
      p.classList.toggle("active", p.dataset.filter === currentFilter);
    });
    renderAllNodes();
  });
});

async function loadNodes() {
  try {
    const res = await fetch("/api/nodes");
    const data = await res.json();
    nodesById = {};
    data.forEach((n) => (nodesById[n.node_id] = n));
    renderAllNodes();
    if (lastUpdateEl) lastUpdateEl.textContent = new Date().toLocaleTimeString("id-ID");
  } catch (e) {
    console.error("Gagal memuat data node:", e);
  }
}

/* ---------- WebSocket Live Update ---------- */
let pingInterval = null;

function connectWebSocket() {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${protocol}//${window.location.host}/ws`);

  ws.onopen = () => {
    if (connDot) {
      connDot.classList.remove("offline");
      connDot.classList.add("online");
    }
    if (connLabel) connLabel.textContent = "SIAP MENERIMA DATA ESP32";
    if (pingInterval) clearInterval(pingInterval);
    pingInterval = setInterval(() => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send("ping");
      }
    }, 25000);
  };

  ws.onclose = () => {
    if (connDot) {
      connDot.classList.remove("online");
      connDot.classList.add("offline");
    }
    if (connLabel) connLabel.textContent = "TERPUTUS, MENCOBA LAGI...";
    if (pingInterval) clearInterval(pingInterval);
    setTimeout(connectWebSocket, 3000);
  };

  ws.onerror = () => ws.close();

  ws.onmessage = (event) => {
    if (event.data === "pong") return;
    try {
      const node = JSON.parse(event.data);
      nodesById[node.node_id] = node;
      renderAllNodes();
      if (lastUpdateEl) lastUpdateEl.textContent = new Date().toLocaleTimeString("id-ID");
      if (activeNodeId === node.node_id) {
        renderModalDetail(node);
        loadHistory(activeNodeId, activeRangeHours);
      }
    } catch (err) {
      console.error("Gagal membaca data WebSocket:", err);
    }
  };
}

/* ---------- Modal & History Charts ---------- */
const modalOverlay = document.getElementById("modalOverlay");
const modalTitle = document.getElementById("modalTitle");
const modalSubtitle = document.getElementById("modalSubtitle");
const modalDetailGrid = document.getElementById("modalDetailGrid");
const btnModalExportCsv = document.getElementById("btnModalExportCsv");

function renderModalDetail(node) {
  const status = node.online === false ? "offline" : node.status;
  modalDetailGrid.innerHTML = `
    <div class="detail-item">
      <span class="detail-label">Suhu Motor</span>
      <span class="detail-value ${metricClass(status)}">${node.temperature !== undefined ? node.temperature.toFixed(1) : "—"} &deg;C</span>
    </div>
    <div class="detail-item">
      <span class="detail-label">Vibrasi RMS</span>
      <span class="detail-value ${metricClass(status)}">${node.vibration_rms !== undefined ? node.vibration_rms.toFixed(2) : "—"} mm/s</span>
    </div>
    <div class="detail-item">
      <span class="detail-label">Sinyal LoRa (RSSI)</span>
      <span class="detail-value">${node.rssi ?? "—"} dBm</span>
    </div>
    <div class="detail-item">
      <span class="detail-label">Status Operasional</span>
      <span class="detail-value"><span class="badge ${status.toUpperCase()}">${STATUS_LABEL[status] || status.toUpperCase()}</span></span>
    </div>
    <div class="detail-item">
      <span class="detail-label">Waktu Telemetry</span>
      <span class="detail-value">${formatTime(node.timestamp)}</span>
    </div>
  `;

  if (btnModalExportCsv) {
    btnModalExportCsv.href = `/api/export?hours=24&node_id=${node.node_id}`;
  }
}

function openModal(nodeId) {
  activeNodeId = nodeId;
  const node = nodesById[nodeId];
  if (!node) return;
  if (modalTitle) modalTitle.textContent = `${node.node_name} — Detail Telemetry`;
  if (modalSubtitle) modalSubtitle.textContent = `Monitoring sensor real-time & historis (ID Node: ${node.node_id})`;
  renderModalDetail(node);
  if (modalOverlay) modalOverlay.classList.add("open");
  loadHistory(nodeId, activeRangeHours);
}

const modalClose = document.getElementById("modalClose");
if (modalClose) {
  modalClose.addEventListener("click", () => {
    modalOverlay.classList.remove("open");
    activeNodeId = null;
  });
}

if (modalOverlay) {
  modalOverlay.addEventListener("click", (e) => {
    if (e.target === modalOverlay) {
      modalOverlay.classList.remove("open");
      activeNodeId = null;
    }
  });
}

document.querySelectorAll(".range-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".range-btn").forEach((b) => b.classList.remove("active"));
    btn.classList.add("active");
    activeRangeHours = parseInt(btn.dataset.hours, 10);
    if (activeNodeId) loadHistory(activeNodeId, activeRangeHours);
  });
});

async function loadHistory(nodeId, hours) {
  try {
    const res = await fetch(`/api/nodes/${nodeId}/history?hours=${hours}`);
    const data = await res.json();
    const labels = data.map((d) => formatTime(d.timestamp));
    const temps = data.map((d) => d.temperature);
    const vibs = data.map((d) => d.vibration_rms);
    renderCharts(labels, temps, vibs);
  } catch (e) {
    console.error("Gagal memuat riwayat:", e);
  }
}

function renderCharts(labels, temps, vibs) {
  if (typeof Chart === "undefined") {
    console.error("Chart.js belum siap atau gagal dimuat.");
    return;
  }

  const commonOpts = {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    scales: {
      x: {
        ticks: { color: "#8b9bb4", maxTicksLimit: 8, font: { family: "'Bekaert', 'Segoe UI', Tahoma, Arial, sans-serif", size: 10 } },
        grid: { color: "#1e2c3e" }
      },
      y: {
        ticks: { color: "#8b9bb4", font: { family: "'Bekaert', 'Segoe UI', Tahoma, Arial, sans-serif", size: 10 } },
        grid: { color: "#1e2c3e" }
      },
    },
    plugins: {
      legend: {
        labels: {
          color: "#f0f4f8",
          font: { family: "'Bekaert', 'Segoe UI', Tahoma, Arial, sans-serif", size: 11, weight: "bold" }
        }
      }
    },
  };

  if (tempChart) tempChart.destroy();
  if (vibChart) vibChart.destroy();

  const tempCanvas = document.getElementById("tempChart");
  if (tempCanvas) {
    tempChart = new Chart(tempCanvas, {
      type: "line",
      data: {
        labels,
        datasets: [{
          label: "Suhu Motor (°C)",
          data: temps,
          borderColor: "#f59e0b",
          backgroundColor: "rgba(245, 158, 11, 0.12)",
          tension: 0.3,
          fill: true,
          pointRadius: 3,
        }],
      },
      options: commonOpts,
    });
  }

  const vibCanvas = document.getElementById("vibChart");
  if (vibCanvas) {
    vibChart = new Chart(vibCanvas, {
      type: "line",
      data: {
        labels,
        datasets: [{
          label: "Vibrasi RMS (mm/s)",
          data: vibs,
          borderColor: "#06b6d4",
          backgroundColor: "rgba(6, 182, 212, 0.12)",
          tension: 0.3,
          fill: true,
          pointRadius: 3,
        }],
      },
      options: commonOpts,
    });
  }
}

/* ---------- Init ---------- */
loadNodes();
connectWebSocket();
setInterval(loadNodes, 30000);
