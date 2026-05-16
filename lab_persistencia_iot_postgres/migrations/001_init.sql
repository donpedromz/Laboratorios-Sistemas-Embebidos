CREATE TABLE IF NOT EXISTS lecturas_sensores (
    id SERIAL PRIMARY KEY,
    fecha TIMESTAMPTZ DEFAULT NOW(),
    temperatura FLOAT,
    humedad FLOAT,
    proximidad_ir FLOAT,
    potenciometro FLOAT,
    device_id TEXT
);

CREATE TABLE IF NOT EXISTS logs_comandos (
    id SERIAL PRIMARY KEY,
    fecha TIMESTAMPTZ DEFAULT NOW(),
    device_id TEXT,
    comando_texto TEXT
);
