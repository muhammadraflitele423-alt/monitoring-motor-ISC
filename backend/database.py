"""
Modul database untuk penyimpanan data time-series hasil monitoring.
Menggunakan SQLite (ringan, lokal, tanpa perlu instalasi server database).
"""
from datetime import datetime, timedelta
from typing import Optional

from sqlalchemy import Column, DateTime, Float, Index, Integer, create_engine
from sqlalchemy.orm import declarative_base, sessionmaker

import config

Base = declarative_base()


class Reading(Base):
    __tablename__ = "readings"

    id = Column(Integer, primary_key=True, autoincrement=True)
    node_id = Column(Integer, nullable=False)
    temperature = Column(Float, nullable=False)
    vibration_rms = Column(Float, nullable=False)
    battery_voltage = Column(Float, nullable=False)
    rssi = Column(Integer, nullable=True)
    timestamp = Column(DateTime, default=datetime.utcnow, nullable=False)


# Index gabungan node_id + timestamp mempercepat query history per node
Index("ix_node_time", Reading.node_id, Reading.timestamp)

if config.USE_MYSQL:
    # Build MySQL connection URL dengan driver pymysql
    mysql_url = f"mysql+pymysql://{config.MYSQL_USER}:{config.MYSQL_PASSWORD}@{config.MYSQL_HOST}:{config.MYSQL_PORT}/{config.MYSQL_DATABASE}"
    connect_args = {}
    if config.MYSQL_TLS:
        connect_args["ssl"] = {}
    engine = create_engine(mysql_url, connect_args=connect_args)
else:
    # Fallback ke SQLite (untuk pengembangan lokal)
    engine = create_engine(
        f"sqlite:///{config.DB_PATH}",
        connect_args={"check_same_thread": False},
    )
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False)


def init_db():
    Base.metadata.create_all(engine)


def insert_reading(data: dict) -> Reading:
    """Menyimpan satu baris pembacaan sensor ke database."""
    session = SessionLocal()
    try:
        reading = Reading(
            node_id=int(data["node_id"]),
            temperature=float(data["temp"]),
            vibration_rms=float(data["vib_rms"]),
            battery_voltage=float(data["battery"]),
            rssi=int(data["rssi"]) if data.get("rssi") is not None else None,
        )
        session.add(reading)
        session.commit()
        session.refresh(reading)
        return reading
    finally:
        session.close()


def get_latest_per_node() -> list[Reading]:
    """Mengambil pembacaan terbaru untuk setiap node (untuk kartu status dashboard)."""
    session = SessionLocal()
    try:
        results = []
        for node_id in range(1, config.TOTAL_NODES + 1):
            reading = (
                session.query(Reading)
                .filter(Reading.node_id == node_id)
                .order_by(Reading.timestamp.desc())
                .first()
            )
            if reading:
                results.append(reading)
        return results
    finally:
        session.close()


def get_history(node_id: int, hours: int = 24) -> list[Reading]:
    """Mengambil riwayat pembacaan satu node dalam N jam terakhir (untuk grafik)."""
    session = SessionLocal()
    try:
        since = datetime.utcnow() - timedelta(hours=hours)
        return (
            session.query(Reading)
            .filter(Reading.node_id == node_id, Reading.timestamp >= since)
            .order_by(Reading.timestamp.asc())
            .all()
        )
    finally:
        session.close()


def get_all_history(hours: int = 24, node_id: Optional[int] = None) -> list[Reading]:
    """Mengambil riwayat pembacaan seluruh node (atau node spesifik) dalam N jam terakhir (untuk ekspor CSV)."""
    session = SessionLocal()
    try:
        since = datetime.utcnow() - timedelta(hours=hours)
        query = session.query(Reading).filter(Reading.timestamp >= since)
        if node_id is not None:
            query = query.filter(Reading.node_id == node_id)
        return query.order_by(Reading.timestamp.desc()).all()
    finally:
        session.close()

