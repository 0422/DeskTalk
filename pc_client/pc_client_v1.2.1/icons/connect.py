import inquirer
import platform
import serial
import serial.tools.list_ports
import asyncio
import threading
from bleak import BleakClient, BleakScanner

from common import *


class SerialClient(object):

    def __init__(self):
        self.port = ''
        self.ser = None
        self.connected = False

    def __unique_ports(self, ports):
        port_list = []
        for port in ports:
            if port not in port_list:
                port_list.append(port)
        return port_list

    def list_ports(self):
        ports = serial.tools.list_ports.comports()
        matching_ports = [port.device for port in ports if platform.system() == 'Windows' or "serial" in port.device.lower()]
        non_matching_ports = [port.device for port in ports if port.device not in matching_ports]
        return self.__unique_ports(matching_ports + non_matching_ports)

    def select_port(self):
        ports = self.list_ports()
        if len(ports) == 1:
            return ports[0]
        questions = [
            inquirer.List('port',
                          message="Select a port",
                          choices=ports,
                          carousel=True)
        ]
        answers = inquirer.prompt(questions)
        self.port = answers['port']
        return self.port

    def connect(self, port="", baud_rate=115200):
        if self.ser is not None:
            self.disconnect()

        try:
            port = port if port else self.port
            self.ser = serial.Serial(port, baud_rate, timeout=1)
            self.port = port
            logger.info(f"Connected to {port} at {baud_rate} baud rate.")
            self.connected = True
            return True
        except Exception as e:
            error(e, f"Connect to {port} Failed")
            self.connected = False
            return False

    def disconnect(self):
        ser = self.ser
        self.connected = False
        self.ser = None

        if ser and ser.is_open:
            try:
                ser.close()
                logger.info(f"Disconnected from {self.port}.")
            except Exception as e:
                error(e, "Failed to disconnect the serial port.")
        else:
            logger.info("Serial port is already closed or was not connected.")

    def read(self, port):
        while True:
            if port.in_waiting:
                data = port.read(port.in_waiting)
                result = data.decode('utf-8', errors='ignore')
                logger.debug("\nReceived:", result.strip('\n').strip())

    def send(self, msg):
        if not self.connected or self.ser is None or not self.ser.is_open:
            logger.error("Serial port is not connected.")
            self.disconnect()
            return False

        try:
            encode_msg = (msg + '\n').encode('utf-8')
            self.ser.write(encode_msg)
            self.ser.flush()
            logger.debug(f"Sent: {msg}")
            return True
        except Exception as e:
            error(e, "Serial port send message Failed!")
            self.disconnect()
            return False


class BaseBluetoothClient(object):
    def __init__(self, device_name="", service_uuid="", characteristic_uuid=""):
        self.device_name = device_name
        self.service_uuid = service_uuid
        self.characteristic_uuid = characteristic_uuid
        self.client = None
        self.connected = False

    async def list_devices(self):
        logger.info("Scanning devices...")
        device_list = []
        devices = await BleakScanner.discover()
        for device in devices:
            if device.name == self.device_name:
                device_list.append(device.address)
        return device_list

    async def connect(self, device_address):
        self.client = BleakClient(device_address)
        try:
            await self.client.connect()
            logger.info(f"Connected to {self.device_name} at {device_address}")
            self.connected = True
            return True
        except Exception as e:
            logger.error(f"Failed to connect to {self.device_name}: {e}")
            self.connected = False
            return False

    async def disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()
            self.connected = False
            logger.info(f"Disconnected from {self.device_name}")

    async def send(self, data):
        if self.client and self.client.is_connected:
            try:
                await self.client.write_gatt_char(self.characteristic_uuid, data.encode('utf-8'))
                logger.info(f"Sent to {self.device_name}: {data}")
            except Exception as e:
                logger.error(f"Failed to send data: {e}")
        else:
            logger.info("Not connected to any device.")


class BluetoothClient(BaseBluetoothClient):
    def __init__(self, device_name="Desk-Emoji",
                 service_uuid="4db9a22d-6db4-d9fe-4d93-38e350abdc3c",
                 characteristic_uuid="ff1cdaef-0105-e4fb-7be2-018500c2e927"):
        super().__init__(device_name, service_uuid, characteristic_uuid)
        self.loop_thread = threading.Thread(target=self._run_event_loop)
        self.loop_thread.daemon = True
        self.loop = asyncio.new_event_loop()
        self.loop_thread.start()

    def _run_event_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def list_devices(self):
        return asyncio.run_coroutine_threadsafe(super().list_devices(), self.loop).result()

    def connect(self, device_address):
        return asyncio.run_coroutine_threadsafe(super().connect(device_address), self.loop).result()

    def disconnect(self):
        asyncio.run_coroutine_threadsafe(super().disconnect(), self.loop).result()

    def send(self, data):
        asyncio.run_coroutine_threadsafe(super().send(data), self.loop).result()
