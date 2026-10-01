#!/usr/bin/env python3
"""Хаб роя ESP32-устройств: один Telegram-бот для всех, связь с устройствами — MQTT.

Устройства публикуют:
  swarm/dev/<id>/status  "online"/"offline" (retained, offline — Last Will)
  swarm/dev/<id>/meta    {"id","name","type","fw","ip"}            (retained)
  swarm/dev/<id>/state   состояние устройства                       (retained)
  swarm/dev/<id>/event   {"kind","text","chat"?}
  swarm/dev/<id>/photo   строка JSON {"caption","kind","chat"?} + "\n" + JPEG
Хаб отправляет команды:
  swarm/dev/<id>/cmd | swarm/type/<type>/cmd | swarm/all/cmd
  {"cmd":"arm","arg":"","chat":"123"}

Зависимости: python3-paho-mqtt, python3-requests (пакеты Ubuntu/Debian).
Запуск: swarm_hub.py /etc/swarm-hub/config.json
"""

import json
import logging
import queue
import sys
import threading
import time

import paho.mqtt.client as mqtt
import requests

log = logging.getLogger("swarm-hub")

TYPE_ICONS = {"guard": "🛡", "gardener": "🌱", "thermostat": "🌡"}
TYPE_NAMES = {"guard": "охранник", "gardener": "садовник", "thermostat": "термостат"}
ALL_WORDS = {"all", "все", "всё"}


def casefold(text):
    return (text or "").strip().casefold()


class Telegram:
    """Тонкая обёртка над Bot API с повтором при сетевой ошибке."""

    def __init__(self, token, api_base):
        self.base = api_base.rstrip("/") + "/bot" + token + "/"
        self.session = requests.Session()

    def call(self, method, data=None, files=None, timeout=30):
        for attempt in range(2):
            try:
                if files:
                    response = self.session.post(self.base + method, data=data, files=files, timeout=timeout)
                else:
                    response = self.session.post(self.base + method, json=data or {}, timeout=timeout)
                payload = response.json()
                if not payload.get("ok"):
                    log.warning("Telegram %s: %s", method, payload.get("description"))
                return payload
            except (requests.RequestException, ValueError) as error:
                log.warning("Telegram %s: %s (попытка %d)", method, error, attempt + 1)
                time.sleep(3)
        return {"ok": False}


class Hub:
    def __init__(self, config):
        self.config = config
        self.chats = [str(c) for c in config["allowed_chats"]]
        self.offline_alert_seconds = int(config.get("offline_alert_seconds", 120))
        self.tg = Telegram(config["telegram_token"], config.get("telegram_api", "https://api.telegram.org"))
        self.devices = {}          # id -> {"meta":{}, "state":{}, "online":bool, "since":ts, "alerted":bool}
        self.lock = threading.Lock()
        self.outbox = queue.Queue()  # задания отправки в Telegram выполняются по очереди
        self.started = time.time()
        self.mqtt = self._make_mqtt()

    # ------------------------------------------------------------------ MQTT --
    def _make_mqtt(self):
        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id="swarm-hub")
        except AttributeError:  # paho-mqtt 1.x
            client = mqtt.Client(client_id="swarm-hub")
        cfg = self.config["mqtt"]
        if cfg.get("user"):
            client.username_pw_set(cfg["user"], cfg.get("password", ""))
        if cfg.get("tls"):
            import ssl
            client.tls_set(cert_reqs=ssl.CERT_NONE)
            client.tls_insecure_set(True)
        client.on_connect = self._on_connect
        client.on_message = self._on_message
        client.reconnect_delay_set(1, 30)
        return client

    def _on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            log.error("MQTT: отказ подключения, код %s", rc)
            return
        log.info("MQTT: подключено")
        client.subscribe("swarm/dev/+/+", qos=1)

    def _on_message(self, client, userdata, msg):
        parts = msg.topic.split("/")
        if len(parts) != 4 or parts[0] != "swarm" or parts[1] != "dev":
            return
        device_id, leaf = parts[2], parts[3]
        try:
            if leaf == "status":
                self._on_status(device_id, msg.payload.decode("utf-8", "replace"), msg.retain)
            elif leaf == "meta":
                with self.lock:
                    self._device(device_id)["meta"] = json.loads(msg.payload or b"{}")
            elif leaf == "state":
                with self.lock:
                    self._device(device_id)["state"] = json.loads(msg.payload or b"{}")
            elif leaf == "event":
                self._on_event(device_id, json.loads(msg.payload or b"{}"))
            elif leaf == "photo":
                self._on_photo(device_id, msg.payload)
        except (ValueError, UnicodeDecodeError) as error:
            log.warning("Битое сообщение %s: %s", msg.topic, error)

    def _device(self, device_id):
        return self.devices.setdefault(device_id, {"meta": {}, "state": {}, "online": False,
                                                    "since": time.time(), "alerted": False})

    def _on_status(self, device_id, status, retained):
        online = status == "online"
        with self.lock:
            device = self._device(device_id)
            was_alerted = device["alerted"]
            changed = device["online"] != online
            device["online"] = online
            if changed:
                device["since"] = time.time()
            if online:
                device["alerted"] = False
        if online and was_alerted and not retained:
            self.broadcast(f"✅ {self.label(device_id)}: снова на связи")

    def check_offline(self):
        """Раз в несколько секунд: устройство молчит дольше порога — сообщаем один раз."""
        now = time.time()
        alerts = []
        with self.lock:
            for device_id, device in self.devices.items():
                if (not device["online"] and not device["alerted"]
                        and now - device["since"] >= self.offline_alert_seconds
                        and now - self.started >= self.offline_alert_seconds):
                    device["alerted"] = True
                    alerts.append(device_id)
        for device_id in alerts:
            seconds = self.offline_alert_seconds
            span = f"{seconds // 60} мин" if seconds >= 60 else f"{seconds} с"
            self.broadcast(f"⚠️ {self.label(device_id)}: нет связи больше {span}")

    def send_command(self, cmd, chat, device_id=None, device_type=None, arg=""):
        if device_id:
            topic = f"swarm/dev/{device_id}/cmd"
        elif device_type:
            topic = f"swarm/type/{device_type}/cmd"
        else:
            topic = "swarm/all/cmd"
        payload = json.dumps({"cmd": cmd, "arg": arg, "chat": str(chat)}, ensure_ascii=False)
        self.mqtt.publish(topic, payload, qos=1)
        log.info("Команда %s -> %s", cmd, topic)

    # -------------------------------------------------------------- события --
    def name(self, device_id):
        with self.lock:
            device = self.devices.get(device_id, {})
            meta, state = device.get("meta", {}), device.get("state", {})
        return meta.get("name") or state.get("name") or device_id

    def dtype(self, device_id):
        with self.lock:
            return self.devices.get(device_id, {}).get("meta", {}).get("type", "")

    def label(self, device_id):
        return f"{TYPE_ICONS.get(self.dtype(device_id), '📟')} {self.name(device_id)}"

    def _on_event(self, device_id, event):
        text = f"{self.label(device_id)}: {event.get('text', '')}"
        chat = event.get("chat")
        if event.get("kind") == "reply" and chat:
            self.send(chat, text)
        else:
            self.broadcast(text)

    def _on_photo(self, device_id, payload):
        newline = payload.find(b"\n")
        if newline < 0:
            return
        header = json.loads(payload[:newline] or b"{}")
        jpeg = payload[newline + 1:]
        caption = f"{self.label(device_id)}: {header.get('caption', '')}"
        chat = header.get("chat")
        targets = [str(chat)] if chat else list(self.chats)
        self.outbox.put(lambda: self._send_photo(targets, jpeg, caption))

    def _send_photo(self, targets, jpeg, caption):
        file_id = None
        for chat in targets:
            if file_id:
                result = self.tg.call("sendPhoto", {"chat_id": chat, "photo": file_id, "caption": caption})
                if result.get("ok"):
                    continue
            result = self.tg.call("sendPhoto", data={"chat_id": chat, "caption": caption},
                                  files={"photo": ("capture.jpg", jpeg, "image/jpeg")}, timeout=60)
            if result.get("ok") and not file_id:
                try:
                    file_id = result["result"]["photo"][-1]["file_id"]
                except (KeyError, IndexError, TypeError):
                    pass

    # ------------------------------------------------------------ Telegram --
    def send(self, chat, text, keyboard=None):
        data = {"chat_id": str(chat), "text": text}
        if keyboard:
            data["reply_markup"] = {"inline_keyboard": keyboard}
        self.outbox.put(lambda: self.tg.call("sendMessage", data))

    def broadcast(self, text):
        for chat in self.chats:
            self.send(chat, text)

    def outbox_worker(self):
        while True:
            job = self.outbox.get()
            try:
                job()
            except Exception:  # одно сломанное сообщение не должно останавливать хаб
                log.exception("Ошибка отправки в Telegram")

    def main_keyboard(self):
        return [[{"text": "📋 Устройства", "callback_data": "list"}],
                [{"text": "🛡 Все на охрану", "callback_data": "grp:guard:arm"},
                 {"text": "🔓 Снять всех", "callback_data": "grp:guard:disarm"}],
                [{"text": "ℹ️ Статус всех", "callback_data": "status"}]]

    def device_keyboard(self, device_id):
        with self.lock:
            state = self.devices.get(device_id, {}).get("state", {})
        armed = state.get("armed", True)
        periodic = state.get("periodic", True)
        return [[{"text": "📷 Снимок", "callback_data": f"cmd:{device_id}:getcapture"},
                 {"text": "🔓 Снять с охраны" if armed else "🛡 На охрану",
                  "callback_data": f"cmd:{device_id}:{'disarm' if armed else 'arm'}"}],
                [{"text": "⏱ Плановые: выключить" if periodic else "⏱ Плановые: включить",
                  "callback_data": f"cmd:{device_id}:{'periodic_off' if periodic else 'periodic_on'}"},
                 {"text": "ℹ️ Статус", "callback_data": f"st:{device_id}"}],
                [{"text": "« Все устройства", "callback_data": "list"}]]

    def help_text(self):
        return ("🐝 Хаб роя: один бот для всех устройств.\n\n"
                "Команды (имя устройства — необязательно):\n"
                "/devices — список устройств с кнопками\n"
                "/arm [имя] — на охрану (без имени — все охранники)\n"
                "/disarm [имя] — снять с охраны\n"
                "/photo [имя] — снимок сейчас\n"
                "/periodic on|off [имя] — плановые снимки\n"
                "/status [имя] — состояние\n\n"
                "Имя — как в настройках устройства («Скворечник»), его ID или тип "
                "(guard); «все» — всем устройствам.")

    def device_status(self, device_id):
        with self.lock:
            device = dict(self.devices.get(device_id, {}))
        state, online = device.get("state", {}), device.get("online", False)
        lines = [f"{self.label(device_id)} — " + ("🟢 в сети" if online else "⚫ не на связи")]
        if not state:
            return lines[0] + "\n(состояние ещё не получено)"
        if "armed" in state:
            lines.append("На охране" if state["armed"] else "Снят с охраны")
        if "periodic" in state:
            lines.append(f"Плановые снимки: каждые {state.get('periodicMinutes', '?')} мин"
                         if state["periodic"] else "Плановые снимки: выключены")
        if "cameraStatus" in state:
            lines.append(f"Камера: {state['cameraStatus']}")
        owner = state.get("owner", {})
        if owner.get("enabled"):
            lines.append("Капитан: " + ("на мостике" if owner.get("present") else "не на мостике"))
        quiet = state.get("quiet", {})
        if quiet.get("enabled"):
            lines.append(f"Тихие часы {quiet.get('start')}–{quiet.get('end')}"
                         + (" (сейчас)" if quiet.get("now") else ""))
        if state.get("pending"):
            lines.append(f"Отложено событий: {state['pending']}")
        extra = []
        if "rssi" in state:
            extra.append(f"сигнал {state['rssi']} дБм")
        if "uptime" in state:
            up = int(state["uptime"])
            extra.append(f"аптайм {up // 86400} д {up % 86400 // 3600:02d}:{up % 3600 // 60:02d}")
        if "fw" in state:
            extra.append(f"прошивка {state['fw']}")
        if extra:
            lines.append(", ".join(extra))
        return "\n".join(lines)

    def resolve(self, target):
        """Возвращает (ids, type, error): список устройств, тип группы или текст ошибки."""
        word = casefold(target)
        with self.lock:
            ids = sorted(self.devices)
            names = {d: casefold(self.devices[d]["meta"].get("name") or self.devices[d]["state"].get("name") or d)
                     for d in ids}
            types = {self.devices[d]["meta"].get("type", "") for d in ids}
        if not word:
            return [], "guard", None
        if word in ALL_WORDS:
            return [], "*", None
        if word in types:
            return [], word, None
        exact = [d for d in ids if names[d] == word or casefold(d) == word]
        if exact:
            return exact, None, None
        prefix = [d for d in ids if names[d].startswith(word) or casefold(d).startswith(word)]
        if len(prefix) == 1:
            return prefix, None, None
        if len(prefix) > 1:
            return [], None, "Под это имя подходят несколько устройств: " + \
                   ", ".join(self.name(d) for d in prefix)
        return [], None, f"Не знаю устройство «{target}». Список: /devices"

    def dispatch(self, cmd, chat, target, arg=""):
        ids, group, error = self.resolve(target)
        if error:
            self.send(chat, error)
            return
        if ids:
            for device_id in ids:
                self.send_command(cmd, chat, device_id=device_id, arg=arg)
        elif group == "*":
            self.send_command(cmd, chat, arg=arg)
        else:
            self.send_command(cmd, chat, device_type=group, arg=arg)

    def list_devices(self, chat):
        with self.lock:
            ids = sorted(self.devices, key=lambda d: casefold(self.name_unlocked(d)))
        if not ids:
            self.send(chat, "Устройств пока нет: ни одно не подключалось к брокеру.", self.main_keyboard())
            return
        rows, lines = [], []
        for device_id in ids:
            with self.lock:
                device = self.devices[device_id]
                online, armed = device["online"], device["state"].get("armed")
            mark = "🟢" if online else "⚫"
            guard = "" if armed is None else (" · на охране" if armed else " · снят")
            lines.append(f"{mark} {self.label(device_id)}{guard}")
            rows.append([{"text": f"{mark} {self.name(device_id)}", "callback_data": f"dev:{device_id}"}])
        rows += self.main_keyboard()[1:]
        self.send(chat, "Устройства:\n" + "\n".join(lines), rows)

    def name_unlocked(self, device_id):
        device = self.devices.get(device_id, {})
        return device.get("meta", {}).get("name") or device.get("state", {}).get("name") or device_id

    def status_all(self, chat):
        with self.lock:
            ids = sorted(self.devices)
        if not ids:
            self.send(chat, "Устройств пока нет.")
            return
        self.send(chat, "\n\n".join(self.device_status(d) for d in ids), self.main_keyboard())

    def photo(self, chat, target):
        if target:
            self.dispatch("getcapture", chat, target)
            return
        with self.lock:
            cameras = [d for d, dev in self.devices.items()
                       if dev["online"] and dev["meta"].get("type") == "guard"]
        if len(cameras) == 1:
            self.send_command("getcapture", chat, device_id=cameras[0])
        elif not cameras:
            self.send(chat, "Нет камер на связи.")
        else:
            rows = [[{"text": f"📷 {self.name(d)}", "callback_data": f"cmd:{d}:getcapture"}] for d in sorted(cameras)]
            self.send(chat, "С какой камеры?", rows)

    def handle_text(self, chat, text):
        text = text.strip()
        if not text.startswith("/"):
            self.send(chat, "Команды: /help", self.main_keyboard())
            return
        head, _, rest = text[1:].partition(" ")
        cmd = head.split("@")[0].lower()
        rest = rest.strip()
        if cmd in ("start", "help"):
            self.send(chat, self.help_text(), self.main_keyboard())
        elif cmd in ("devices", "list"):
            self.list_devices(chat)
        elif cmd in ("arm", "disarm"):
            self.dispatch(cmd, chat, rest)
        elif cmd in ("photo", "getcapture", "capture"):
            self.photo(chat, rest)
        elif cmd == "periodic":
            mode, _, target = rest.partition(" ")
            if casefold(mode) not in ("on", "off", "вкл", "выкл"):
                self.send(chat, "Так: /periodic on [имя] или /periodic off [имя]")
                return
            self.dispatch("periodic", chat, target.strip(), arg=casefold(mode))
        elif cmd == "status":
            if not rest:
                self.status_all(chat)
            else:
                ids, group, error = self.resolve(rest)
                if error:
                    self.send(chat, error)
                elif ids:
                    for device_id in ids:
                        self.send(chat, self.device_status(device_id), self.device_keyboard(device_id))
                else:
                    self.status_all(chat)
        else:
            self.send(chat, "Не знаю такой команды. /help", self.main_keyboard())

    def handle_callback(self, chat, data):
        if data == "list":
            self.list_devices(chat)
        elif data == "status":
            self.status_all(chat)
        elif data.startswith("dev:") or data.startswith("st:"):
            device_id = data.split(":", 1)[1]
            self.send(chat, self.device_status(device_id), self.device_keyboard(device_id))
        elif data.startswith("cmd:"):
            _, device_id, cmd = data.split(":", 2)
            if cmd in ("periodic_on", "periodic_off"):
                self.send_command("periodic", chat, device_id=device_id, arg=cmd.split("_")[1])
            else:
                self.send_command(cmd, chat, device_id=device_id)
        elif data.startswith("grp:"):
            _, group, cmd = data.split(":", 2)
            self.send_command(cmd, chat, device_type=group)

    def poll_telegram(self):
        offset = None
        self.tg.call("setMyCommands", {"commands": [
            {"command": "devices", "description": "Список устройств"},
            {"command": "arm", "description": "На охрану [имя]"},
            {"command": "disarm", "description": "Снять с охраны [имя]"},
            {"command": "photo", "description": "Снимок [имя]"},
            {"command": "periodic", "description": "Плановые снимки on|off [имя]"},
            {"command": "status", "description": "Состояние [имя]"},
            {"command": "help", "description": "Справка"}]})
        while True:
            params = {"timeout": 25, "allowed_updates": ["message", "callback_query"]}
            if offset is not None:
                params["offset"] = offset
            result = self.tg.call("getUpdates", params, timeout=35)
            if not result.get("ok"):
                time.sleep(5)
                continue
            for update in result.get("result", []):
                offset = update["update_id"] + 1
                try:
                    self.handle_update(update)
                except Exception:
                    log.exception("Ошибка обработки обновления")

    def handle_update(self, update):
        callback = update.get("callback_query")
        message = update.get("message")
        if callback:
            chat = str(callback.get("message", {}).get("chat", {}).get("id", ""))
            self.outbox.put(lambda: self.tg.call("answerCallbackQuery", {"callback_query_id": callback["id"]}))
            if chat in self.chats:
                self.handle_callback(chat, callback.get("data", ""))
        elif message and "text" in message:
            chat = str(message["chat"]["id"])
            if chat in self.chats:
                self.handle_text(chat, message["text"])
            elif message["text"].startswith("/start"):
                log.info("Незнакомый чат %s", chat)
                self.send(chat, f"Нет доступа. Ваш chat ID: {chat} — добавьте его в allowed_chats хаба.")

    def run(self):
        cfg = self.config["mqtt"]
        self.mqtt.connect_async(cfg.get("host", "127.0.0.1"), int(cfg.get("port", 1883)), keepalive=30)
        self.mqtt.loop_start()
        threading.Thread(target=self.outbox_worker, daemon=True).start()
        threading.Thread(target=self.poll_telegram, daemon=True).start()
        log.info("Хаб запущен, чатов: %d", len(self.chats))
        while True:
            time.sleep(5)
            self.check_offline()


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    path = sys.argv[1] if len(sys.argv) > 1 else "/etc/swarm-hub/config.json"
    with open(path, encoding="utf-8") as handle:
        config = json.load(handle)
    Hub(config).run()


if __name__ == "__main__":
    main()
