"""
Sistem Monitoring Kondisi Bearing Motor pada Mesin ISC - PT Bekaert Indonesia
Backend FastAPI: menerima data dari Gateway ESP32+LoRa lewat WiFi (HTTP POST),
menyimpannya ke SQLite, dan menyajikan dashboard web + WebSocket live update.
"""
import asyncio
from datetime import datetime
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Header, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

import config
import database

BASE_DIR = Path(__file__).resolve().parent
FRONTEND_DIR = BASE_DIR.parent / "frontend"

app = FastAPI(title="Bearing Motor Condition Monitoring - PT Bekaert Indonesia")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

app.mount("/static", StaticFiles(directory=str(FRONTEND_DIR / "static")), name="static")

main_loop = None
connected_clients: list[WebSocket] = []

import csv
import io
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from pydantic import BaseModel, Field

class NodeReading(BaseModel):
    node_id: int = Field(..., ge=1, le=config.TOTAL_NODES, description="ID Node 1-20")
    temp: float = Field(..., ge=-50.0, le=150.0, description="Suhu dalam Celcius (-50 s/d 150)")
    vib_rms: float = Field(..., ge=0.0, le=50.0, description="Vibrasi RMS dalam mm/s (0 s/d 50)")
    battery: Optional[float] = Field(None, ge=0.0, le=10.0, description="Tegangan baterai (tidak ditampilkan)")
    rssi: Optional[int] = Field(None, ge=-150, le=0, description="RSSI Signal (-150 s/d 0)")


def evaluate_status(reading) -> str:
    """Menentukan status visual (normal/warning/critical) berdasarkan ambang batas.
    Hanya berdasarkan Suhu dan Vibrasi RMS — baterai tidak digunakan."""
    t = config.THRESHOLDS
    if (
        reading.temperature >= t["temperature"]["critical"]
        or reading.vibration_rms >= t["vibration_rms"]["critical"]
    ):
        return "critical"
    if (
        reading.temperature >= t["temperature"]["warning"]
        or reading.vibration_rms >= t["vibration_rms"]["warning"]
    ):
        return "warning"
    return "normal"


def reading_to_dict(reading, online: Optional[bool] = None) -> dict:
    ts = reading.timestamp.isoformat()
    if not ts.endswith("Z"):
        ts += "Z"
    d = {
        "node_id": reading.node_id,
        "node_name": config.NODE_NAMES.get(reading.node_id, f"Node {reading.node_id}"),
        "temperature": reading.temperature,
        "vibration_rms": reading.vibration_rms,
        "rssi": reading.rssi,
        "timestamp": ts,
        "status": evaluate_status(reading),
    }
    if online is not None:
        d["online"] = online
    return d


async def broadcast(payload: dict):
    dead_clients = []
    for client in connected_clients:
        try:
            await client.send_json(payload)
        except Exception:
            dead_clients.append(client)
    for client in dead_clients:
        if client in connected_clients:
            connected_clients.remove(client)


def get_local_ips() -> list[str]:
    import socket
    ips = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        main_ip = s.getsockname()[0]
        s.close()
        if main_ip and not main_ip.startswith("127."):
            ips.append(main_ip)
    except Exception:
        pass
    try:
        hostname = socket.gethostname()
        for ip in socket.gethostbynameex(hostname)[2]:
            if not ip.startswith("127.") and ip not in ips:
                ips.append(ip)
    except Exception:
        pass
    return ips


@app.on_event("startup")
async def startup_event():
    global main_loop
    main_loop = asyncio.get_event_loop()
    database.init_db()
    local_ips = get_local_ips()
    print(f"[Startup] Server siap di http://localhost:{config.PORT}")
    if local_ips:
        for ip in local_ips:
            print(f"[Startup] IP Jaringan WiFi/LAN: http://{ip}:{config.PORT} (Gunakan IP ini di Gateway ESP32)")
    else:
        print("[Startup] PERINGATAN: Tidak terdeteksi IP jaringan WiFi/LAN aktif")
    print("[Startup] Menunggu POST data dari Gateway ESP32 ke /api/ingest ...")


@app.get("/")
async def serve_dashboard():
    response = FileResponse(str(FRONTEND_DIR / "index.html"))
    response.headers["Cache-Control"] = "no-cache, no-store, must-revalidate"
    response.headers["Pragma"] = "no-cache"
    response.headers["Expires"] = "0"
    return response


@app.get("/api/health")
async def health_check():
    """Endpoint status kesehatan backend server"""
    return {
        "status": "healthy",
        "timestamp": datetime.utcnow().isoformat(),
        "active_ws_clients": len(connected_clients),
        "total_nodes_monitored": config.TOTAL_NODES
    }


@app.post("/api/ingest")
async def ingest_reading(reading: NodeReading, x_api_key: Optional[str] = Header(None)):
    """
    Endpoint yang dipanggil oleh Gateway ESP32 lewat WiFi (HTTP POST) setiap kali
    ada data baru dari LoRa.
    """
    if config.API_KEY and x_api_key != config.API_KEY:
        raise HTTPException(status_code=401, detail="API key tidak valid")

    db_reading = database.insert_reading(reading.model_dump())
    payload = reading_to_dict(db_reading, online=True)
    await broadcast(payload)
    return {"status": "ok", "received": payload}


@app.get("/api/nodes")
async def get_nodes():
    readings = database.get_latest_per_node()
    now = datetime.utcnow()
    readings_by_id = {r.node_id: r for r in readings}
    result = []
    for node_id in range(1, config.TOTAL_NODES + 1):
        if node_id in readings_by_id:
            r = readings_by_id[node_id]
            age_seconds = (now - r.timestamp).total_seconds()
            online = age_seconds <= config.OFFLINE_THRESHOLD_SECONDS
            result.append(reading_to_dict(r, online=online))
        else:
            result.append({
                "node_id": node_id,
                "node_name": config.NODE_NAMES.get(node_id, f"Node {node_id:02d}"),
                "temperature": 0.0,
                "vibration_rms": 0.0,
                "battery_voltage": 0.0,
                "rssi": None,
                "timestamp": None,
                "status": "offline",
                "online": False
            })
    return JSONResponse(result)


@app.get("/api/nodes/{node_id}/history")
async def get_node_history(node_id: int, hours: int = 24):
    readings = database.get_history(node_id, hours)
    return JSONResponse([reading_to_dict(r) for r in readings])


@app.get("/api/export")
async def export_csv(hours: int = 24, node_id: Optional[int] = None):
    """Ekspor log telemetry historis dalam format CSV"""
    readings = database.get_all_history(hours=hours, node_id=node_id)
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(["Timestamp", "Node ID", "Node Name", "Suhu (°C)", "Vibrasi RMS (mm/s)", "RSSI (dBm)", "Status"])
    
    for r in readings:
        writer.writerow([
            r.timestamp.isoformat(),
            r.node_id,
            config.NODE_NAMES.get(r.node_id, f"Node {r.node_id}"),
            r.temperature,
            r.vibration_rms,
            r.rssi if r.rssi is not None else "",
            evaluate_status(r)
        ])
    
    output.seek(0)
    filename = f"bearing_monitoring_{datetime.utcnow().strftime('%Y%m%d_%H%M%S')}.csv"
    return StreamingResponse(
        io.BytesIO(output.getvalue().encode('utf-8')),
        media_type="text/csv",
        headers={"Content-Disposition": f"attachment; filename={filename}"}
    )


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    connected_clients.append(websocket)
    try:
        while True:
            data = await websocket.receive_text()
            # Tanggapi pesan ping dari client untuk menjaga koneksi tetap hidup
            if data == "ping":
                await websocket.send_text("pong")
    except WebSocketDisconnect:
        if websocket in connected_clients:
            connected_clients.remove(websocket)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host=config.HOST, port=config.PORT, reload=True)

