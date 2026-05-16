# Laboratorio Persistencia IoT con PostgreSQL

Proyecto de laboratorio IoT completo: ESP32 con lectura de sensores (DHT11, IR, potenciometro), envio bulk via MQTT a RabbitMQ, procesamiento con Node-RED y persistencia en PostgreSQL.

## Arquitectura

```
+--------+     MQTT      +----------+     SQL      +------------+
|  ESP32 |  ---------->  | RabbitMQ |  ----------> |  Node-RED  |
+--------+               +----------+              +------------+
                                                          |
                                                          v
                                                    +------------+
                                                    | PostgreSQL |
                                                    +------------+
```

- **ESP32**: Dual-core. Core 1 lee sensores cada 1s y acumula 5 lecturas. Core 0 envia el array bulk por MQTT cada 5s.
- **RabbitMQ**: Broker MQTT local (plugin MQTT habilitado en puerto 1883).
- **Node-RED**: Suscripcion a topics MQTT, procesamiento del bulk load y escritura a PostgreSQL.
- **PostgreSQL**: Base de datos relacional con tablas para lecturas de sensores, logs de comandos y registros agregados.

## Estructura del Proyecto

```
.
├── docker-compose.yml          # Solo PostgreSQL (RabbitMQ local)
├── package.json                # Scripts para correr Node-RED y DB
├── platformio.ini              # Configuracion PlatformIO
├── migrations/
│   └── 001_init.sql            # Migraciones de base de datos (auto-ejecutadas por PostgreSQL)
├── nodered/
│   ├── settings.js             # Configuracion Node-RED
│   ├── package.json            # Dependencias Node-RED
│   └── flows.json              # Flujos (creados por ti)
├── include/
│   ├── config.h                # Constantes del firmware (completar placeholders)
│   ├── types.h                 # Estructuras de datos
│   ├── globals.h               # Variables globales compartidas
│   ├── sensors.h               # Modulo de lectura de sensores
│   ├── network.h               # Modulo WiFi/MQTT
│   └── bulk_storage.h          # Serializacion JSON bulk
└── src/
    ├── main.cpp                # Punto de entrada y tareas FreeRTOS
    ├── sensors.cpp             # Implementacion lectura DHT11/IR/POT
    ├── network.cpp             # Implementacion conexion WiFi/MQTT
    └── bulk_storage.cpp        # Implementacion serializacion
```

## Requisitos Previos

- [PlatformIO](https://platformio.org/) (para compilar y subir el firmware ESP32)
- [RabbitMQ](https://www.rabbitmq.com/) instalado localmente con **plugin MQTT habilitado**
- [Node.js](https://nodejs.org/) >= 18
- [pnpm](https://pnpm.io/) >= 10
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) (solo para PostgreSQL)

## Instalacion y Ejecucion

### 1. PostgreSQL (Docker) con Migraciones Automaticas

```bash
pnpm db:up
```

O directamente con Docker:

```bash
docker compose up -d postgres
```

PostgreSQL queda disponible en `localhost:5433`:
- Base de datos: `iot_db`
- Usuario: `iot_user`
- Password: `iot_password`

**Migraciones**: La carpeta `migrations/` se monta en `/docker-entrypoint-initdb.d/` dentro del contenedor. PostgreSQL ejecuta automaticamente todos los archivos `.sql` al iniciar por primera vez, creando las tablas con `CREATE TABLE IF NOT EXISTS`. Si necesitas agregar nuevas tablas, crea un nuevo archivo numerado en `migrations/` (ej. `002_nueva_tabla.sql`) y reinicia el contenedor.

### 2. RabbitMQ (Local)

Asegurate de tener RabbitMQ corriendo localmente con el plugin MQTT activado en el puerto `1883`.

El firmware usa las credenciales por defecto de RabbitMQ para MQTT:
- Usuario: `guest`
- Password: `guest`

Si cambiaste las credenciales, actualiza `include/config.h` antes de compilar.

### 3. Node-RED (Local)

Instalar dependencias (desde la raiz del proyecto):

```bash
pnpm install
```

Correr Node-RED:

```bash
pnpm nodered
```

Node-RED queda disponible en: http://127.0.0.1:1880

El directorio de usuario de Node-RED es `./nodered/`, por lo que flows, settings y credenciales se guardan dentro del proyecto.

### 4. Firmware ESP32

1. Abre el proyecto en VS Code con la extension de PlatformIO.
2. Completa los placeholders en `include/config.h`:
   - `WIFI_SSID` y `WIFI_PASSWORD`
   - `MQTT_BROKER_HOST` (IP de tu PC donde corre RabbitMQ)
3. Compila y sube al ESP32DOIT-DevKit-V1.

**Consola Serial activa**: `platformio.ini` tiene configurado `monitor_speed = 115200` y `monitor_filters = esp32_exception_decoder`. Abre el monitor serial con:
```bash
pio device monitor
```
Desde la consola serial puedes escribir mensajes de texto. El ESP32 los acumula en un buffer configurable (`BULK_MSG_SIZE = 3` por defecto) y los envia en bulk al topic `logs/mensajes_sistema` via MQTT.

El monitor serial muestra:
- Lecturas de sensores (cada 1s).
- Publicacion del bulk de sensores (cada 5s).
- Mensajes recibidos de usuario via MQTT (`comandos/esp32`).
- Mensajes de sistema enviados en bulk (desde la consola serial).

## Esquema de Base de Datos

### Tabla: `lecturas_sensores`
Almacena cada lectura desagregada del bulk enviado por el ESP32.

| Columna       | Tipo         | Descripcion                          |
|---------------|--------------|--------------------------------------|
| id            | SERIAL PK    | ID autoincremental                   |
| fecha         | TIMESTAMPTZ  | Fecha/hora de insercion              |
| temperatura   | FLOAT        | Temperatura DHT11 (C)                |
| humedad       | FLOAT        | Humedad DHT11 (%)                    |
| proximidad_ir | FLOAT        | Valor analogico sensor IR (0-4095)   |
| potenciometro | FLOAT        | Valor analogico potenciometro        |
| device_id     | TEXT         | Identificador del dispositivo        |

### Tabla: `logs_comandos`
Registra los comandos enviados por los usuarios al ESP32 via MQTT.

| Columna       | Tipo         | Descripcion                          |
|---------------|--------------|--------------------------------------|
| id            | SERIAL PK    | ID autoincremental                   |
| fecha         | TIMESTAMPTZ  | Fecha/hora de insercion              |
| device_id     | TEXT         | Identificador del dispositivo        |
| comando_texto | TEXT         | Texto del comando recibido           |

## Nodos a Configurar en Node-RED

A continuacion se describe el flujo completo que debes armar en Node-RED usando el nodo **`node-red-contrib-postgresql`**.

### Flujo 1: Procesamiento Bulk Load (Sensores)

Estructura del flujo:
```
[mqtt in] --> [json] --> [function] --> [postgresql]
                                        |
                                  [catch] --> [debug error]
```

#### 1. Nodo `mqtt in`
- **Server**: Broker RabbitMQ local (localhost:1883)
- **Topic**: `sensores/bulk`
- **QoS**: 0
- **Output**: String JSON (no olvides conectarlo al nodo JSON a continuacion)

#### 2. Nodo `json` (JSON Parser)
- Convierte el payload string del `mqtt in` a un **objeto JavaScript**.
- Deja la configuracion por defecto (`msg.payload`).
- Salida esperada: un array con 5 objetos `SensorReading`.

#### 3. Nodo `function` (Preparacion de datos / Bulk Loading)
Este nodo recibe el array JSON parseado y genera la query SQL dinamica para insertar las 5 lecturas acumuladas del ESP32.

Mapeo directo a la tabla `lecturas_sensores`:
- `temperatura` = temperatura (DHT11)
- `humedad` = humedad (DHT11)
- `proximidad_ir` = sensor IR (pin 32)
- `potenciometro` = potenciometro (pin 31)
- `device_id` = device_id del JSON

**Opcion A: Dynamic SQL via `msg.query`**

```javascript
const bulk = msg.payload;
let values = [];

for (let r of bulk) {
    values.push(`(${r.temperatura}, ${r.humedad}, ${r.ir_proximidad}, ${r.potenciometro}, '${r.device_id}')`);
}

msg.query = `INSERT INTO lecturas_sensores (temperatura, humedad, proximidad_ir, potenciometro, device_id) VALUES ${values.join(", ")}`;
return msg;
```

**Opcion B: Parameterized Query (Recomendada - mas segura contra SQL injection)**

Genera un array plano `msg.params` con todos los valores y construye los placeholders `$1`, `$2`, etc. dinamicamente:

```javascript
const bulk = msg.payload;
msg.params = [];
let placeholders = [];

for (let r of bulk) {
    let base = msg.params.length;
    msg.params.push(r.temperatura, r.humedad, r.ir_proximidad, r.potenciometro, r.device_id);
    placeholders.push(`($${base+1}, $${base+2}, $${base+3}, $${base+4}, $${base+5})`);
}

msg.query = `INSERT INTO lecturas_sensores (temperatura, humedad, proximidad_ir, potenciometro, device_id) VALUES ${placeholders.join(", ")}`;
return msg;
```

#### 4. Nodo `postgresql` (node-red-contrib-postgresql)
Arrastra el nodo **`postgresql`** de la paleta (categoria storage).
- **Configuracion de conexion** (doble click > Add new postgresql config):
  - Host: `localhost`
  - Port: `5433`
  - Database: `iot_db`
  - User: `iot_user`
  - Password: `iot_password`
- **Query**: Deja vacio el campo de query en el nodo; se tomara automaticamente de `msg.query`.
- Conecta la salida del nodo `function` a este nodo.

> **Nota**: Si usas la **Opcion B** (parameterized), asegurate de que el campo **Query** del nodo postgresql este vacio. El nodo ejecutara la query de `msg.query` con los parametros de `msg.params`.

#### 5. Nodo `catch` (Error Handling)
Es **indispensable** para evitar fugas de conexion o bloqueos del sistema si la base de datos falla.

- Arrastra el nodo **`catch`** de la paleta (categoria input).
- Configuracion:
  - **Scope**: `all nodes` (captura errores de cualquier nodo del flujo)
  - **Property**: `msg.payload`
- Conecta la salida del nodo `catch` a un nodo **`debug`** o **`function`** para loguear el error:
```javascript
node.warn("Error en base de datos: " + msg.error.message);
return msg;
```

> El nodo `catch` garantiza que, si el nodo `postgresql` falla, la conexion no queda colgada y el flujo puede continuar recibiendo nuevos mensajes.

---

### Flujo 2: Mensajes de Sistema (Consola Serial)

Estructura del flujo:
```
[mqtt in] --> [json] --> [function] --> [postgresql]
                                         |
                                   [catch] --> [debug error]
```

#### 1. Nodo `mqtt in`
- **Server**: Broker RabbitMQ local
- **Topic**: `logs/mensajes_sistema`
- **QoS**: 0

#### 2. Nodo `json`
- Convierte el payload string a objeto JavaScript.

#### 3. Nodo `function` (Preparacion INSERT)

**Opcion A: Dynamic SQL via `msg.query`**
```javascript
const msgs = msg.payload;
let values = [];
for (let m of msgs) {
    values.push(`('${m.device_id}', '${m.msg}', to_timestamp(${m.timestamp} / 1000.0))`);
}
msg.query = `INSERT INTO logs_comandos (device_id, comando_texto, fecha) VALUES ${values.join(", ")}`;
return msg;
```

**Opcion B: Parameterized Query**
```javascript
const msgs = msg.payload;
msg.params = [];
let placeholders = [];

for (let m of msgs) {
    let base = msg.params.length;
    msg.params.push(m.device_id, m.msg, new Date(m.timestamp).toISOString());
    placeholders.push(`($${base+1}, $${base+2}, $${base+3})`);
}

msg.query = `INSERT INTO logs_comandos (device_id, comando_texto, fecha) VALUES ${placeholders.join(", ")}`;
return msg;
```

#### 4. Nodo `postgresql`
- Usa la **misma configuracion de conexion** que en el Flujo 1.
- Deja el campo **Query** vacio; se tomara de `msg.query`.

#### 5. Nodo `catch`
- Captura errores del nodo `postgresql` para liberar conexiones.

### Flujo 3: Monitoreo (Opcional)

Puedes agregar un nodo `debug` despues del nodo `json` del topic `sensores/bulk` para visualizar el array JSON parseado en el sidebar de Node-RED sin afectar el flujo principal.

## Formato del Payload Bulk (ESP32 -> MQTT)

El ESP32 publica un JSON array con exactamente 5 objetos en `sensores/bulk`:

```json
[
  {
    "device_id": "esp32_devkit_001",
    "idx": 0,
    "temperatura": 24.5,
    "humedad": 60.0,
    "ir_proximidad": 1024.0,
    "potenciometro": 2048.0,
    "timestamp_ms": 12345
  },
  ...
]
```

Y un JSON array con los mensajes ingresados por consola serial en `logs/mensajes_sistema`:

```json
[
  {
    "device_id": "esp32_devkit_001",
    "msg": "hola mundo",
    "timestamp": 123456
  },
  ...
]
```

## Topics MQTT

| Topic                   | Tipo        | Descripcion                                         |
|-------------------------|-------------|-----------------------------------------------------|
| `sensores/bulk`         | Publicacion | Array JSON con 5 lecturas del ESP32                 |
| `logs/mensajes_sistema`| Publicacion | Array JSON con mensajes ingresados por consola serial |

## Scripts Disponibles (package.json)

| Script            | Descripcion                                      |
|-------------------|--------------------------------------------------|
| `pnpm nodered`    | Levanta Node-RED local con settings del proyecto |
| `pnpm db:up`      | Inicia el contenedor PostgreSQL                  |
| `pnpm db:down`    | Detiene el contenedor PostgreSQL                 |
| `pnpm db:logs`    | Muestra logs de PostgreSQL en tiempo real        |

## Notas

- El ESP32 usa FreeRTOS con dos tareas fijas en nucleos separados.
- El bulk load reduce drasticamente el overhead de red al enviar 5 lecturas en un solo mensaje MQTT.
- RabbitMQ debe tener el plugin `rabbitmq_mqtt` habilitado para que el ESP32 (PubSubClient) pueda conectarse.
- Para habilitar el plugin MQTT en RabbitMQ: `rabbitmq-plugins enable rabbitmq_mqtt`
