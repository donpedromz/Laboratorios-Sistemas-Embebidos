import json
import os
import sys
import threading
from datetime import datetime, timezone

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
CLIENT_ID = os.getenv("MQTT_CLIENT_ID", "python_auth_subscriber")
DEVICE_ID_DEFAULT = os.getenv("DEVICE_ID", "ESP32-TempHum")

TOPIC_LOGIN_ATTEMPT = os.getenv("MQTT_TOPIC_LOGIN_ATTEMPT", "auth/login/attempt")
TOPIC_COMMAND = os.getenv("MQTT_TOPIC_COMMAND", "auth/esp32/command")
TOPIC_STATUS = os.getenv("MQTT_TOPIC_STATUS", "sensor/esp32/status")
TOPIC_ERROR = os.getenv("MQTT_TOPIC_ERROR", "sensor/esp32/error")
TOPIC_AUTH_RESULT = os.getenv("MQTT_TOPIC_AUTH_RESULT", "auth/login/result")

TOPICS = [
    (TOPIC_LOGIN_ATTEMPT, 1),
    (TOPIC_STATUS, 1),
    (TOPIC_ERROR, 1),
]

SERVO_OPEN_DURATION_MS = int(os.getenv("SERVO_OPEN_DURATION_MS", "120000"))
SERVO_OPEN_MAX_MS = int(os.getenv("SERVO_OPEN_MAX_MS", str(30 * 60 * 1000)))
AUTO_RELOCK_SECONDS = int(os.getenv("AUTO_RELOCK_SECONDS", "5"))
ENABLE_AUTO_RELOCK = os.getenv("ENABLE_AUTO_RELOCK", "false").strip().lower() == "true"
MAX_FAILED_ATTEMPTS = int(os.getenv("MAX_FAILED_ATTEMPTS", "3"))
BLOCK_SECONDS = int(os.getenv("BLOCK_SECONDS", "20"))

# ===========================================================================
# ESTADO EN MEMORIA
# ===========================================================================

message_count = 0
error_count = 0
auth_success_count = 0
auth_fail_count = 0

failed_attempts = {}
blocked_until = {}
lock_state = {}
auto_relock_timers = {}

# ===========================================================================
# UTILIDADES
# ===========================================================================


def now_iso():
    return datetime.now(timezone.utc).isoformat()


def publish_command(client, action, device_id, **extra):
    payload = {
        "accion": action,
        "dispositivo_id": device_id,
        "emitido_en": now_iso(),
    }
    payload.update(extra)

    payload_str = json.dumps(payload, ensure_ascii=False)
    result = client.publish(TOPIC_COMMAND, payload_str, qos=1)
    published = result.rc == mqtt.MQTT_ERR_SUCCESS

    estado = "OK" if published else f"FALLO rc={result.rc}"
    print(f"  -> Comando '{action}' a {TOPIC_COMMAND}: {estado}")
    return published


def publish_auth_result(client, ok, usuario, device_id, detalle, **extra):
    payload = {
        "usuario": usuario,
        "dispositivo_id": device_id,
        "autenticado": ok,
        "detalle": detalle,
        "emitido_en": now_iso(),
    }
    payload.update(extra)
    client.publish(TOPIC_AUTH_RESULT, json.dumps(payload, ensure_ascii=False), qos=1)


def extract_credentials(data):
    usuario = (data.get("usuario") or data.get("email") or data.get("user") or "").strip().lower()
    contrasena = (
        data.get("contraseña")
        or data.get("contrasena")
        or data.get("password")
        or ""
    )
    device_id = (
        data.get("dispositivo_id")
        or data.get("device_id")
        or DEVICE_ID_DEFAULT
    )
    return usuario, contrasena, device_id


def write_security_log(supabase, evento, detalles, device_id):
    inserted = supabase.insert_security_log(
        evento=evento,
        detalles=detalles,
        dispositivo_id=device_id,
    )
    print(f"  -> Log auditoria ({evento}): {'OK' if inserted else 'FALLO'}")


def send_relock_command(client, supabase, device_id):
    lock_state[device_id] = False
    publish_command(client, "CERRAR_SEGURO", device_id)
    write_security_log(
        supabase,
        "BLOQUEO_SEGURIDAD",
        "Cierre automatico del cerrojo despues de acceso concedido",
        device_id,
    )


def schedule_auto_relock(client, supabase, device_id):
    timer = auto_relock_timers.get(device_id)
    if timer and timer.is_alive():
        timer.cancel()

    timer = threading.Timer(
        AUTO_RELOCK_SECONDS,
        send_relock_command,
        args=(client, supabase, device_id),
    )
    timer.daemon = True
    auto_relock_timers[device_id] = timer
    timer.start()


def get_servo_open_duration_ms():
    if SERVO_OPEN_DURATION_MS < 0:
        return 0
    return min(SERVO_OPEN_DURATION_MS, SERVO_OPEN_MAX_MS)


# ===========================================================================
# CALLBACKS MQTT
# ===========================================================================


def on_connect(client, userdata, flags, reason_code, properties):
    if not reason_code.is_failure:
        print("=" * 70)
        print("  CONECTADO EXITOSAMENTE AL BROKER MQTT")
        print(f"  Host: {BROKER_HOST}:{BROKER_PORT}")
        print(f"  Client ID: {CLIENT_ID}")
        print("=" * 70)

        for topic, qos in TOPICS:
            client.subscribe(topic, qos=qos)
            print(f"  Suscrito a: {topic} (QoS {qos})")

        print("\n  Esperando intentos de login desde ESP32...\n")
    else:
        print(f"Error de conexion: {reason_code}")
        sys.exit(1)


def on_disconnect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        print(f"\nDesconexion inesperada: {reason_code}")
        print("Intentando reconectar...")


def on_message(client, userdata, msg):
    global message_count, error_count, auth_success_count, auth_fail_count

    message_count += 1
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    try:
        payload_str = msg.payload.decode("utf-8")
    except UnicodeDecodeError:
        print(f"Error decodificando mensaje en {msg.topic}")
        return

    if msg.topic == TOPIC_STATUS:
        print(f"\n  [{timestamp}] ESP32 Status: {payload_str}")
        return

    if msg.topic == TOPIC_ERROR:
        error_count += 1
        print(f"\n  [{timestamp}] ESP32 Error: {payload_str}")
        return

    if msg.topic != TOPIC_LOGIN_ATTEMPT:
        print(f"\n  [{timestamp}] Topic ignorado: {msg.topic}")
        return

    supabase = userdata["supabase"]

    try:
        data = json.loads(payload_str)
    except json.JSONDecodeError as exc:
        print(f"\n  [{timestamp}] Payload login invalido: {exc}")
        return

    usuario, contrasena, device_id = extract_credentials(data)
    if not usuario or not contrasena:
        auth_fail_count += 1
        detalle = "Payload sin usuario o contraseña"
        print(f"\n  [{timestamp}] Intento rechazado: {detalle}")
        write_security_log(supabase, "INTENTO_FALLIDO", detalle, device_id)
        publish_auth_result(
            client,
            False,
            usuario,
            device_id,
            detalle,
            bloqueado=False,
            intentos_fallidos=0,
            reintentos_maximos=MAX_FAILED_ATTEMPTS,
            reintentos_disponibles=MAX_FAILED_ATTEMPTS,
        )
        return

    now_epoch = datetime.now(timezone.utc).timestamp()
    blocked_until_epoch = blocked_until.get(device_id, 0)
    if now_epoch < blocked_until_epoch:
        auth_fail_count += 1
        remaining = int(blocked_until_epoch - now_epoch)
        detalle = f"Dispositivo bloqueado temporalmente ({remaining}s restantes)"
        print(f"\n  [{timestamp}] {detalle}")
        write_security_log(supabase, "BLOQUEO_SEGURIDAD", detalle, device_id)
        publish_auth_result(
            client,
            False,
            usuario,
            device_id,
            detalle,
            bloqueado=True,
            intentos_fallidos=failed_attempts.get(device_id, 0),
            reintentos_maximos=MAX_FAILED_ATTEMPTS,
            reintentos_disponibles=0,
            bloqueo_restante_s=max(remaining, 0),
        )
        return

    ok, login_message = supabase.simulate_login_from_credentials(usuario, contrasena)

    if ok:
        auth_success_count += 1
        failed_attempts[device_id] = 0

        lock_state[device_id] = True
        publish_command(
            client,
            "ABRIR_SEGURO",
            device_id,
            duracion_ms=get_servo_open_duration_ms(),
        )

        write_security_log(
            supabase,
            "ACCESO_CONCEDIDO",
            f"Usuario {usuario}: {login_message}",
            device_id,
        )
        if ENABLE_AUTO_RELOCK:
            schedule_auto_relock(client, supabase, device_id)

        print("\n" + "=" * 70)
        print(f"  [{timestamp}] ACCESO CONCEDIDO")
        print("-" * 70)
        print(f"  Usuario:            {usuario}")
        print(f"  Dispositivo:        {device_id}")
        print(f"  Mensaje login:      {login_message}")
        if ENABLE_AUTO_RELOCK:
            print(f"  Re-bloqueo en:      {AUTO_RELOCK_SECONDS}s")
        else:
            print("  Re-bloqueo en:      controlado por ESP32 (timeout/menu)")
        print(f"  Exitos acumulados:  {auth_success_count}")
        print("=" * 70)

        publish_auth_result(
            client,
            True,
            usuario,
            device_id,
            login_message,
            bloqueado=False,
            intentos_fallidos=0,
            reintentos_maximos=MAX_FAILED_ATTEMPTS,
            reintentos_disponibles=MAX_FAILED_ATTEMPTS,
            bloqueo_restante_s=0,
        )
        return

    auth_fail_count += 1
    current_fails = failed_attempts.get(device_id, 0) + 1
    failed_attempts[device_id] = current_fails

    write_security_log(
        supabase,
        "INTENTO_FALLIDO",
        f"Usuario {usuario}: {login_message}",
        device_id,
    )

    blocked_now = False
    if current_fails >= MAX_FAILED_ATTEMPTS:
        blocked_now = True
        blocked_until[device_id] = datetime.now(timezone.utc).timestamp() + BLOCK_SECONDS
        write_security_log(
            supabase,
            "BLOQUEO_SEGURIDAD",
            (
                f"Dispositivo bloqueado por {BLOCK_SECONDS}s tras "
                f"{current_fails} intentos fallidos"
            ),
            device_id,
        )

    print("\n" + "!" * 70)
    print(f"  [{timestamp}] ACCESO DENEGADO")
    print("-" * 70)
    print(f"  Usuario:            {usuario}")
    print(f"  Dispositivo:        {device_id}")
    print(f"  Motivo:             {login_message}")
    print(f"  Fallidos dispositivo:{current_fails}")
    print(f"  Fallidos globales:  {auth_fail_count}")
    print("!" * 70)

    publish_auth_result(
        client,
        False,
        usuario,
        device_id,
        login_message,
        bloqueado=blocked_now,
        intentos_fallidos=current_fails,
        reintentos_maximos=MAX_FAILED_ATTEMPTS,
        reintentos_disponibles=max(MAX_FAILED_ATTEMPTS - current_fails, 0),
        bloqueo_restante_s=BLOCK_SECONDS if blocked_now else 0,
    )


# ===========================================================================
# RESUMEN FINAL
# ===========================================================================


def mostrar_resumen_final():
    print("\n" + "=" * 70)
    print("  RESUMEN FINAL - AUTENTICACION MQTT")
    print("=" * 70)
    print(f"  Mensajes totales:         {message_count}")
    print(f"  Errores del ESP32:        {error_count}")
    print(f"  Accesos concedidos:       {auth_success_count}")
    print(f"  Intentos fallidos:        {auth_fail_count}")

    if failed_attempts:
        print("\n  Fallos por dispositivo:")
        for device_id, count in sorted(failed_attempts.items()):
            print(f"    - {device_id}: {count}")

    print("=" * 70)


# ===========================================================================
# MAIN
# ===========================================================================


def main():
    print("=" * 70)
    print("  Python MQTT Subscriber - Control de Acceso")
    print("=" * 70)
    print()

    try:
        supabase = SupabaseConnector.from_env()
    except ValueError as exc:
        print(f"  Error de configuracion Supabase: {exc}")
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
    except Exception as exc:
        print(f"  Error al conectar: {exc}")
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
        print("  Desconectado correctamente")


if __name__ == "__main__":
    main()
