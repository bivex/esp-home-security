# ESP Home Security — Схемы подключений

## 1. Hub Node (ESP32 DevKit V1) — Коридор

```
ESP32 DevKit         Компонент
────────────         ─────────
GPIO 13 ──────────── Keypad R1
GPIO 12 ──────────── Keypad R2
GPIO 14 ──────────── Keypad R3
GPIO 27 ──────────── Keypad R4
GPIO 26 ──────────── Keypad C1
GPIO 25 ──────────── Keypad C2
GPIO 33 ──────────── Keypad C3
GPIO 32 ──────────── Keypad C4

GPIO 15 ──[1kΩ]──┐  IRLZ44N MOSFET
                   ├── Gate
              GND ─┘── Source
                   └── Drain ──── Сирена (-)
                                  Сирена (+) ─── 12V
                                  12V GND ───── ESP32 GND

GPIO 21 (SDA) ───── OLED SDA
GPIO 22 (SCL) ───── OLED SCL
3.3V ─────────────── OLED VCC
GND ──────────────── OLED GND

GPIO 2 ───────────── Built-in LED (статус)
```

## 2. Sensor Node (ESP8266 D1 Mini) — каждая комната

```
Wemos D1 Mini        Компонент
─────────────        ─────────
GPIO 14 (D5) ─────── HC-SR501 OUT
3.3V ─────────────── HC-SR501 VCC
GND ──────────────── HC-SR501 GND

GPIO 12 (D6) ─────── Геркон MC-38 (один контакт)
GND ──────────────── Геркон MC-38 (второй контакт)
                     NOTE: INPUT_PULLUP, LOW = закрыто

A0 ─────[100kΩ]───── Battery +
   └──[100kΩ]── GND  (делитель 1:2 для 0-3.3V → 0-6.6V диапазон)

GPIO 2 ───────────── Built-in LED (active LOW)
```

### Настройка HC-SR501 (PIR)
- Перемычка: H (режим повторного срабатывания)
- Потенциометр времени: ~5 сек (минимум)
- Потенциометр чувствительности: средняя

## 3. Camera Node (ESP32-CAM AI-Thinker)

```
ESP32-CAM            Примечание
──────────           ─────────
GPIO 4  ──────────── Вспышка LED (НЕ используйте с SD картой!)
GPIO 33 ──────────── Статус LED (на плате)
                      Kamery пины захвачены камерой — НЕ трогайте

Питание:
  5V ──────────────── 5V pin (через стабилизатор на плате)
  GND ─────────────── GND

ПРОГРАММИРОВАНИЕ:
  GPIO 0 → GND  (для прошивки)
  TX → USB-TX
  RX → USB-RX
  После прошивки убрать GPIO 0 от GND
```

## 4. Питание

### Hub + Камеры (стационарное)
```
БП 5V 2A ──── USB кабель ──── ESP32 / ESP32-CAM
```

### Sensor Nodes (батарея 18650)
```
18650 (3.7V) ── TP4056 ── MT3608 (boost to 5V) ── D1 Mini 5V pin

   ┌──────────────────────────────────────────────┐
   │ 18650+  → TP4056 BAT+ → TP4056 OUT+ → MT3608 IN+ │
   │ 18650-  → TP4056 BAT- → TP4056 OUT- → MT3608 IN- │
   │ MT3608 OUT+ (5V) → D1 Mini 5V                │
   │ MT3608 OUT-      → D1 Mini GND               │
   └──────────────────────────────────────────────┘
```

## 5. Сервер (Raspberry Pi)

```
Raspberry Pi Zero W
  ├── Wi-Fi (точка доступа или клиент роутера)
  ├── Mosquitto MQTT broker (port 1883)
  ├── Python server.py (port 5000)
  └── Telegram Bot API

Установка:
  sudo apt install mosquitto mosquitto-clients
  sudo systemctl enable mosquitto
  pip install -r requirements.txt
  python server.py
```
