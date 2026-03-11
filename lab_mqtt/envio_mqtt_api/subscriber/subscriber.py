import paho.mqtt.client as mqtt
import json
import sys
from datetime import datetime

# ===========================================================================
# CONFIGURACION
# ===========================================================================

BROKER_HOST = "10.46.80.245"
BROKER_PORT = 1883
USERNAME = "iot_device"
PASSWORD = "SecurePass123!"
CLIENT_ID = "python_location_subscriber"

# Topics para suscribirse (deben coincidir con los del ESP32)
TOPICS = [
    ("location/manizales/current", 1),  # Datos de ubicacion (Nominatim)
    ("location/esp32/status", 1),       # Status del ESP32
    ("location/esp32/error", 1),        # Errores
]

# ===========================================================================
# ALMACENAMIENTO DE DATOS
# ===========================================================================

# Lista para almacenar historial de pasos
location_history = []

# Ultimo dato de ubicacion recibido
last_location = {}

# Contador de mensajes recibidos
message_count = 0

# ===========================================================================
# FUNCIONES DE FORMATO
# ===========================================================================

def format_distance(meters):
    """Formatear distancia en metros o km"""
    if meters >= 1000:
        return f"{meters / 1000:.2f} km"
    return f"{meters:.0f} m"

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
        print("CONECTADO EXITOSAMENTE AL BROKER MQTT")
        print(f"  Host: {BROKER_HOST}:{BROKER_PORT}")
        print(f"  Client ID: {CLIENT_ID}")
        print("=" * 70)

        # Suscribirse a topics
        for topic, qos in TOPICS:
            client.subscribe(topic, qos=qos)
            print(f"Suscrito a: {topic} (QoS {qos})")

        print("\nEsperando datos de ubicacion de Manizales desde ESP32...\n")
    else:
        print(f"Error de conexion: {reason_code}")
        sys.exit(1)

def on_message(client, userdata, msg):
    """Callback cuando se recibe un mensaje"""
    global message_count, last_location

    message_count += 1
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # Decodificar payload
    try:
        payload_str = msg.payload.decode('utf-8')
    except UnicodeDecodeError:
        print(f"Error decodificando mensaje en {msg.topic}")
        return

    topic = msg.topic

    # STATUS DEL ESP32
    if topic == "location/esp32/status":
        print(f"\n[{timestamp}] ESP32 Status: {payload_str}")
        return

    # ERRORES DEL ESP32
    if topic == "location/esp32/error":
        print(f"\n[{timestamp}] ESP32 Error: {payload_str}")
        return

    # DATOS DE UBICACION (JSON)
    if topic == "location/manizales/current":
        try:
            data = json.loads(payload_str)

            # Extraer informacion (esquema del ESP32)
            step         = data.get("step", 0)
            lat          = data.get("lat", "0")
            lon          = data.get("lon", "0")
            road         = data.get("road", "")
            neighbourhood = data.get("neighbourhood", "")
            suburb       = data.get("suburb", "")
            city         = data.get("city", "")
            place_type   = data.get("type", "")
            display_name = data.get("display_name", "")
            dist_origin  = data.get("dist_origin_m", 0)
            esp_millis   = data.get("esp_millis", 0)

            # Guardar en historial y ultimo dato
            last_location = {
                "step": step,
                "lat": lat,
                "lon": lon,
                "road": road,
                "neighbourhood": neighbourhood,
                "suburb": suburb,
                "city": city,
                "type": place_type,
                "display_name": display_name,
                "dist_origin_m": dist_origin,
                "esp_millis": esp_millis,
                "received_at": timestamp,
            }
            location_history.append(last_location)

            # Mostrar datos formateados
            print("\n" + "=" * 70)
            print(f"[{timestamp}] PASO #{step} - CAMINATA MANIZALES")
            print("=" * 70)
            print(f"  Coordenadas:         {lat}, {lon}")
            print(f"  Calle / Via:         {road}")
            print(f"  Barrio:              {neighbourhood}")
            print(f"  Comuna:              {suburb}")
            print(f"  Ciudad:              {city}")
            print(f"  Tipo de lugar:       {place_type}")
            print(f"  Dist. al origen:     {format_distance(dist_origin)}")
            print(f"  Uptime ESP32:        {format_millis(esp_millis)}")
            print("-" * 70)
            # Direccion corta a partir de campos estructurados
            partes = [p for p in (road, neighbourhood, suburb, city) if p]
            direccion_corta = ", ".join(partes) if partes else display_name
            print(f"  Ubicacion actual:    {direccion_corta}")
            print("=" * 70)
            print(f"  Total mensajes recibidos: {message_count}")
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
# FUNCIONES DE VISUALIZACION
# ===========================================================================

def mostrar_resumen_final():
    """Mostrar resumen final de la caminata"""
    if not location_history:
        print("\nNo hay datos de ubicacion para mostrar")
        return

    print("\n")
    print("=" * 70)
    print("RESUMEN FINAL - CAMINATA POR MANIZALES")
    print("=" * 70)
    print(f"  Total de pasos registrados:  {len(location_history)}")

    # Ultimo dato
    ultimo = location_history[-1]
    print(f"  Ultimo paso (#):             {ultimo.get('step', 'N/A')}")
    print(f"  Ultima posicion:             {ultimo.get('lat', 'N/A')}, {ultimo.get('lon', 'N/A')}")
    print(f"  Ultima calle:                {ultimo.get('road', 'N/A')}")
    print(f"  Ultimo barrio:               {ultimo.get('neighbourhood', 'N/A')}")
    print(f"  Ultima comuna:               {ultimo.get('suburb', 'N/A')}")
    print(f"  Ciudad:                      {ultimo.get('city', 'N/A')}")
    print(f"  Dist. al origen (ultimo):    {format_distance(ultimo.get('dist_origin_m', 0))}")

    # Distancia maxima alcanzada
    max_dist = max(loc.get("dist_origin_m", 0) for loc in location_history)
    print(f"  Dist. maxima al origen:      {format_distance(max_dist)}")

    # Calles unicas visitadas
    calles = {loc.get("road", "") for loc in location_history if loc.get("road")}
    barrios = {loc.get("neighbourhood", "") for loc in location_history if loc.get("neighbourhood")}
    print(f"  Calles unicas visitadas:     {len(calles)}")
    print(f"  Barrios unicos visitados:    {len(barrios)}")

    if calles:
        print("\n  Calles recorridas:")
        for calle in sorted(calles):
            print(f"    - {calle}")

    if barrios:
        print("\n  Barrios visitados:")
        for barrio in sorted(barrios):
            print(f"    - {barrio}")

    print("=" * 70)

# ===========================================================================
# MAIN
# ===========================================================================

def main():
    print("=" * 70)
    print("Python MQTT Subscriber - Caminata Manizales")
    print("Recibiendo datos de ubicacion del ESP32 via RabbitMQ")
    print("=" * 70)
    print()

    # Crear cliente MQTT (API v2 de paho-mqtt)
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=CLIENT_ID,
        clean_session=True,
    )
    client.username_pw_set(USERNAME, PASSWORD)

    # Configurar callbacks
    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect

    # Conectar al broker
    print(f"Conectando a {BROKER_HOST}:{BROKER_PORT}...")
    try:
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    except Exception as e:
        print(f"Error al conectar: {e}")
        print("\nVerifica que:")
        print("  1. RabbitMQ este corriendo")
        print("  2. Plugin MQTT este habilitado")
        print("  3. Usuario 'iot_device' tenga permisos")
        print("  4. La IP del broker sea correcta")
        sys.exit(1)

    # Loop infinito
    print("\nPresiona Ctrl+C para detener y ver resumen\n")

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n\nDeteniendo subscriber...")

        # Mostrar resumen final
        mostrar_resumen_final()

        # Desconectar
        client.disconnect()

        print("\nResumen final:")
        print(f"   Total de mensajes recibidos: {message_count}")
        print(f"   Pasos de caminata registrados: {len(location_history)}")
        print("\nDesconectado correctamente")

if __name__ == "__main__":
    main()