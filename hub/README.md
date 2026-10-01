# Хаб роя: один Telegram-бот для всех устройств

Устройства (охранники `guard`, позже — садовники, термостаты) подключаются к MQTT-брокеру на вашем VPS. Сервис `swarm_hub.py` на том же VPS — единственный Telegram-бот: показывает список устройств, раздаёт команды и пересылает в чаты события и фото.

```
ESP32 ──TLS:8883──▶ Mosquitto (VPS) ◀──127.0.0.1:1883── swarm_hub.py ──HTTPS──▶ Telegram
```

VPS должен быть за пределами блокировки: хаб ходит в `api.telegram.org` напрямую. Прокси nginx для Telegram больше не нужен.

## 1. Mosquitto (Ubuntu/Debian, под root)

```bash
apt update && apt install -y mosquitto mosquitto-clients python3-paho-mqtt python3-requests openssl

# Сертификат для TLS (устройства его не проверяют, нужен только для шифрования)
mkdir -p /etc/mosquitto/certs
openssl req -x509 -nodes -days 3650 -newkey rsa:2048 -subj "/CN=swarm" \
  -keyout /etc/mosquitto/certs/swarm.key -out /etc/mosquitto/certs/swarm.crt

# Пользователи: hub — для хаба, device — общий для всех устройств
mosquitto_passwd -c /etc/mosquitto/swarm.passwd hub 
   2337.... 
mosquitto_passwd /etc/mosquitto/swarm.passwd device

# Конфиг и права из этой папки
cp mosquitto-swarm.conf /etc/mosquitto/conf.d/swarm.conf
cp mosquitto-swarm.acl /etc/mosquitto/swarm.acl
chown mosquitto: /etc/mosquitto/swarm.passwd /etc/mosquitto/swarm.acl /etc/mosquitto/certs/*
chmod 600 /etc/mosquitto/swarm.passwd /etc/mosquitto/swarm.acl /etc/mosquitto/certs/swarm.key

systemctl restart mosquitto
ufw allow 8883/tcp   # если включён ufw; и в firewall хостера тоже
```

Если в `/etc/mosquitto/mosquitto.conf` уже есть свои `listener`, уберите их или поправьте `swarm.conf` — два одинаковых порта брокер не откроет.

Проверка (с любого компьютера):

```bash
mosquitto_pub --insecure -h IP_VPS -p 8883 -u device -P ПАРОЛЬ_DEVICE -t swarm/dev/test/status -m online
```

Тишина — успех; `not authorised` — неверный пароль; `Connection refused`/таймаут — закрыт порт.

## 2. Сервис-бот

```bash
useradd --system --no-create-home swarmhub
mkdir -p /opt/swarm-hub /etc/swarm-hub
cp swarm_hub.py /opt/swarm-hub/
cp config.example.json /etc/swarm-hub/config.json
nano /etc/swarm-hub/config.json      # токен бота, chat ID, пароль пользователя hub
chown root:swarmhub /etc/swarm-hub/config.json && chmod 640 /etc/swarm-hub/config.json
cp swarm-hub.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now swarm-hub
journalctl -u swarm-hub -f           # лог; Ctrl+C — выйти
```

`config.json`:

| Поле | Что это |
|---|---|
| `telegram_token` | токен бота от @BotFather. Лучше новый бот только для хаба: если тот же токен останется в прошивке камеры в прямом режиме, они будут перехватывать сообщения друг у друга |
| `allowed_chats` | ID чатов, которым бот подчиняется и куда шлёт тревоги. Свой ID: напишите боту `/start` — он ответит «Нет доступа. Ваш chat ID: …» |
| `offline_alert_seconds` | через сколько секунд без связи сообщать «нет связи» (по умолчанию 180) |
| `mqtt` | подключение хаба к брокеру: `127.0.0.1:1883`, пользователь `hub` |

## 3. Устройство

В веб-панели камеры, раздел «Хаб роя (MQTT)»:

- **Адрес брокера** — IP или имя VPS;
- **Порт** — `8883` (TLS; `1883` — без шифрования, только для локальной сети);
- **Логин / пароль** — пользователь `device`;
- **Имя устройства** — как оно будет называться в боте: «Скворечник», «Калитка».

После сохранения и перезапуска в «Состоянии» должно быть `Хаб (MQTT): IP:8883 (TLS): подключено`, а в боте `/devices` — новое устройство.

Если адрес брокера пуст, камера работает как раньше — напрямую со своим Telegram-ботом.

## Команды бота

| Команда | Действие |
|---|---|
| `/devices` | список устройств: 🟢 в сети / ⚫ нет, кнопка на каждое — меню устройства |
| `/arm [имя]` | на охрану; без имени — все охранники (`guard`) |
| `/disarm [имя]` | снять с охраны |
| `/photo [имя]` | снимок сейчас; без имени — с единственной камеры или кнопки выбора |
| `/periodic on\|off [имя]` | плановые снимки; без имени — у всех охранников |
| `/status [имя]` | состояние; без имени — всех |

Имя — из настроек устройства, без учёта регистра; можно начало имени (`/status скв`), ID (`guard-957338`), тип (`guard`) или `все`. Снятое с охраны устройство не шлёт тревог по движению; плановые снимки управляются отдельно. Режимы сохраняются на устройстве и переживают перезагрузку.

Хаб сообщает, когда устройство пропало со связи дольше `offline_alert_seconds`, и когда вернулось.

## Протокол (для новых типов устройств)

| Топик | Кто пишет | Содержимое |
|---|---|---|
| `swarm/dev/<id>/status` | устройство (Last Will) | `online` / `offline`, retained |
| `swarm/dev/<id>/meta` | устройство | `{"id","name","type","fw","ip"}`, retained |
| `swarm/dev/<id>/state` | устройство | произвольный JSON состояния, retained; хаб понимает `armed`, `periodic`, `periodicMinutes`, `cameraStatus`, `owner`, `quiet`, `pending`, `rssi`, `uptime`, `fw` |
| `swarm/dev/<id>/event` | устройство | `{"kind","text","chat"?}`; `kind:"reply"` с `chat` — ответ одному чату, остальное — всем |
| `swarm/dev/<id>/photo` | устройство | строка JSON `{"caption","kind","chat"?}`, `\n`, затем JPEG |
| `swarm/dev/<id>/cmd`, `swarm/type/<type>/cmd`, `swarm/all/cmd` | хаб | `{"cmd","arg","chat"}` |

Новый тип устройства («садовник») публикует те же топики со своим `type` и обрабатывает свои команды; общие (`status`) — как все.
