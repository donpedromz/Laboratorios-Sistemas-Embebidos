import json
import os
import sys
from datetime import datetime

import paho.mqtt.client as mqtt
from dotenv import load_dotenv

try:
    from .supabase_connector import SupabaseConnector
except ImportError:
    from supabase_connector import SupabaseConnector

# ===========================================================================
# CONFIGURACION
# ===========================================================================

load_dotenv()

BROKER_HOST = os.getenv("MQTT_BROKER_HOST", "")
BROKER_PORT = int(os.getenv("MQTT_BROKER_PORT", "1883"))
USERNAME = os.getenv("MQTT_USERNAME", "iot_device")
PASSWORD = os.getenv("MQTT_PASSWORD", "SecurePass123!")
CLIENT_ID = os.getenv("MQTT_CLIENT_ID", "python_temperatura_humedad_subscriber")
DEVICE_ID_DEFAULT = os.getenv("DEVICE_ID", "ESP32-TempHum")

# Topics para suscribirse
TOPICS = [
    (os.getenv("MQTT_TOPIC_READING", "sensor/ambient/reading"), 1),
    (os.getenv("MQTT_TOPIC_STATUS", "sensor/esp32/status"), 1),
    (os.getenv("MQTT_TOPIC_ERROR", "sensor/esp32/error"), 1),
]

TOPIC_READING = TOPICS[0][0]
TOPIC_STATUS = TOPICS[1][0]
TOPIC_ERROR = TOPICS[2][0]

# ===========================================================================
# CONSTANTES DE PROCESAMIENTO
# ===========================================================================

TEMP_HIGH_ALERT_C = float(os.getenv("TEMP_HIGH_ALERT_C", "30"))
TEMP_LOW_ALERT_C = float(os.getenv("TEMP_LOW_ALERT_C", "15"))
HUM_HIGH_ALERT = float(os.getenv("HUM_HIGH_ALERT", "75"))
HUM_LOW_ALERT = float(os.getenv("HUM_LOW_ALERT", "30"))

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

def classify_environment(temperature_c, humidity):
    """Clasificar el estado ambiental usando umbrales configurables."""
    if temperature_c >= TEMP_HIGH_ALERT_C or humidity >= HUM_HIGH_ALERT:
        return "ALERTA_CALOR_HUMEDAD"
    if temperature_c <= TEMP_LOW_ALERT_C or humidity <= HUM_LOW_ALERT:
        return "ALERTA_FRIO_SEQUEDAD"
    return "NORMAL"


def format_millis(ms):
    """Convertir milisegundos del ESP32 a formato legible"""
    seconds = ms // 1000
    minutes = seconds // 60
    hours = minutes // 60
    return f"{hours:02d}:{minutes % 60:02d}:{seconds % 60:02d}"


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

        print("\n  Esperando lecturas de temperatura/humedad desde ESP32...\n")
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
    if topic == TOPIC_STATUS:
        print(f"\n  [{timestamp}] ESP32 Status: {payload_str}")
        return

    # ERRORES DEL ESP32
    if topic == TOPIC_ERROR:
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

    # DATOS DE TEMPERATURA/HUMEDAD
    if topic == TOPIC_READING:
        try:
            data = json.loads(payload_str)

            reading = int(data.get("reading", 0))
            temperature_c = float(data.get("temperature_c", 0.0))
            humidity = float(data.get("humidity", 0.0))
            esp_millis = int(data.get("esp_millis", 0))
            device_id = data.get("device_id", DEVICE_ID_DEFAULT)
            state = classify_environment(temperature_c, humidity)

            saved_ok = userdata["supabase"].insert_telemetry(
                device_id=device_id,
                temperatura=temperature_c,
                humedad=humidity,
                estado=state,
            )

            processed = {
                "reading": reading,
                "device_id": device_id,
                "temperature_c": round(temperature_c, 2),
                "humidity": round(humidity, 2),
                "estado": state,
                "saved_to_supabase": saved_ok,
                "esp_millis": esp_millis,
                "received_at": timestamp,
            }
            last_reading = processed
            reading_history.append(processed)

            print("=" * 70)
            print(f"  [{timestamp}] LECTURA #{reading}")
            print("-" * 70)
            print(f"  Device ID:           {device_id}")
            print(f"  Temperatura:         {temperature_c:.2f} C")
            print(f"  Humedad:             {humidity:.2f} %")
            print(f"  Estado:              {state}")
            print(f"  Guardado Supabase:   {'OK' if saved_ok else 'FALLO'}")
            print(f"  Uptime ESP32:        {format_millis(esp_millis)}")
            print(f"  Total mensajes:      {message_count}")
            print("=" * 70)

        except (json.JSONDecodeError, TypeError, ValueError) as e:
            print(f"Error procesando payload de lectura: {e}")
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

    temperatures = [r["temperature_c"] for r in reading_history]
    humidities = [r["humidity"] for r in reading_history]

    print("\n")
    print("=" * 70)
    print("  RESUMEN FINAL - SENSOR TEMP/HUMEDAD")
    print("=" * 70)
    print(f"  Total lecturas recibidas:    {len(reading_history)}")
    print()
    print(f"  Temp C    - Min: {min(temperatures):>5.2f}  |  Max: {max(temperatures):>5.2f}  |  Prom: {sum(temperatures)/len(temperatures):.2f}")
    print(f"  Humedad   - Min: {min(humidities):>5.2f}% |  Max: {max(humidities):>5.2f}% |  Prom: {sum(humidities)/len(humidities):.2f}%")
    print()

    # Distribucion por estados
    levels = {}
    for r in reading_history:
        lvl = r["estado"]
        levels[lvl] = levels.get(lvl, 0) + 1

    print("  Distribucion por estado:")
    for lvl in ("NORMAL", "ALERTA_CALOR_HUMEDAD", "ALERTA_FRIO_SEQUEDAD"):
        count = levels.get(lvl, 0)
        pct = (count / len(reading_history)) * 100
        print(f"    {lvl:<12s}  {count:>4d} lecturas  ({pct:.1f}%)")

    saved_count = sum(1 for r in reading_history if r.get("saved_to_supabase"))
    print(f"\n  Guardadas en Supabase: {saved_count}/{len(reading_history)}")

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
    print("  Python MQTT Subscriber - Temperatura/Humedad")
    print("=" * 70)
    print()

    try:
        supabase = SupabaseConnector.from_env()
    except ValueError as e:
        print(f"  Error de configuracion Supabase: {e}")
        print("  Configura SUPABASE_URL y SUPABASE_KEY en variables de entorno")
        sys.exit(1)

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=CLIENT_ID,
        clean_session=True,
        userdata={"supabase": supabase},
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
