import os
from dataclasses import dataclass

from supabase import Client, create_client

@dataclass
class SupabaseConnector:
	"""Cliente simple para persistir telemetria IoT en Supabase."""
	client: Client
	table_name: str = "telemetria_iot"
	@classmethod
	def from_env(cls):
		url = os.getenv("SUPABASE_URL", "")
		key = os.getenv("SUPABASE_KEY", "")
		table = os.getenv("SUPABASE_TABLE", "telemetria_iot")
		if not url or not key:
			raise ValueError("SUPABASE_URL y SUPABASE_KEY son obligatorias")
		return cls(client=create_client(url, key), table_name=table)
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
