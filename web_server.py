import serial
import threading
import time
import re
import socket
import sqlite3
from datetime import datetime
from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# --- Serial Configuration ---
SERIAL_PORT = "/dev/ttyUSB2"
BAUD_RATE = 115200
DB_PATH = "configs.db"

ser = None
clients_db = {}
lock = threading.Lock()
pending_show_mac = None
initial_sync_done = False
last_show_request = {}

telegraf_config = {"host": "localhost", "port": 8094, "protocol": "udp"}

def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    with get_db() as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS client_prefs (
                mac TEXT PRIMARY KEY,
                name TEXT NOT NULL DEFAULT '',
                log_enabled INTEGER NOT NULL DEFAULT 0,
                tlg_bucket TEXT NOT NULL DEFAULT 'sensors'
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS app_config (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
        """)
        conn.commit()

def load_app_config():
    with get_db() as conn:
        rows = conn.execute("SELECT key, value FROM app_config").fetchall()
    return {row["key"]: row["value"] for row in rows}

def save_app_config(key, value):
    with get_db() as conn:
        conn.execute("""
            INSERT INTO app_config(key, value)
            VALUES(?, ?)
            ON CONFLICT(key) DO UPDATE SET value = excluded.value
        """, (key, str(value)))
        conn.commit()

def load_client_pref(mac):
    with get_db() as conn:
        row = conn.execute(
            "SELECT mac, name, log_enabled, tlg_bucket FROM client_prefs WHERE mac = ?",
            (mac,),
        ).fetchone()
    if not row:
        return None
    return {
        "mac": row["mac"],
        "name": row["name"],
        "log_enabled": bool(row["log_enabled"]),
        "tlg_bucket": row["tlg_bucket"],
    }

def save_client_pref(mac, name=None, log_enabled=None, tlg_bucket=None):
    existing = load_client_pref(mac) or {
        "mac": mac,
        "name": "",
        "log_enabled": False,
        "tlg_bucket": "sensors",
    }
    updated = {
        "mac": mac,
        "name": existing["name"] if name is None else name,
        "log_enabled": existing["log_enabled"] if log_enabled is None else bool(log_enabled),
        "tlg_bucket": existing["tlg_bucket"] if tlg_bucket is None else tlg_bucket,
    }
    with get_db() as conn:
        conn.execute("""
            INSERT INTO client_prefs(mac, name, log_enabled, tlg_bucket)
            VALUES(?, ?, ?, ?)
            ON CONFLICT(mac) DO UPDATE SET
                name = excluded.name,
                log_enabled = excluded.log_enabled,
                tlg_bucket = excluded.tlg_bucket
        """, (updated["mac"], updated["name"], int(updated["log_enabled"]), updated["tlg_bucket"]))
        conn.commit()
    return updated

def merge_client_state(mac, updates):
    persisted = load_client_pref(mac) or {}
    existing = clients_db.get(mac, {})
    return {
        "log_enabled": False,
        "tlg_bucket": "sensors",
        **persisted,
        **existing,
        **updates,
    }

def request_show(mac, min_interval_sec=2.0):
    now = time.time()
    last_ts = last_show_request.get(mac, 0.0)
    if now - last_ts < min_interval_sec:
        return
    s = get_serial()
    if s:
        s.write(f"show {mac}\n".encode())
        last_show_request[mac] = now

def send_to_telegraf(mac, name, bucket, v, t, h, p, tv, tt, pv, pt):
    try:
        clean_name = name.replace(" ", "_") if name else "unknown"
        clean_bucket = bucket.replace(" ", "_") if bucket else "default"
        line = (
            f"esp32_sensor,mac={mac},name={clean_name},bucket={clean_bucket} "
            f"v_batt={v},temperature={t},humidity={h},pressure={p},"
            f"temphumid_validcount={tv}i,temphumid_trial={tt}i,"
            f"pressure_validcount={pv}i,pressure_trial={pt}i\n"
        )
        if telegraf_config["protocol"].lower() == "udp":
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(line.encode(), (telegraf_config["host"], int(telegraf_config["port"])))
        else:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(2); s.connect((telegraf_config["host"], int(telegraf_config["port"]))); s.sendall(line.encode()); s.close()
    except Exception as e: print(f"Telegraf Error: {e}")

def get_serial():
    global ser
    if ser is None or not ser.is_open:
        try: ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
        except Exception as e: print(f"Serial Open Error: {e}")
    return ser

def serial_reader_task():
    global clients_db, pending_show_mac, initial_sync_done
    MAC_PTN = r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}"
    VAL_PTN = r"[0-9.-]+"
    HASH_PTN = r"[0-9A-F]+"
    detail_patterns = {
        "name": re.compile(r"^\s*Name:\s*(?P<value>.+)$"),
        "sleep": re.compile(r"^\s*Sleep:\s*(?P<value>\d+)\s+s$"),
        "work_delay_ms": re.compile(r"^\s*Work Delay:\s*(?P<value>\d+)\s+ms$"),
        "batt_avg": re.compile(r"^\s*Batt Avg:\s*(?P<value>\d+)$"),
        "sht_avg": re.compile(r"^\s*SHT Avg:\s*(?P<value>\d+)$"),
        "sht_read_wait_time_ms": re.compile(r"^\s*SHT Wait:\s*(?P<value>\d+)\s+ms$"),
        "dps_osr": re.compile(r"^\s*DPS OSR:\s*(?P<value>\d+)$"),
        "dps_avg": re.compile(r"^\s*DPS Avg:\s*(?P<value>\d+)$"),
        "dps_read_wait_time_ms": re.compile(r"^\s*DPS Wait:\s*(?P<value>\d+)\s+ms$"),
        "hash": re.compile(r"^\s*Hash:\s*(?P<value>" + HASH_PTN + r")$"),
    }

    while True:
        s = get_serial()
        if s:
            try:
                if not initial_sync_done:
                    s.write(b"ls\n")
                    initial_sync_done = True
                    time.sleep(0.4)
                raw_line = s.readline().decode('utf-8', errors='ignore').strip()
                if not raw_line: continue
                line = "".join(c for c in raw_line if c.isprintable())

                # 1. Parse LS output: [Name] MAC | Default|Custom
                ls_match = re.search(fr"\[(?P<name>.*?)\]\s+(?P<mac>{MAC_PTN})\s+\|\s+(?P<profile>Default|Custom)", line)
                if ls_match:
                    d = ls_match.groupdict()
                    with lock:
                        clients_db[d['mac']] = merge_client_state(d['mac'], {
                            "mac": d['mac'],
                            "name": "" if d['name'] == "(unnamed)" else d['name'],
                            "profile": d['profile']
                        })
                        if d['name'] and d['name'] != "(unnamed)":
                            save_client_pref(d['mac'], name=d['name'])
                    request_show(d['mac'])
                    continue

                # 2. Parse show header: Config for MAC (Custom):
                show_header = re.search(fr"Config for (?P<mac>{MAC_PTN}) \((?P<profile>Default|Custom)\):", line)
                if show_header:
                    d = show_header.groupdict()
                    with lock:
                        pending_show_mac = d["mac"]
                        clients_db[d["mac"]] = merge_client_state(d["mac"], {
                            "mac": d["mac"], "profile": d["profile"]
                        })
                    continue

                # 3. Parse show details following the show header.
                if pending_show_mac:
                    matched_detail = False
                    for field, pattern in detail_patterns.items():
                        detail_match = pattern.match(line)
                        if detail_match:
                            value = detail_match.group("value").strip()
                            with lock:
                                current = merge_client_state(pending_show_mac, {"mac": pending_show_mac})
                                if field == "name":
                                    current[field] = "" if value == "N/A" else value
                                    save_client_pref(pending_show_mac, name=current[field])
                                elif field == "hash":
                                    current[field] = value
                                else:
                                    current[field] = int(value)
                                clients_db[pending_show_mac] = current
                            matched_detail = True
                            if field == "hash":
                                pending_show_mac = None
                            break
                    if matched_detail:
                        continue

                # 4. Parse DATA logs: DATA [Name (MAC)] V:X T:X H:X P:X TV:X TT:X PV:X PT:X Hash:X
                data_match = re.search(
                    fr"DATA \[(?:(?P<name>[^\[\]\(\)]+)\s+\()?(?P<mac>{MAC_PTN})\)?\]\s+"
                    fr"V:(?P<v>{VAL_PTN})\s+T:(?P<t>{VAL_PTN})\s+H:(?P<h>{VAL_PTN})\s+P:(?P<p>{VAL_PTN})\s+"
                    fr"TV:(?P<tv>\d+)\s+TT:(?P<tt>\d+)\s+PV:(?P<pv>\d+)\s+PT:(?P<pt>\d+)\s+Hash:(?P<hash>{HASH_PTN})",
                    line
                )
                if data_match:
                    d = data_match.groupdict()
                    with lock:
                        mac = d['mac']
                        existing = merge_client_state(mac, {"mac": mac})
                        name = d['name'] if d['name'] else existing.get("name", "")
                        clients_db[mac] = {
                            **existing,
                            "mac": mac, "name": name, "v_batt": d['v'], "temperature": d['t'], 
                            "humidity": d['h'], "pressure": d['p'],
                            "temphumid_validcount": int(d['tv']), "temphumid_trial": int(d['tt']),
                            "pressure_validcount": int(d['pv']), "pressure_trial": int(d['pt']),
                            "hash": d['hash'],
                            "last_seen": datetime.now().strftime("%Y/%m/%d %H:%M:%S")
                        }
                        if name:
                            save_client_pref(mac, name=name)
                        if clients_db[mac].get("log_enabled"):
                            send_to_telegraf(
                                mac, name, clients_db[mac].get("tlg_bucket"),
                                d['v'], d['t'], d['h'], d['p'],
                                d['tv'], d['tt'], d['pv'], d['pt']
                            )
                    if "sleep" not in clients_db[mac]:
                        request_show(mac)
            except Exception as e: print(f"Read error: {e}"); time.sleep(1)
        else: time.sleep(2)

init_db()
stored_app_cfg = load_app_config()
telegraf_config.update({
    "host": stored_app_cfg.get("telegraf_host", telegraf_config["host"]),
    "port": int(stored_app_cfg.get("telegraf_port", telegraf_config["port"])),
    "protocol": stored_app_cfg.get("telegraf_protocol", telegraf_config["protocol"]),
})
threading.Thread(target=serial_reader_task, daemon=True).start()

app = FastAPI()
class TlgConfig(BaseModel): host: str; port: int; protocol: str
class ClientLogSet(BaseModel): mac: str; enabled: bool; bucket: str

@app.get("/api/clients")
def get_clients():
    with lock: return list(clients_db.values())

@app.get("/api/telegraf")
def get_tlg(): return telegraf_config

@app.post("/api/telegraf")
def set_tlg(cfg: TlgConfig):
    global telegraf_config
    telegraf_config.update(cfg.dict())
    save_app_config("telegraf_host", telegraf_config["host"])
    save_app_config("telegraf_port", telegraf_config["port"])
    save_app_config("telegraf_protocol", telegraf_config["protocol"])
    return {"status": "ok"}

@app.post("/api/client_log")
def set_client_log(data: ClientLogSet):
    with lock:
        if data.mac in clients_db:
            clients_db[data.mac]["log_enabled"] = data.enabled
            clients_db[data.mac]["tlg_bucket"] = data.bucket
        save_client_pref(data.mac, log_enabled=data.enabled, tlg_bucket=data.bucket)
    return {"status": "ok"}

@app.post("/api/fetch")
def fetch_server():
    global initial_sync_done
    s = get_serial()
    if s:
        s.write(b"ls\n")
        initial_sync_done = True
        time.sleep(0.4)
        with lock:
            macs = list(clients_db.keys())
        for mac in macs:
            s.write(f"show {mac}\n".encode())
            time.sleep(0.15)
    return {"status": "ok"}

@app.post("/api/set")
def set_config(cfg: dict):
    s = get_serial(); s.write(f"set {cfg['mac']} {cfg['key']} {cfg['val']}\n".encode()) if s else None
    return {"status": "ok"}

@app.post("/api/name")
def set_name(data: dict):
    with lock:
        if data["mac"] in clients_db:
            clients_db[data["mac"]]["name"] = data["name"]
        save_client_pref(data["mac"], name=data["name"])
    s = get_serial(); s.write(f"name {data['mac']} {data['name']}\n".encode()) if s else None
    return {"status": "ok"}

app.mount("/", StaticFiles(directory="static", html=True), name="static")

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8082)
