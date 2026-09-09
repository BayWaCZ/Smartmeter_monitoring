import time
import json
import struct
import traceback
import serial.tools.list_ports
import paho.mqtt.client as mqtt
from pymodbus.client import ModbusSerialClient

# --- 1. KONFIGURACE HIVEMQ CLOUD ---
MQTT_SERVER = "109e3418cc4e472ca8be85f97368e4da.s1.eu.hivemq.cloud"
MQTT_PORT = 8883
MQTT_USER = "BayWa_r1"  # Účet v HiveMQ Access Control
MQTT_PASS = "Distribuce2016"
MQTT_TOPIC = "baywa/mereni/rozvadec1"  # TOPIC_BASE + ID záložky v HTML

# --- 2. MODBUS NASTAVENÍ PRO GOODWE GM3000 ---
SLAVE_ID = 3
START_REG = 0x0061  # Počáteční registr (Phase A Voltage)
REG_COUNT = 10  # Čteme 10 registrů v kuse (0x0061 - 0x006A)


def vybrat_com_port():
    print("=" * 50)
    print("    GOODWE GM3000 MODBUS READER")
    print("=" * 50)
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("\n[Upozornění] Nebyly nalezeny žádné COM porty!")
        return input("Zadejte název portu ručně (např. COM3): ").strip()

    print("\nDostupné COM porty:")
    for idx, port in enumerate(ports):
        print(f"[{idx + 1}] {port.device} - {port.description}")

    while True:
        try:
            volba = int(input(f"\nVyberte číslo COM portu (1-{len(ports)}): "))
            if 1 <= volba <= len(ports):
                vybrany = ports[volba - 1].device
                print(f"-> Vybrán port: {vybrany}\n")
                return vybrany
        except ValueError:
            pass
        print("Neplatná volba, zkuste to znovu.")


def to_int16(val):
    """Převede 16bitové číslo bez znaménka na znaménkové (pro záporný výkon/přetoky)."""
    return struct.unpack('h', struct.pack('H', val))[0]


def main():
    # --- VÝBĚR PORTU A SPUŠTĚNÍ ---
    PORT_NAME = vybrat_com_port()

    # --- INICIALIZACE MQTT KLIENTA ---
    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
    mqtt_client.tls_set()
    mqtt_client.connect(MQTT_SERVER, MQTT_PORT)
    mqtt_client.loop_start()

    # --- INICIALIZACE MODBUS KLIENTA ---
    modbus_client = ModbusSerialClient(
        port=PORT_NAME,
        baudrate=9600,
        parity='N',
        stopbits=1,
        bytesize=8,
        timeout=0.5
    )

    # Otevření COM portu pouze JEDNOU před smyčkou (zabrání přetížení ovladače)
    if not modbus_client.connect():
        print(f"Nelze otevřít port {PORT_NAME}. Ověřte, že jej neblokuje jiná aplikace.")
        return

    print(f"Vyčítám data z GoodWe GM3000 (Slave ID: {SLAVE_ID})...\n")

    # --- HLAVNÍ SMYČKA ---
    while True:
        try:
            # Čtení holding registrů
            rr = modbus_client.read_holding_registers(address=START_REG, count=REG_COUNT, device_id=SLAVE_ID)

            if rr and not rr.isError():
                regs = rr.registers

                # Dekódování podle tabulky registrů GM3000:
                v1 = regs[0] / 10.0  # 0x0061: Phase A voltage
                v2 = regs[1] / 10.0  # 0x0062: Phase B voltage
                v3 = regs[2] / 10.0  # 0x0063: Phase C voltage

                i1 = regs[3] / 100.0  # 0x0064: Phase A current
                i2 = regs[4] / 100.0  # 0x0065: Phase B current
                i3 = regs[5] / 100.0  # 0x0066: Phase C current

                p1 = to_int16(regs[6])  # 0x0067: Phase A active power
                p2 = to_int16(regs[7])  # 0x0068: Phase B active power
                p3 = to_int16(regs[8])  # 0x0069: Phase C active power

                # Příprava JSONu pro webový dashboard
                payload = {
                    "p1": p1, "p2": p2, "p3": p3,
                    "v1": round(v1), "v2": round(v2), "v3": round(v3),
                    "i1": round(i1, 1), "i2": round(i2, 1), "i3": round(i3, 1)
                }

                mqtt_client.publish(MQTT_TOPIC, json.dumps(payload))
                timestamp = time.strftime('%H:%M:%S')
                print(f"[{timestamp}] Data odeslána na '{MQTT_TOPIC}': {payload}")
            else:
                timestamp = time.strftime('%H:%M:%S')
                print(f"[{timestamp}] Chyba čtení z Modbus sběrnice (elektroměr neodpovídá / vynechaný paket).")

        except Exception as e:
            timestamp = time.strftime('%H:%M:%S')
            print(f"[{timestamp}] Dočasná chyba komunikace: {e}")

        time.sleep(0.5)


# --- OŠETŘENÍ PÁDU APULIKACE V PROSTŘEDÍ .EXE ---
if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("\n" + "=" * 50)
        print("KRITICKÁ CHYBA PROGRAMU:")
        print("=" * 50)
        traceback.print_exc()
    finally:
        input("\nStiskněte Enter pro ukončení okna...")