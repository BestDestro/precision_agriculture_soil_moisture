from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import traceback
from threading import Lock

import psycopg

DATA_FILE = "data.json"

# 0 = solo guardar JSON en archivo
# 1 = insertar en base de datos
MODE = 0

DB_HOST = os.getenv("DB_HOST", "127.0.0.1")
DB_PORT = int(os.getenv("DB_PORT", "5432"))
DB_NAME = os.getenv("DB_NAME", "esp32")
DB_USER = os.getenv("DB_USER", "espuser")
DB_PASS = os.getenv("DB_PASS", "esp_pass_123")

CONNINFO = (
    f"host={DB_HOST} port={DB_PORT} dbname={DB_NAME} "
    f"user={DB_USER} password={DB_PASS} connect_timeout=5"
)

file_lock = Lock()


def round_floats(obj, decimals=5):
    if isinstance(obj, float):
        return round(obj, decimals)
    elif isinstance(obj, dict):
        return {k: round_floats(v, decimals) for k, v in obj.items()}
    elif isinstance(obj, list):
        return [round_floats(x, decimals) for x in obj]
    else:
        return obj


def ensure_data_file():
    if not os.path.exists(DATA_FILE):
        with open(DATA_FILE, "w", encoding="utf-8") as f:
            json.dump([], f, indent=4, ensure_ascii=False)


def append_to_file(data: dict):
    ensure_data_file()
    with file_lock:
        with open(DATA_FILE, "r", encoding="utf-8") as f:
            json_data = json.load(f)
        json_data.append(data)
        with open(DATA_FILE, "w", encoding="utf-8") as f:
            json.dump(json_data, f, indent=4, ensure_ascii=False)


def insert_payload_to_db(payload: dict) -> int:
    device = payload.get("device") or payload.get("dispositivo", "unknown")
    num_barridos = int(payload.get("barridos_totales", 0))
    barridos = payload.get("barridos", [])

    if not isinstance(barridos, list):
        raise ValueError("Formato inválido: 'barridos' no es una lista")

    readings = [
        ("128mhz", "std_128mhz", 128_000_000),
        ("64mhz",  "std_64mhz",   64_000_000),
        ("32mhz",  "std_32mhz",   32_000_000),
        ("16mhz",  "std_16mhz",   16_000_000),
        ("8mhz",   "std_8mhz",     8_000_000),
        ("4mhz",   "std_4mhz",     4_000_000),
        ("2mhz",   "std_2mhz",     2_000_000),
        ("1mhz",   "std_1mhz",     1_000_000),
        ("500khz", "std_500khz",     500_000),
    ]

    muestras_por_frecuencia = payload.get("muestras_por_frecuencia", None)

    with psycopg.connect(CONNINFO) as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO execution (device, num_barridos, raw_payload)
                VALUES (%s, %s, %s::jsonb)
                RETURNING id
                """,
                (device, num_barridos, json.dumps(payload, ensure_ascii=False))
            )
            execution_id = cur.fetchone()[0]

            for b in barridos:
                sweep_index = int(b.get("id", 0)) or None
                temp = b.get("temp", None)
                hum = b.get("humedad", None)

                cur.execute(
                    """
                    INSERT INTO sweep (execution_id, sweep_index, temperature_c, humidity_pct)
                    VALUES (%s, %s, %s, %s)
                    RETURNING id
                    """,
                    (execution_id, sweep_index, temp, hum)
                )
                sweep_id = cur.fetchone()[0]

                for mean_key, std_key, freq_hz in readings:
                    mean_v = b.get(mean_key, None)
                    std_v = b.get(std_key, None)

                    if mean_v is None:
                        continue

                    cur.execute(
                        """
                        INSERT INTO sweep_reading (sweep_id, freq_hz, voltage_v, stddev_v, samples_count)
                        VALUES (%s, %s, %s, %s, %s)
                        """,
                        (
                            sweep_id,
                            freq_hz,
                            mean_v,
                            std_v,
                            int(muestras_por_frecuencia) if muestras_por_frecuencia is not None else None,
                        )
                    )

    return execution_id


class Handler(BaseHTTPRequestHandler):
    def _send(self, code: int, body: str):
        body_bytes = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body_bytes)))
        self.end_headers()
        self.wfile.write(body_bytes)

    def do_POST(self):
        if self.path != "/data":
            self._send(404, "Not Found")
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(content_length)
            payload = json.loads(body)

            payload = round_floats(payload, 5)
            resumen = {
                "dispositivo": payload.get("dispositivo") or payload.get("device"),
                "barridos_totales": payload.get("barridos_totales"),
                "muestras_por_frecuencia": payload.get("muestras_por_frecuencia"),
            }
            print("JSON recibido:", resumen)

            if MODE == 0:
                # Solo recoger JSON y guardarlo en archivo
                append_to_file(payload)
                self._send(200, "OK JSON recibido (modo 0: sin BBDD)")

            elif MODE == 1:
                # Insertar en base de datos
                append_to_file(payload)
                execution_id = insert_payload_to_db(payload)
                self._send(200, f"OK execution_id={execution_id}")

            else:
                self._send(500, "Modo inválido. Usa MODE = 0 o MODE = 1")

        except Exception as e:
            print("Error procesando POST /data:", repr(e))
            traceback.print_exc()
            self._send(500, f"Internal Server Error: {e}")


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", 8080), Handler)
    print("Servidor escuchando en puerto 8080")
    print(f"MODO ACTUAL: {MODE} ({'solo JSON' if MODE == 0 else 'BBDD'})")
    print(f"DB: {DB_HOST}:{DB_PORT} db={DB_NAME} user={DB_USER}")
    server.serve_forever()