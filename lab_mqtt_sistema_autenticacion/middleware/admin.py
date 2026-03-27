import getpass
import sys

from dotenv import load_dotenv

try:
	from .supabase_connector import SupabaseConnector
except ImportError:
	from supabase_connector import SupabaseConnector


def ask_email():
	return input("Email: ").strip().lower()


def ask_password(label="Password: "):
	return getpass.getpass(label)


def registrar_usuario(supabase):
	print("\n--- Registro de nuevo usuario ---")
	email = ask_email()
	if not email:
		print("Email no puede estar vacio")
		return

	password = ask_password()
	confirm = ask_password("Confirmar password: ")

	if not password:
		print("Password no puede estar vacia")
		return
	if password != confirm:
		print("Las passwords no coinciden")
		return

	ok, message = supabase.create_user(email, password)
	print(message)


def iniciar_sesion(supabase):
	print("\n--- Inicio de sesion (simulado) ---")
	email = ask_email()
	if not email:
		print("Email no puede estar vacio")
		return

	password = ask_password()
	if not password:
		print("Password no puede estar vacia")
		return

	ok, message = supabase.simulate_login(email, password)
	print(message)


def menu_loop(supabase):
	while True:
		print("\n" + "=" * 52)
		print("  ADMIN AUTH - SCRIPT DE PRUEBA")
		print("=" * 52)
		print("1) Registrar nuevo usuario")
		print("2) Iniciar sesion")
		print("3) Salir")
		opcion = input("\nSelecciona una opcion: ").strip()

		if opcion == "1":
			registrar_usuario(supabase)
		elif opcion == "2":
			iniciar_sesion(supabase)
		elif opcion == "3":
			print("Saliendo...")
			break
		else:
			print("Opcion invalida")


def main():
	load_dotenv()

	try:
		supabase = SupabaseConnector.from_env()
	except ValueError as exc:
		print(f"Error de configuracion: {exc}")
		sys.exit(1)

	menu_loop(supabase)


if __name__ == "__main__":
	main()
