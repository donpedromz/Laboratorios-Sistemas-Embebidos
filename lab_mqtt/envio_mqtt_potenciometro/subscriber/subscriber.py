import paho.mqtt.client as mqtt
import json
import sys
from datetime import datetime

# ===========================================================================
# CONFIGURACION
# ===========================================================================

BROKER_HOST = ""
BROKER_PORT = 1883
USERNAME = "iot_device"
PASSWORD = "SecurePass123!"
CLIENT_ID = "python_potenciometro_subscriber"

# Topics para suscribirse (deben coincidir con los del ESP32)
TOPICS = [
    ("sensor/potenciometro/volume", 1),  # Datos crudos del potenciometro
    ("sensor/esp32/status", 1),          # Status del ESP32
    ("sensor/esp32/error", 1),           # Errores del ESP32
]

# ===========================================================================
# CONSTANTES DE PROCESAMIENTO
# ===========================================================================

ADC_MAX = 4095       # Resolucion del ADC (12 bits)
V_REF = 3.3          # Voltaje de referencia del ESP32

# ===========================================================================
# ALMACENAMIENTO DE DATOS
# ===========================================================================

reading_history = []
error_history = []
last_reading = {}
message_count = 0
error_count = 0

# ===========================================================================
# FUNCIONES DE PROCESAMIENTO
# ===========================================================================

def process_raw_data(raw_value):
    """Procesar el valor crudo del ADC y devolver voltage y porcentaje"""
    voltage = (raw_value / ADC_MAX) * V_REF
    percentage = (raw_value / ADC_MAX) * 100.0
    return voltage, percentage


def format_millis(ms):
    """Convertir milisegundos del ESP32 a formato legible"""
    seconds = ms // 1000
    minutes = seconds // 60
    hours = minutes // 60
    return f"{hours:02d}:{minutes % 60:02d}:{seconds % 60:02d}"


def classify_volume(percentage):
    """Clasificar el nivel de volumen"""
    if percentage == 0:
        return "SILENCIO"
    elif percentage < 25:
        return "BAJO"
    elif percentage < 50:
        return "MEDIO-BAJO"
    elif percentage < 75:
        return "MEDIO-ALTO"
    else:
        return "ALTO"


def volume_bar(percentage, width=30):
    """Generar barra visual de volumen"""
    filled = int((percentage / 100.0) * width)
    bar = "█" * filled + "░" * (width - filled)
    return f"[{bar}]"

# ===========================================================================
# CALLBACKS MQTT
# ===========================================================================

def on_connect(client, userdata, flags, reason_code, properties):
    """Callback cuando se conecta al broker"""
    if not reason_code.is_failure:
        print("=" * 70)
        print("  CONECTADO EXITOSAMENTE AL BROKER MQTT")
        print(f"  Host: {BROKER_HOST}:{BROKER_PORT}")
        print(f"  Client ID: {CLIENT_ID}")
        print("=" * 70)

        for topic, qos in TOPICS:
            client.subscribe(topic, qos=qos)
            print(f"  Suscrito a: {topic} (QoS {qos})")

        print("\n  Esperando datos del potenciometro desde ESP32...\n")
    else:
        print(f"Error de conexion: {reason_code}")
        sys.exit(1)


def on_message(client, userdata, msg):
    """Callback cuando se recibe un mensaje"""
    global message_count, last_reading, error_count

    message_count += 1
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    try:
        payload_str = msg.payload.decode("utf-8")
    except UnicodeDecodeError:
        print(f"Error decodificando mensaje en {msg.topic}")
        return

    topic = msg.topic

    # STATUS DEL ESP32
    if topic == "sensor/esp32/status":
        print(f"\n  [{timestamp}] ESP32 Status: {payload_str}")
        return

    # ERRORES DEL ESP32
    if topic == "sensor/esp32/error":
        error_count += 1
        try:
            data = json.loads(payload_str)
            error_type = data.get("error_type", "UNKNOWN")
            error_msg = data.get("message", payload_str)
            esp_millis = data.get("esp_millis", 0)
            free_heap = data.get("free_heap", 0)

            error_entry = {
                "error_type": error_type,
                "message": error_msg,
                "esp_millis": esp_millis,
                "free_heap": free_heap,
                "received_at": timestamp,
            }
            error_history.append(error_entry)

            print("\n" + "!" * 70)
            print(f"  [{timestamp}] ERROR DEL ESP32")
            print("!" * 70)
            print(f"  Tipo:            {error_type}")
            print(f"  Mensaje:         {error_msg}")
            print(f"  Uptime ESP32:    {format_millis(esp_millis)}")
            print(f"  Heap libre:      {free_heap} bytes")
            print(f"  Total errores:   {error_count}")
            print("!" * 70)
        except json.JSONDecodeError:
            print(f"\n  [{timestamp}] ESP32 Error (raw): {payload_str}")
        return

    # DATOS DEL POTENCIOMETRO (JSON crudo)
    if topic == "sensor/potenciometro/volume":
        try:
            data = json.loads(payload_str)

            # Extraer datos crudos enviados por el ESP32
            reading = data.get("reading", 0)
            raw = data.get("raw", 0)
            esp_millis = data.get("esp_millis", 0)

            # === PROCESAMIENTO EN EL SUBSCRIBER ===
            voltage, percentage = process_raw_data(raw)
            level = classify_volume(percentage)
            bar = volume_bar(percentage)

            # Guardar en historial
            processed = {
                "reading": reading,
                "raw": raw,
                "voltage": round(voltage, 2),
                "percentage": round(percentage, 1),
                "level": level,
                "esp_millis": esp_millis,
                "received_at": timestamp,
            }
            last_reading = processed
            reading_history.append(processed)

            # Mostrar datos procesados
            print("=" * 70)
            print(f"  [{timestamp}] LECTURA #{reading}")
            print("-" * 70)
            print(f"  ADC Crudo (ESP32):   {raw} / {ADC_MAX}")
            print(f"  Voltaje (calculado): {voltage:.2f} V")
            print(f"  Volumen:             {percentage:.1f}%")
            print(f"  Nivel:               {level}")
            print(f"  Volumen visual:      {bar}")
            print(f"  Uptime ESP32:        {format_millis(esp_millis)}")
            print(f"  Total mensajes:      {message_count}")
            print("=" * 70)

        except json.JSONDecodeError as e:
            print(f"Error parseando JSON: {e}")
            print(f"Payload: {payload_str[:100]}...")


def on_disconnect(client, userdata, flags, reason_code, properties):
    """Callback cuando se desconecta"""
    if reason_code.is_failure:
        print(f"\nDesconexion inesperada: {reason_code}")
        print("Intentando reconectar...")

# ===========================================================================
# RESUMEN FINAL
# ===========================================================================

def mostrar_resumen_final():
    """Mostrar resumen estadistico de las lecturas"""
    if not reading_history:
        print("\n  No hay datos para mostrar")
        return

    raws = [r["raw"] for r in reading_history]
    voltages = [r["voltage"] for r in reading_history]
    percentages = [r["percentage"] for r in reading_history]

    print("\n")
    print("=" * 70)
    print("  RESUMEN FINAL - SENSOR POTENCIOMETRO")
    print("=" * 70)
    print(f"  Total lecturas recibidas:    {len(reading_history)}")
    print()
    print(f"  ADC Raw   - Min: {min(raws):>4d}  |  Max: {max(raws):>4d}  |  Prom: {sum(raws)/len(raws):.0f}")
    print(f"  Voltaje   - Min: {min(voltages):>4.2f} V  |  Max: {max(voltages):>4.2f} V |  Prom: {sum(voltages)/len(voltages):.2f} V")
    print(f"  Volumen   - Min: {min(percentages):>5.1f}%  |  Max: {max(percentages):>5.1f}%  |  Prom: {sum(percentages)/len(percentages):.1f}%")
    print()

    # Distribucion por niveles
    levels = {}
    for r in reading_history:
        lvl = r["level"]
        levels[lvl] = levels.get(lvl, 0) + 1

    print("  Distribucion por nivel:")
    for lvl in ("SILENCIO", "BAJO", "MEDIO-BAJO", "MEDIO-ALTO", "ALTO"):
        count = levels.get(lvl, 0)
        pct = (count / len(reading_history)) * 100
        print(f"    {lvl:<12s}  {count:>4d} lecturas  ({pct:.1f}%)")

    # Resumen de errores
    if error_history:
        print()
        print("  ERRORES REGISTRADOS:")
        print("-" * 70)
        error_types = {}
        for e in error_history:
            et = e["error_type"]
            error_types[et] = error_types.get(et, 0) + 1
        for et, cnt in sorted(error_types.items()):
            print(f"    {et:<25s}  {cnt:>3d} veces")
        print(f"\n  Total errores: {len(error_history)}")
    else:
        print("\n  Sin errores registrados")

    print("=" * 70)

# ===========================================================================
# MAIN
# ===========================================================================

def main():
    print("=" * 70)
    print("  Python MQTT Subscriber - Potenciometro (Volumen)")
    print("=" * 70)
    print()

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=CLIENT_ID,
        clean_session=True,
    )
    client.username_pw_set(USERNAME, PASSWORD)

    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect

    print(f"  Conectando a {BROKER_HOST}:{BROKER_PORT}...")
    try:
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    except Exception as e:
        print(f"  Error al conectar: {e}")
        print("\n  Verifica que:")
        print("    1. RabbitMQ este corriendo")
        print("    2. Plugin MQTT este habilitado")
        print("    3. Usuario 'iot_device' tenga permisos")
        print("    4. La IP del broker sea correcta")
        sys.exit(1)

    print("\n  Presiona Ctrl+C para detener y ver resumen\n")

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n\n  Deteniendo subscriber...")
        mostrar_resumen_final()
        client.disconnect()
        print(f"\n  Total mensajes recibidos: {message_count}")
        print("  Desconectado correctamente")


if __name__ == "__main__":
    main()
