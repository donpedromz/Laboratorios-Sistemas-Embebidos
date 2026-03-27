import os
import hashlib
import hmac
from dataclasses import dataclass

from supabase import Client, create_client


@dataclass
class SupabaseConnector:
	"""Cliente simple para telemetria IoT y autenticacion basica en Supabase."""
	client: Client
	table_name: str = "telemetria_iot"
	users_table: str = "users"
	logs_table: str = "logs_seguridad"
	hash_iterations: int = 100_000

	@classmethod
	def from_env(cls):
		url = os.getenv("SUPABASE_URL", "")
		key = os.getenv("SUPABASE_KEY", "")
		table = os.getenv("SUPABASE_TABLE", "telemetria_iot")
		users_table = os.getenv("SUPABASE_USERS_TABLE", "users")
		logs_table = os.getenv("SUPABASE_LOGS_TABLE", "logs_seguridad")
		hash_iterations = int(os.getenv("PASSWORD_HASH_ITERATIONS", "100000"))
		if not url or not key:
			raise ValueError("SUPABASE_URL y SUPABASE_KEY son obligatorias")
		return cls(
			client=create_client(url, key),
			table_name=table,
			users_table=users_table,
			logs_table=logs_table,
			hash_iterations=hash_iterations,
		)

	def insert_telemetry(self, device_id, temperatura, humedad, estado):
		"""Inserta una fila en la tabla telemetria_iot."""
		payload = {
			"device_id": device_id,
			"temperatura": float(temperatura),
			"humedad": float(humedad),
			"estado": estado,
		}
		try:
			self.client.table(self.table_name).insert(payload).execute()
			return True
		except Exception as exc:
			print(f"[Supabase] Error insertando telemetria: {exc}")
			return False

	def hash_password(self, plain_password):
		"""Genera hash PBKDF2 SHA-256 con salt aleatoria."""
		salt = os.urandom(16)
		hashed = hashlib.pbkdf2_hmac(
			"sha256",
			plain_password.encode("utf-8"),
			salt,
			self.hash_iterations,
		)
		return (
			f"pbkdf2_sha256${self.hash_iterations}"
			f"${salt.hex()}"
			f"${hashed.hex()}"
		)

	@staticmethod
	def verify_password(plain_password, stored_hash):
		"""Valida una contraseña en texto plano contra el hash almacenado."""
		try:
			algorithm, iterations, salt_hex, hash_hex = stored_hash.split("$", 3)
			if algorithm != "pbkdf2_sha256":
				return False
			expected = bytes.fromhex(hash_hex)
			computed = hashlib.pbkdf2_hmac(
				"sha256",
				plain_password.encode("utf-8"),
				bytes.fromhex(salt_hex),
				int(iterations),
			)
			return hmac.compare_digest(computed, expected)
		except (ValueError, TypeError):
			return False

	def create_user(self, email, plain_password):
		"""Crea un usuario en la tabla users con password hasheada."""
		email = email.strip().lower()
		if not email or not plain_password:
			return False, "Email y password son obligatorios"

		payload = {
			"email": email,
			"password": self.hash_password(plain_password),
		}

		try:
			self.client.table(self.users_table).insert(payload).execute()
			return True, "Usuario creado correctamente"
		except Exception as exc:
			return False, f"Error creando usuario: {exc}"

	def get_user_by_email(self, email):
		"""Obtiene un usuario por email o None si no existe."""
		email = email.strip().lower()
		try:
			result = (
				self.client.table(self.users_table)
				.select("id,email,password,created_at")
				.eq("email", email)
				.limit(1)
				.execute()
			)
			data = result.data or []
			return data[0] if data else None
		except Exception as exc:
			print(f"[Supabase] Error obteniendo usuario: {exc}")
			return None

	def simulate_login(self, email, plain_password):
		"""Simula login validando email y password hasheada."""
		user = self.get_user_by_email(email)
		if not user:
			return False, "Usuario no encontrado"

		stored_hash = user.get("password", "")
		if not self.verify_password(plain_password, stored_hash):
			return False, "Credenciales invalidas"

		return True, f"Login correcto para {user.get('email')}"

	def simulate_login_from_credentials(self, usuario, contrasena):
		"""Alias para login recibiendo llaves de payload IoT en espanol."""
		return self.simulate_login(usuario, contrasena)

	def insert_security_log(self, evento, detalles, dispositivo_id=None):
		"""Inserta un log de auditoria en la tabla logs_seguridad."""
		payload = {
			"evento": evento,
			"detalles": detalles,
			"dispositivo_id": dispositivo_id,
		}
		try:
			self.client.table(self.logs_table).insert(payload).execute()
			return True
		except Exception as exc:
			print(f"[Supabase] Error insertando log de seguridad: {exc}")
			return False
