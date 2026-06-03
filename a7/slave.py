#!/usr/bin/env python3
"""
mqtt_terminal.py — MQTT Terminal Client for STM32MP1
Robust, non-blocking, no deadlocks.

Install:
    pip install paho-mqtt pyreadline3
    pip install paho-mqtt pyreadline3
Run:
    python mqtt_terminal.py
"""

import json
import ssl
import sys
import time
import threading
import queue
import os
import signal

import paho.mqtt.client as mqtt

# ─── CONFIG ───────────────────────────────────────────────────────────────────
MQTT_BROKER      = '3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud'
MQTT_PORT        = 8883
MQTT_USERNAME    = 'prasad'
MQTT_PASSWORD    = 'prasad#12$A'
TOPIC_INPUT      = 'data/terminal/input'
TOPIC_OUTPUT     = 'data/terminal/output'
BOARD_NAME       = 'stm32mp1'
RESPONSE_TIMEOUT = 12    # seconds to wait for board reply
KEEPALIVE        = 60
# ──────────────────────────────────────────────────────────────────────────────

# ─── Readline (cross-platform) ────────────────────────────────────────────────
try:
    import readline
except ImportError:
    try:
        import pyreadline3 as readline
    except ImportError:
        readline = None

# ─── ANSI ─────────────────────────────────────────────────────────────────────
class C:
    RESET  = '\033[0m'
    BOLD   = '\033[1m'
    DIM    = '\033[2m'
    RED    = '\033[91m'
    GREEN  = '\033[92m'
    YELLOW = '\033[93m'
    CYAN   = '\033[96m'
    WHITE  = '\033[97m'

    @staticmethod
    def rgb(r, g, b):
        return f'\033[38;2;{r};{g};{b}m'

if os.name == 'nt':
    os.system('color')
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleMode(
            ctypes.windll.kernel32.GetStdHandle(-11), 7)
    except Exception:
        pass
# ──────────────────────────────────────────────────────────────────────────────

# ─── Globals ──────────────────────────────────────────────────────────────────
# All printing goes through this queue → one dedicated printer thread
# This completely eliminates lock contention between MQTT callbacks and input()
print_q        = queue.Queue()

# Set when a board 'output' or 'error' message arrives
response_event = threading.Event()

connected      = threading.Event()
board_online   = threading.Event()
running        = True
# ──────────────────────────────────────────────────────────────────────────────

def enqueue(*args):
    """Thread-safe print: push a tuple onto the print queue."""
    print_q.put(args)


def prompt_str():
    return (f"{C.GREEN}{C.BOLD}root@{BOARD_NAME}{C.RESET}"
            f"{C.WHITE}:{C.RESET}"
            f"{C.CYAN}~{C.RESET}"
            f"{C.WHITE}# {C.RESET}")


def print_banner():
    os.system('cls' if os.name == 'nt' else 'clear')
    print(f"{C.rgb(0,255,180)}{C.BOLD}")
    print("  ╔══════════════════════════════════════════════════════╗")
    print("  ║         MQTT  REMOTE  TERMINAL  CLIENT               ║")
    print(f"  ║         Target : {BOARD_NAME:<35}║")
    print("  ╚══════════════════════════════════════════════════════╝")
    print(f"{C.RESET}")
    print(f"  {C.DIM}Broker : {MQTT_BROKER}:{MQTT_PORT}{C.RESET}")
    print(f"  {C.DIM}Commands: exit · clear · help{C.RESET}\n")


# ─── Printer thread ───────────────────────────────────────────────────────────
# Single thread owns all stdout writes → zero race conditions

def printer_thread():
    while True:
        try:
            item = print_q.get(timeout=1)
        except queue.Empty:
            continue

        if item is None:
            break

        kind = item[0]

        # Erase current line (clears any partial input the user typed)
        sys.stdout.write('\r\033[2K')

        if kind == 'raw':
            print(item[1])

        elif kind == 'info':
            print(f"  {C.GREEN}ℹ  {item[1]}{C.RESET}")

        elif kind == 'warn':
            print(f"  {C.YELLOW}⚠  {item[1]}{C.RESET}")

        elif kind == 'error':
            print(f"  {C.RED}✗  {item[1]}{C.RESET}")

        elif kind == 'online':
            print(f"\n  {C.GREEN}●  Board online  [{item[1]}]{C.RESET}\n")

        elif kind == 'offline':
            print(f"\n  {C.RED}●  Board offline  [{item[1]}]{C.RESET}\n")

        elif kind == 'blocked':
            print(f"  {C.RED}[BLOCKED]{C.RESET} {item[1]}\n")

        elif kind == 'output':
            ts, output = item[1], item[2]
            print(f"{C.DIM}── {ts} {'─'*30}{C.RESET}")
            if output and output.strip() not in ('', '(no output)'):
                for line in output.splitlines():
                    print(f"  {line}")
            else:
                print(f"  {C.DIM}(no output){C.RESET}")
            print()

        elif kind == 'timeout':
            print(f"  {C.YELLOW}⚠  No response from board (timeout {RESPONSE_TIMEOUT}s){C.RESET}\n")

        elif kind == 'prompt':
            # Reprint the prompt after output
            sys.stdout.write(prompt_str())
            sys.stdout.flush()
            continue   # skip the flush below

        sys.stdout.flush()


# ─── MQTT Callbacks ───────────────────────────────────────────────────────────

def on_connect(mqttc, userdata, flags, rc):
    msgs = {
        0: ('Connected to broker ✓', True),
        1: ('Bad protocol version', False),
        2: ('Client ID rejected', False),
        3: ('Broker unavailable', False),
        4: ('Bad username/password', False),
        5: ('Not authorized', False),
    }
    msg, ok = msgs.get(rc, (f'Unknown rc={rc}', False))
    if ok:
        mqttc.subscribe(TOPIC_OUTPUT)
        connected.set()
        enqueue('info', msg)
    else:
        enqueue('error', msg)


def on_disconnect(mqttc, userdata, rc):
    connected.clear()
    board_online.clear()
    response_event.set()   # unblock any waiting command
    if running:
        enqueue('warn', f'Disconnected (rc={rc}). Auto-reconnecting...')


def on_message(mqttc, userdata, msg):
    try:
        data    = json.loads(msg.payload.decode('utf-8'))
        mtype   = data.get('type', 'output')
        output  = data.get('output',  '')
        message = data.get('message', '')
        ts      = data.get('timestamp', '')

        if mtype == 'status':
            if 'online' in message.lower():
                board_online.set()
                enqueue('online', ts)
            else:
                board_online.clear()
                enqueue('offline', ts)
            # Status messages don't unblock command wait
            return

        elif mtype == 'error':
            enqueue('blocked', output)

        elif mtype == 'output':
            enqueue('output', ts, output)

    except Exception as e:
        enqueue('error', f'Parse error: {e}')

    finally:
        # Always unblock the waiting command (output or error)
        response_event.set()


def on_log(mqttc, userdata, level, buf):
    pass   # suppress mosquitto debug logs


# ─── Send command ─────────────────────────────────────────────────────────────

def send_command(mqttc, cmd):
    """Publish command and wait for board response."""
    response_event.clear()
    try:
        result = mqttc.publish(TOPIC_INPUT, json.dumps({"cmd": cmd}))
        result.wait_for_publish(timeout=5)
    except Exception as e:
        enqueue('error', f'Publish failed: {e}')
        return

    # Wait for response
    got = response_event.wait(timeout=RESPONSE_TIMEOUT)
    if not got:
        enqueue('timeout')


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    global running

    print_banner()

    # Start printer thread first
    pt = threading.Thread(target=printer_thread, daemon=True)
    pt.start()

    # MQTT setup
    mqttc = mqtt.Client(client_id="pc-terminal-client", protocol=mqtt.MQTTv311)
    mqttc.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    mqttc.tls_set(tls_version=ssl.PROTOCOL_TLS)
    mqttc.on_connect    = on_connect
    mqttc.on_disconnect = on_disconnect
    mqttc.on_message    = on_message
    mqttc.on_log        = on_log

    # Auto-reconnect settings
    mqttc.reconnect_delay_set(min_delay=1, max_delay=10)

    enqueue('raw', f"  {C.DIM}Connecting to broker...{C.RESET}")
    try:
        mqttc.connect(MQTT_BROKER, MQTT_PORT, keepalive=KEEPALIVE)
    except Exception as e:
        enqueue('error', f'Could not connect: {e}')
        time.sleep(0.5)
        sys.exit(1)

    mqttc.loop_start()

    # Wait for broker connection
    if not connected.wait(timeout=10):
        enqueue('error', 'Broker connection timed out.')
        time.sleep(0.5)
        mqttc.loop_stop()
        sys.exit(1)

    # Wait for board
    enqueue('raw', f"  {C.DIM}Waiting for board...{C.RESET}")
    board_online.wait(timeout=15)
    time.sleep(0.6)   # let online message print

    # ── REPL ──────────────────────────────────────────────────────────────────
    while True:
        try:
            # Show prompt
            sys.stdout.write(prompt_str())
            sys.stdout.flush()

            cmd = input('').strip()

        except KeyboardInterrupt:
            print()
            enqueue('warn', 'Use "exit" to quit.')
            continue
        except EOFError:
            cmd = 'exit'

        if not cmd:
            continue

        # ── Local commands ────────────────────────────────────────────────────
        if cmd.lower() in ('exit', 'quit', 'logout'):
            enqueue('raw', f"\n  {C.DIM}Disconnecting...{C.RESET}\n")
            break

        if cmd == 'clear':
            print_banner()
            continue

        if cmd == 'help':
            enqueue('raw', (
                f"\n  {C.CYAN}Local commands:{C.RESET}\n"
                f"  {C.YELLOW}clear{C.RESET}  — clear screen\n"
                f"  {C.YELLOW}exit{C.RESET}   — quit\n"
                f"  {C.YELLOW}help{C.RESET}   — this message\n\n"
                f"  All other commands are sent to the board.\n"
            ))
            continue

        # Add to readline history
        if readline:
            try:
                readline.add_history(cmd)
            except Exception:
                pass

        # ── Send to board and wait ────────────────────────────────────────────
        send_command(mqttc, cmd)

        # Small gap before reprinting prompt so output has time to render
        time.sleep(0.05)

    # ── Cleanup ───────────────────────────────────────────────────────────────
    running = False
    print_q.put(None)   # stop printer thread
    pt.join(timeout=2)
    mqttc.loop_stop()
    mqttc.disconnect()
    print(f"  {C.GREEN}Goodbye.{C.RESET}\n")


if __name__ == '__main__':
    # Handle Ctrl+C at OS level gracefully
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    main()