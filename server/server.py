"""
ESP Home Security — Server (MQTT bridge + Telegram bot + photo storage)

Run:
    pip install -r requirements.txt
    python server.py

Environment variables:
    MQTT_HOST      — MQTT broker address (default: localhost)
    MQTT_PORT      — MQTT broker port (default: 1883)
    TELEGRAM_TOKEN — Bot token from @BotFather
    TELEGRAM_CHAT  — Your Telegram user/chat ID
"""

import json
import logging
import os
import time
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt
from flask import Flask, request, jsonify

import config

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("security-server")

# ============ STATE ============
system_mode = "disarmed"
alarm_active = False
last_alerts: list[dict] = []

# ============ TELEGRAM (optional) ============
telegram_enabled = (
    config.TELEGRAM_TOKEN
    and config.TELEGRAM_TOKEN != "YOUR_BOT_TOKEN"
    and config.TELEGRAM_CHAT != 0
)

if telegram_enabled:
    import telegram
    _bot = telegram.Bot(token=config.TELEGRAM_TOKEN)

    async def _tg_send(text, photo_path=None):
        if photo_path and os.path.exists(photo_path):
            with open(photo_path, "rb") as f:
                await _bot.send_photo(chat_id=config.TELEGRAM_CHAT, photo=f, caption=text)
        else:
            await _bot.send_message(chat_id=config.TELEGRAM_CHAT, text=text)

    def send_telegram(text, photo_path=None):
        import asyncio
        asyncio.new_event_loop().run_until_complete(_tg_send(text, photo_path))
else:
    def send_telegram(text, photo_path=None):
        log.info("[Telegram disabled] %s", text)


# ============ PHOTO STORAGE ============
PHOTO_DIR = Path(config.PHOTO_DIR)
PHOTO_DIR.mkdir(exist_ok=True)


def save_photo(cam_id: str, data: bytes) -> str:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = PHOTO_DIR / f"{cam_id}_{ts}.jpg"
    path.write_bytes(data)
    log.info("Photo saved: %s (%d bytes)", path, len(data))
    return str(path)


# ============ MQTT HANDLERS ============
def on_connect(client, userdata, flags, rc, properties=None):
    log.info("MQTT connected, rc=%s", rc)
    client.subscribe("home/security/#")


def on_message(client, userdata, msg):
    global system_mode, alarm_active

    topic = msg.topic
    payload = msg.payload.decode("utf-8", errors="replace")
    log.info("MQTT: %s = %s", topic, payload[:100])

    if topic == "home/security/hub/mode":
        system_mode = payload
        send_telegram(f"Режим изменён: {payload}")

    elif topic == "home/security/hub/alarm":
        alarm_active = payload == "on"
        send_telegram("ТРЕВОГА!" if alarm_active else "Тревога снята")

    elif topic == "home/security/hub/alert":
        last_alerts.append({"time": datetime.now().isoformat(), "message": payload})
        if len(last_alerts) > 50:
            last_alerts.pop(0)
        send_telegram(payload)

    elif topic.endswith("/status") and payload == "offline":
        node = topic.split("/")[-2]
        send_telegram(f"Датчик {node} отключился!")

    elif topic.endswith("/photo") and payload == "captured":
        cam = topic.split("/")[-2]
        log.info("Camera %s captured photo", cam)


# ============ FLASK ============
app = Flask(__name__)


@app.route("/api/photo/<cam_id>", methods=["POST"])
def receive_photo(cam_id):
    data = request.get_data()
    path = save_photo(cam_id, data)
    if alarm_active:
        send_telegram(f"Снимок с {cam_id}", photo_path=path)
    return jsonify({"status": "ok", "path": path})


@app.route("/api/status", methods=["GET"])
def get_status():
    return jsonify({
        "mode": system_mode,
        "alarm": alarm_active,
        "alerts": last_alerts[-10:],
    })


@app.route("/api/command/<cmd>", methods=["POST"])
def send_command(cmd):
    if cmd not in ("arm_away", "arm_home", "disarm", "alarm_on", "alarm_off"):
        return jsonify({"error": "unknown command"}), 400
    mqtt_client.publish("home/security/hub/config", cmd)
    return jsonify({"status": "sent", "command": cmd})


# ============ MAIN ============
mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="security-server")
if config.MQTT_USER:
    mqtt_client.username_pw_set(config.MQTT_USER, config.MQTT_PASS)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message


def run_mqtt():
    while True:
        try:
            mqtt_client.connect(config.MQTT_HOST, config.MQTT_PORT, 60)
            mqtt_client.loop_forever()
        except Exception as e:
            log.error("MQTT connection failed: %s — retry in 5s", e)
            time.sleep(5)


if __name__ == "__main__":
    import threading

    log.info("Starting ESP Home Security Server...")
    log.info("MQTT: %s:%s", config.MQTT_HOST, config.MQTT_PORT)
    log.info("Telegram: %s", "enabled" if telegram_enabled else "disabled (set TELEGRAM_TOKEN env)")
    log.info("HTTP API: http://localhost:%d", config.HTTP_PORT)

    mqtt_thread = threading.Thread(target=run_mqtt, daemon=True)
    mqtt_thread.start()

    time.sleep(1)

    send_telegram("Система безопасности запущена")

    app.run(host=config.HTTP_HOST, port=config.HTTP_PORT, debug=False, use_reloader=False)
