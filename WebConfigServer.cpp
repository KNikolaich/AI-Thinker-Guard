#include "WebConfigServer.h"
#include <WiFi.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "SystemHealth.h"
#include "TelegramService.h"
#include "WifiPresence.h"
#include <ctype.h>
#include <stdlib.h>

static bool parseIntegerValue(String value, long &parsed) {
  value.trim();
  if (value.isEmpty()) return false;
  char *end = nullptr;
  parsed = strtol(value.c_str(), &end, 10);
  return end != value.c_str() && *end == '\0';
}

// Пароль Wi-Fi (WPA2) и Basic Auth надёжно работают только с латиницей,
// цифрами и печатными ASCII-символами; кириллица ломает вход.
bool isValidDevicePassword(const String &value) {
  if (value.length() < 8 || value.length() > 63) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 32 || c > 126) return false;
  }
  return true;
}

static bool isValidClockValue(const String &value) {
  if (value.length() != 5 || value[2] != ':') return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (i == 2) continue;
    if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  const int hour = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}


static bool isValidBotTokenValue(const String &token) {
  const int separator = token.indexOf(':');
  if (separator <= 0 || separator >= static_cast<int>(token.length()) - 1) return false;
  for (int i = 0; i < separator; ++i) {
    if (!isdigit(static_cast<unsigned char>(token[i]))) return false;
  }
  for (size_t i = separator + 1; i < token.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(token[i]);
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

// Единая проверка настроек: для формы и для загруженного JSON.
// Пустая строка = всё в порядке, иначе текст ошибки для пользователя.
static bool isValidHostValue(const String &value) {
  if (value.isEmpty() || value.length() > 63) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    if (!isalnum(ch) && ch != '.' && ch != '-') return false;
  }
  return true;
}

static bool isPrintableAscii(const String &value, size_t maxLength) {
  if (value.length() > maxLength) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    if (ch < 32 || ch > 126) return false;
  }
  return true;
}

static String validateConfig(const AppConfig &c) {
  if (c.deviceName.length() > 48) return "Имя устройства — до 48 байт (примерно 24 русских буквы).";
  for (size_t i = 0; i < c.deviceName.length(); ++i) {
    if (static_cast<unsigned char>(c.deviceName[i]) < 32) return "Имя устройства не должно содержать управляющих символов.";
  }
  if (!c.mqttHost.isEmpty() && !isValidHostValue(c.mqttHost))
    return "Адрес MQTT-брокера: имя хоста или IP без https:// и порта.";
  if (c.mqttPort == 0) return "Порт MQTT — от 1 до 65535 (обычно 8883 с TLS).";
  if (!isPrintableAscii(c.mqttUser, 64) || !isPrintableAscii(c.mqttPassword, 64))
    return "Логин и пароль MQTT — до 64 символов латиницей, цифрами и знаками.";
  if (c.wifiSsid.length() > 32) return "SSID Wi-Fi — до 32 символов.";
  if (c.wifiPassword.length() > 0 && (c.wifiPassword.length() < 8 || c.wifiPassword.length() > 64))
    return "Пароль Wi-Fi — от 8 до 64 символов.";
  if (!c.telegramToken.isEmpty() &&
      (c.telegramToken.length() > 128 || !isValidBotTokenValue(c.telegramToken)))
    return "Токен Telegram имеет неверный формат. Он должен содержать числовой ID и двоеточие.";
  {
    String host, prefix;
    uint16_t port = 443;
    if (!TelegramService::parseApiEndpoint(c.telegramApiHost, host, port, prefix))
      return "Адрес API Telegram: host[:порт][/путь], только латиница, цифры, точки и дефисы.";
  }
  {
    String ids[TelegramService::MAX_CHATS];
    uint8_t count = 0;
    if (!TelegramService::parseChatIds(c.chatId, ids, count))
      return "ID чатов: числа через запятую (до 5), например 123456789, -1001234567890.";
  }
  if (!c.ownerIp.isEmpty() && !WifiPresence::isValidIp(c.ownerIp))
    return "IP телефона капитана: адрес вида 192.168.1.50; оставьте пустым, чтобы отключить.";
  if (c.devicePasswordCustom && !isValidDevicePassword(c.devicePassword))
    return "Пароль панели: 8–63 символа, только латиница, цифры и знаки (без кириллицы).";
  if (c.motionCount < 1 || c.motionCount > 5) return "N должно быть от 1 до 5.";
  if (c.manualCount < 1 || c.manualCount > 5) return "M должно быть от 1 до 5.";
  if (c.periodicMinutes < 1 || c.periodicMinutes > 1440) return "X должно быть от 1 до 1440 минут.";
  if (c.timeoutSeconds < 1 || c.timeoutSeconds > 86400) return "TO должно быть от 1 до 86400 секунд.";
  if (c.ownerAwayMinutes < 1 || c.ownerAwayMinutes > 60) return "Время отсутствия капитана — от 1 до 60 минут.";
  if (c.timezoneOffsetMinutes < -720 || c.timezoneOffsetMinutes > 840)
    return "Часовой пояс должен быть в диапазоне от -720 до 840 минут.";
  if (c.quietEnabled && (!isValidClockValue(c.quietStart) || !isValidClockValue(c.quietEnd) ||
                         c.quietStart == c.quietEnd))
    return "Для тихих часов задайте разные значения времени в формате ЧЧ:ММ.";
  return "";
}

void WebConfigServer::begin(ConfigStore &store, AppConfig &config) {
  store_ = &store;
  config_ = &config;
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/retry", HTTP_POST, [this]() { handleRetry(); });
  server_.on("/update", HTTP_GET, [this]() { handleUpdatePage(); });
  server_.on("/config", HTTP_GET, [this]() { handleConfigPage(); });
  server_.on("/config.json", HTTP_GET, [this]() { handleConfigExport(); });
  server_.on("/config", HTTP_POST, [this]() { handleConfigImport(); });
  server_.on("/update", HTTP_POST, [this]() { handleUpdateDone(); },
             [this]() { handleUpdateUpload(); });
  // Любой неизвестный адрес (в т.ч. проверки «есть ли интернет» от Android/iOS/
  // Windows: generate_204, hotspot-detect.html и т.п.) перенаправляем на панель.
  // Для клиентов точки доступа адрес абсолютный, чтобы телефон открыл окно
  // «Вход в сеть» с нашей страницей, а не считал сеть сломанной.
  server_.onNotFound([this]() {
    const String target = requestFromAccessPoint()
                              ? "http://" + WiFi.softAPIP().toString() + "/"
                              : String("/");
    server_.sendHeader("Location", target, true);
    server_.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server_.send(302, "text/plain", "");
  });
  server_.begin();
}

void WebConfigServer::handleClient() {
  server_.handleClient();
}

bool WebConfigServer::requestFromAccessPoint() {
  if (!(WiFi.getMode() & WIFI_MODE_AP)) return false;
  return server_.client().localIP() == WiFi.softAPIP();
}

bool WebConfigServer::authenticate() {
  if (config_ == nullptr) return false;
  // Клиент точки доступа уже ввёл пароль устройства при подключении к Wi-Fi
  // (WPA2). Повторный Basic Auth не нужен, а окно «Вход в сеть» на телефонах
  // его и не умеет показывать. В домашней сети логин admin по-прежнему нужен.
  if (apProtected_ && requestFromAccessPoint()) return true;
  if (config_->devicePassword.length() >= 8 &&
      server_.authenticate("admin", config_->devicePassword.c_str())) return true;
  server_.requestAuthentication(BASIC_AUTH, "AI-Thinker Guard");
  return false;
}

String WebConfigServer::statusHtml() const {
  if (!statusProvider_) return "";
  const String raw = statusProvider_();
  String html = "<h2>Состояние</h2><ul class=\"status\">";
  int start = 0;
  while (start < static_cast<int>(raw.length())) {
    int end = raw.indexOf('\n', start);
    if (end < 0) end = raw.length();
    const String line = raw.substring(start, end);
    if (line.length()) {
      const bool warn = line.startsWith("!");
      html += warn ? "<li class=\"bad\">" : "<li>";
      html += escapeHtml(warn ? line.substring(1) : line);
      html += "</li>";
    }
    start = end + 1;
  }
  html += "</ul><form method=\"post\" action=\"/retry\"><button class=\"secondary\" type=\"submit\">"
          "Повторить инициализацию всех модулей (перезапуск)</button></form>";
  return html;
}

void WebConfigServer::restartSoon(const String &message) {
  server_.send(200, "text/html; charset=utf-8",
               "<!doctype html><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"25;url=/\">"
               "<p>" + message + "</p>");
  delay(700);
  if (beforeRestart_) beforeRestart_();
  ESP.restart();
}

void WebConfigServer::handleRetry() {
  if (!authenticate()) return;
  restartSoon("Блокировки модулей сняты. Перезапуск…");
}

String WebConfigServer::escapeHtml(const String &value) const {
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("\"", "&quot;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  return escaped;
}

String WebConfigServer::textField(const String &label, const String &name,
                                  const String &value, const String &type) const {
  return "<label>" + label + "<input type=\"" + type + "\" name=\"" + name +
         "\" value=\"" + escapeHtml(value) + "\"></label>";
}

String WebConfigServer::passwordField(const String &label, const String &name) const {
  const String constraints = name == "devicePassword"
                                 ? " minlength=\"8\" maxlength=\"63\""
                                 : "";
  return "<label>" + label + "<input type=\"password\" name=\"" + name +
         "\" value=\"\" placeholder=\"пусто — оставить без изменений\"" +
         constraints + "></label>";
}

void WebConfigServer::handleRoot() {
  if (!authenticate()) return;
  if (config_ == nullptr) {
    server_.send(500, "text/plain; charset=utf-8", "Нет конфигурации");
    return;
  }

  String page = R"HTML(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI-Thinker Guard</title><style>
body{font:16px system-ui,sans-serif;background:#f4f6f8;color:#17212b;margin:0;padding:24px}
main{max-width:760px;margin:auto;background:white;padding:24px;border-radius:14px;box-shadow:0 4px 24px #0001}
h1{margin-top:0}h2{font-size:1.1rem;border-bottom:1px solid #ddd;padding-bottom:8px;margin-top:28px}
label{display:block;margin:14px 0;font-weight:600}input{display:block;box-sizing:border-box;width:100%;padding:11px;margin-top:6px;border:1px solid #bbc4ce;border-radius:7px;font:inherit}
.row{display:grid;grid-template-columns:1fr 1fr;gap:14px}.check{display:flex;align-items:center;gap:10px}.check input{width:auto;margin:0}
button{background:#1769aa;color:white;border:0;border-radius:8px;padding:13px 20px;font:inherit;font-weight:700;cursor:pointer}
.status{padding-left:20px;line-height:1.6}.status .bad{color:#a4262c;font-weight:600}
button.secondary{background:#5c6b7a;margin-top:6px}
nav{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 18px}nav a{background:#e8eef4;color:#17212b;text-decoration:none;padding:9px 14px;border-radius:8px;font-weight:600}
.note{color:#52616f;font-size:.92rem;line-height:1.5}.warn{background:#fff5d6;padding:12px;border-radius:8px}
@media(max-width:600px){body{padding:10px}.row{grid-template-columns:1fr}}
</style></head><body><main><h1>AI-Thinker Guard</h1>
<p class="note">Настройки сохраняются во внутреннюю NVS-память. После сохранения камера перезапустится.</p>)HTML";
  page += menuHtml();
  page += statusHtml();
  page += R"HTML(<form method="post" action="/save">
<h2>Устройство</h2>)HTML";
  page += textField("Имя устройства (например, Скворечник; пусто — ID)", "deviceName", config_->deviceName);
  page += R"HTML(<label class="check"><input type="checkbox" name="armed" value="1")HTML";
  if (config_->armed) page += " checked";
  page += R"HTML(> На охране (тревоги по движению)</label>
<label class="check"><input type="checkbox" name="periodicEnabled" value="1")HTML";
  if (config_->periodicEnabled) page += " checked";
  page += R"HTML(> Плановые снимки включены</label>
<h2>Хаб роя (MQTT)</h2><div class="row">)HTML";
  page += textField("Адрес брокера (пусто — прямой режим Telegram)", "mqttHost", config_->mqttHost);
  page += textField("Порт (8883 — TLS, 1883 — без шифрования)", "mqttPort", String(config_->mqttPort), "number");
  page += "</div><div class=\"row\">";
  page += textField("Логин MQTT", "mqttUser", config_->mqttUser);
  page += passwordField("Пароль MQTT", "mqttPassword");
  page += R"HTML(</div><p class="note">С хабом устройство общается только с брокером на вашем сервере, а Telegram-бот хаба управляет всеми устройствами сразу. Поля Telegram ниже тогда не используются.</p>
<h2>Wi-Fi</h2>)HTML";
  page += textField("Имя сети (SSID)", "wifiSsid", config_->wifiSsid);
  page += passwordField("Пароль Wi-Fi", "wifiPassword");
  page += R"HTML(<h2>Telegram (прямой режим, без хаба)</h2>)HTML";
  page += passwordField("Токен бота", "telegramToken");
  page += textField("ID чатов через запятую (до 5)", "chatId", config_->chatId);
  page += textField("Адрес API Telegram (пусто — api.telegram.org)", "telegramApiHost",
                    config_->telegramApiHost);
  page += R"HTML(<p class="note">Каждый из этих чатов получает тревоги и может командовать ботом; остальные чаты бот игнорирует. Узнать свой ID: @userinfobot, или напишите боту — незнакомый ID появится в «Состоянии».
Если Telegram заблокирован, укажите свой прокси в виде <code>host[:порт][/путь]</code>, например <code>203.0.113.10:8443/k3y</code>.</p>
<h2>Снимки и движение</h2><div class="row">)HTML";
  page += textField("N снимков по движению (1–5)", "motionCount",
                    String(config_->motionCount), "number");
  page += textField("M снимков по GetCapture (1–5)", "manualCount",
                    String(config_->manualCount), "number");
  page += "</div><div class=\"row\">";
  page += textField("X: интервал планового снимка, минут (1–1440)", "periodicMinutes",
                    String(config_->periodicMinutes), "number");
  page += textField("TO: пауза между тревогами, секунд (1–86400)", "timeoutSeconds",
                    String(config_->timeoutSeconds), "number");
  page += R"HTML(</div><p class="note">Детектор сравнивает уменьшенные до 80×60 серые копии кадров. Снимки отправляются в VGA.</p>
<h2>Капитан и расписание тишины</h2>)HTML";
  page += "<div class=\"row\">";
  page += textField("IP телефона капитана в этой Wi-Fi сети (пусто — отключено)", "ownerIp",
                    config_->ownerIp);
  page += textField("Капитан ушёл, если телефон молчит, минут (1–60)", "ownerAwayMinutes",
                    String(config_->ownerAwayMinutes), "number");
  page += R"HTML(</div><p class="note">Пока телефон капитана подключён к Wi-Fi, тревоги по движению не отправляются.
Закрепите за телефоном постоянный IP в роутере (Keenetic: «Список устройств» → телефон → «Постоянный IP-адрес»).</p>)HTML";
  page += R"HTML(<label class="check"><input type="checkbox" name="quietEnabled" value="1")HTML";
  if (config_->quietEnabled) page += " checked";
  page += R"HTML(> Включить тихие часы</label><div class="row">)HTML";
  page += textField("Начало тишины (ЧЧ:ММ)", "quietStart", config_->quietStart);
  page += textField("Конец тишины (ЧЧ:ММ)", "quietEnd", config_->quietEnd);
  page += "</div>";
  page += textField("Часовой пояс: смещение от UTC в минутах", "timezoneOffsetMinutes",
                    String(config_->timezoneOffsetMinutes), "number");
  page += R"HTML(<p class="note">В тихие часы события копятся счётчиками в RAM. Изображения не сохраняются; после тишины отправляется сводка и один свежий кадр.</p>
<h2>Доступ и OTA</h2>)HTML";
  page += passwordField("Новый пароль веб-панели и OTA (8–63 символа)", "devicePassword");
  page += R"HTML(<label class="check"><input type="checkbox" name="otaEnabled" value="1")HTML";
  if (config_->otaEnabled) page += " checked";
  page += R"HTML(> Разрешить обновление прошивки (веб-страница /update и Arduino OTA) с этим же паролем</label>
<p class="note warn">Пароль резервной точки доступа не меняется (Guard + 5 цифр из MAC, см. «Состояние»). Пароль панели (логин admin) и OTA по умолчанию такой же; если задать свой, через точку доступа панель тоже будет спрашивать логин.</p>
<button type="submit">Сохранить и перезапустить</button></form>)HTML";
  page += "<p class=\"note\">Прошивка " + String(firmwareVersion_) + "</p></main></body></html>";
  server_.send(200, "text/html; charset=utf-8", page);
}

void WebConfigServer::handleSave() {
  if (!authenticate()) return;
  if (config_ == nullptr || store_ == nullptr) {
    server_.send(500, "text/plain; charset=utf-8", "Нет хранилища настроек");
    return;
  }

  AppConfig next = *config_;
  if (server_.hasArg("deviceName")) {
    next.deviceName = server_.arg("deviceName");
    next.deviceName.trim();
  }
  if (server_.hasArg("mqttHost")) {
    next.mqttHost = server_.arg("mqttHost");
    next.mqttHost.trim();
  }
  if (server_.hasArg("mqttUser")) {
    next.mqttUser = server_.arg("mqttUser");
    next.mqttUser.trim();
  }
  if (server_.hasArg("mqttPassword") && server_.arg("mqttPassword").length())
    next.mqttPassword = server_.arg("mqttPassword");
  next.armed = server_.hasArg("armed");
  next.periodicEnabled = server_.hasArg("periodicEnabled");
  {
    long port = 0;
    if (!parseIntegerValue(server_.arg("mqttPort"), port) || port < 1 || port > 65535) {
      server_.send(400, "text/plain; charset=utf-8", "Порт MQTT — число от 1 до 65535.");
      return;
    }
    next.mqttPort = static_cast<uint16_t>(port);
  }
  if (server_.hasArg("wifiSsid")) next.wifiSsid = server_.arg("wifiSsid");
  if (server_.hasArg("chatId")) next.chatId = server_.arg("chatId");
  if (server_.hasArg("telegramApiHost")) {
    next.telegramApiHost = server_.arg("telegramApiHost");
    next.telegramApiHost.trim();
  }
  if (server_.hasArg("ownerIp")) {
    next.ownerIp = server_.arg("ownerIp");
    next.ownerIp.trim();
  }
  if (server_.hasArg("quietStart")) next.quietStart = server_.arg("quietStart");
  if (server_.hasArg("quietEnd")) next.quietEnd = server_.arg("quietEnd");

  if (server_.hasArg("wifiPassword") && server_.arg("wifiPassword").length())
    next.wifiPassword = server_.arg("wifiPassword");
  if (server_.hasArg("telegramToken") && server_.arg("telegramToken").length())
    next.telegramToken = server_.arg("telegramToken");

  const String requestedDevicePassword = server_.arg("devicePassword");
  if (requestedDevicePassword.length() > 0) {
    next.devicePassword = requestedDevicePassword;
    next.devicePasswordCustom = true;
  }
  next.otaEnabled = server_.hasArg("otaEnabled");

  next.quietEnabled = server_.hasArg("quietEnabled");
  // Числа: пустое или нечисловое поле — ошибка; диапазоны проверяет validateConfig().
  struct NumberField { const char *name; const char *label; long value; };
  NumberField numbers[] = {
      {"motionCount", "N", 0},           {"manualCount", "M", 0},
      {"periodicMinutes", "X", 0},       {"timeoutSeconds", "TO", 0},
      {"ownerAwayMinutes", "время отсутствия капитана", 0},
      {"timezoneOffsetMinutes", "часовой пояс", 0},
  };
  for (NumberField &field : numbers) {
    if (!parseIntegerValue(server_.arg(field.name), field.value) ||
        field.value < -100000 || field.value > 100000) {
      server_.send(400, "text/plain; charset=utf-8", String("Поле «") + field.label + "» должно быть числом.");
      return;
    }
  }
  next.motionCount = static_cast<uint8_t>(constrain(numbers[0].value, 0, 255));
  next.manualCount = static_cast<uint8_t>(constrain(numbers[1].value, 0, 255));
  next.periodicMinutes = static_cast<uint16_t>(constrain(numbers[2].value, 0, 65535));
  next.timeoutSeconds = static_cast<uint32_t>(constrain(numbers[3].value, 0, 100000));
  next.ownerAwayMinutes = static_cast<uint16_t>(constrain(numbers[4].value, 0, 65535));
  next.timezoneOffsetMinutes = static_cast<int16_t>(numbers[5].value);

  storeAndRestart(next, "Настройки сохранены. Перезапуск…");
}

bool WebConfigServer::storeAndRestart(const AppConfig &next, const String &message) {
  const String problem = validateConfig(next);
  if (!problem.isEmpty()) {
    server_.send(400, "text/plain; charset=utf-8", problem);
    return false;
  }
  if (!store_->ready()) {
    server_.send(500, "text/plain; charset=utf-8",
                 "Память настроек (NVS) недоступна: сохранить нельзя. Подробности — в Serial Monitor.");
    return false;
  }
  if (!store_->save(next)) {
    server_.send(500, "text/plain; charset=utf-8", "Не удалось сохранить настройки");
    return false;
  }
  restartSoon(message);
  return true;
}
// ------------------------------------------------------ веб-обновление ---
// GET /update — страница выбора .bin; POST /update — потоковая запись в
// свободный OTA-слот (нужна схема разделов с OTA). Доступ — как к панели,
// и только если в настройках разрешено обновление прошивки.

bool WebConfigServer::updateAllowed() {
  return config_ != nullptr && config_->otaEnabled;
}

void WebConfigServer::handleUpdatePage() {
  if (!authenticate()) return;
  if (!updateAllowed()) {
    server_.send(403, "text/html; charset=utf-8",
                 "<!doctype html><meta charset=\"utf-8\"><p>Обновление выключено. Включите "
                 "«Разрешить обновление прошивки» на <a href=\"/\">главной странице</a>.</p>");
    return;
  }
  String page = R"HTML(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Обновление прошивки</title><style>
body{font:16px system-ui,sans-serif;background:#f4f6f8;color:#17212b;margin:0;padding:24px}
main{max-width:560px;margin:auto;background:white;padding:24px;border-radius:14px;box-shadow:0 4px 24px #0001}
h1{margin-top:0;font-size:1.4rem}.note{color:#52616f;font-size:.92rem;line-height:1.5}
input[type=file]{display:block;width:100%;margin:16px 0;font:inherit}
button{background:#1769aa;color:white;border:0;border-radius:8px;padding:13px 20px;font:inherit;font-weight:700;cursor:pointer}
button:disabled{background:#9aa7b3}
progress{width:100%;height:18px;margin-top:16px}#msg{margin-top:12px;font-weight:600}
.ok{color:#1b7f3b}.bad{color:#a4262c}a{color:#1769aa}
</style></head><body><main><h1>Обновление прошивки</h1>
<p class="note">Текущая версия: <b>)HTML";
  page += firmwareVersion_;
  page += R"HTML(</b>.<br>Файл: в Arduino IDE «Скетч → Экспорт скомпилированного бинарного файла», затем
<code>AIThinkerGuard.ino.bin</code> из папки <code>build</code> проекта. Не выбирайте
<code>*.bootloader.bin</code>, <code>*.partitions.bin</code> и <code>*.merged.bin</code>.</p>
<form id="f"><input type="file" id="file" accept=".bin" required>
<button id="go" type="submit">Загрузить и перезапустить</button></form>
<progress id="bar" max="100" value="0" hidden></progress><div id="msg"></div>
<p class="note">Во время записи камера не снимает. Если что-то пойдёт не так, остаётся
прежняя прошивка. <a href="/">← к настройкам</a></p>
<script>
const f=document.getElementById('f'),file=document.getElementById('file'),go=document.getElementById('go'),
bar=document.getElementById('bar'),msg=document.getElementById('msg');
function say(t,c){msg.textContent=t;msg.className=c||''}
f.onsubmit=e=>{e.preventDefault();const x=file.files[0];if(!x)return;
 if(!/\.bin$/i.test(x.name)){say('Нужен файл .bin','bad');return}
 if(/(bootloader|partitions|merged)/i.test(x.name)){say('Это не тот .bin — нужен AIThinkerGuard.ino.bin','bad');return}
 const d=new FormData();d.append('firmware',x,x.name);const r=new XMLHttpRequest();
 go.disabled=true;bar.hidden=false;bar.value=0;say('Загрузка…');
 r.upload.onprogress=p=>{if(p.lengthComputable){bar.value=p.loaded*100/p.total;
  say(p.loaded<p.total?'Загрузка… '+Math.round(bar.value)+'%':'Запись во flash…')}};
 r.onload=()=>{if(r.status==200){bar.value=100;say('Готово. Перезапуск, страница обновится через 25 с…','ok');
   setTimeout(()=>location.href='/',25000)}else{say('Ошибка: '+(r.responseText||r.status),'bad');go.disabled=false}};
 r.onerror=()=>{say('Связь прервалась. Если запись успела завершиться, устройство перезапустится само.','bad');go.disabled=false};
 r.open('POST','/update');r.send(d)};
</script></main></body></html>)HTML";
  server_.send(200, "text/html; charset=utf-8", page);
}

void WebConfigServer::handleUpdateUpload() {
  HTTPUpload &upload = server_.upload();
  feedWatchdog();

  if (upload.status == UPLOAD_FILE_START) {
    uploadStarted_ = true;
    uploadError_ = "";
    // Проверки доступа: ответ отправит handleUpdateDone(), здесь только не пишем.
    const bool authorized =
        (apProtected_ && requestFromAccessPoint()) ||
        (config_ != nullptr && config_->devicePassword.length() >= 8 &&
         server_.authenticate("admin", config_->devicePassword.c_str()));
    if (!authorized) {
      uploadError_ = "нет доступа";
      return;
    }
    if (!updateAllowed()) {
      uploadError_ = "обновление прошивки выключено в настройках";
      return;
    }
    Serial.printf("Веб-обновление: приём файла %s...\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      uploadError_ = String("Update.begin: ") + Update.errorString() +
                     " (схема разделов должна быть с OTA, например Minimal SPIFFS)";
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadError_.isEmpty()) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      uploadError_ = String("запись: ") + Update.errorString();
      Update.abort();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!uploadError_.isEmpty()) return;
    if (Update.end(true)) {
      Serial.printf("Веб-обновление: записано %u байт.\n", static_cast<unsigned>(upload.totalSize));
    } else {
      uploadError_ = String("проверка образа: ") + Update.errorString();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (Update.isRunning()) Update.abort();
    uploadError_ = "загрузка прервана";
  }
}

void WebConfigServer::handleUpdateDone() {
  if (!authenticate()) return;
  if (!uploadStarted_) uploadError_ = "файл не получен";
  uploadStarted_ = false;
  if (!uploadError_.isEmpty()) {
    Serial.println("Веб-обновление: ошибка — " + uploadError_);
    server_.send(uploadError_ == "нет доступа" ? 401 : 500, "text/plain; charset=utf-8", uploadError_);
    return;
  }
  Serial.println("Веб-обновление: успешно, перезапуск.");
  server_.send(200, "text/plain; charset=utf-8", "OK");
  delay(700);
  if (beforeRestart_) beforeRestart_();
  ESP.restart();
}

// ------------------------------------------------------------ меню ---

String WebConfigServer::menuHtml() const {
  return "<nav><a href=\"/update\">Обновить прошивку</a>"
         "<a href=\"/config.json\">Сохранить конфиг</a>"
         "<a href=\"/config\">Загрузить конфиг</a></nav>";
}

// --------------------------------------------- выгрузка/загрузка конфига ---
// JSON с плоскими ключами, как имена полей формы. При загрузке ключи, которых
// нет в файле, остаются как были, — можно хранить «общий» файл без лишнего.

void WebConfigServer::handleConfigExport() {
  if (!authenticate()) return;
  if (config_ == nullptr) return;
  DynamicJsonDocument doc(2048);
  doc["format"] = "ai-thinker-guard-config";
  doc["configVersion"] = 1;
  doc["firmware"] = firmwareVersion_;
  doc["device"] = deviceName_;
  doc["deviceName"] = config_->deviceName;
  doc["armed"] = config_->armed;
  doc["periodicEnabled"] = config_->periodicEnabled;
  doc["mqttHost"] = config_->mqttHost;
  doc["mqttPort"] = config_->mqttPort;
  doc["mqttUser"] = config_->mqttUser;
  doc["mqttPassword"] = config_->mqttPassword;
  doc["wifiSsid"] = config_->wifiSsid;
  doc["wifiPassword"] = config_->wifiPassword;
  doc["telegramToken"] = config_->telegramToken;
  doc["chatId"] = config_->chatId;
  doc["telegramApiHost"] = config_->telegramApiHost;
  doc["motionCount"] = config_->motionCount;
  doc["manualCount"] = config_->manualCount;
  doc["periodicMinutes"] = config_->periodicMinutes;
  doc["timeoutSeconds"] = config_->timeoutSeconds;
  doc["ownerIp"] = config_->ownerIp;
  doc["ownerAwayMinutes"] = config_->ownerAwayMinutes;
  doc["quietEnabled"] = config_->quietEnabled;
  doc["quietStart"] = config_->quietStart;
  doc["quietEnd"] = config_->quietEnd;
  doc["timezoneOffsetMinutes"] = config_->timezoneOffsetMinutes;
  doc["otaEnabled"] = config_->otaEnabled;
  // Пароль панели выгружается, только если задан вручную; иначе на другом
  // устройстве останется его собственный пароль по умолчанию.
  if (config_->devicePasswordCustom) doc["devicePassword"] = config_->devicePassword;
  String body;
  serializeJsonPretty(doc, body);
  server_.sendHeader("Content-Disposition",
                     "attachment; filename=\"" + deviceName_ + "-config.json\"");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json; charset=utf-8", body);
}

void WebConfigServer::handleConfigPage() {
  if (!authenticate()) return;
  String page = R"HTML(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Конфигурация</title><style>
body{font:16px system-ui,sans-serif;background:#f4f6f8;color:#17212b;margin:0;padding:24px}
main{max-width:560px;margin:auto;background:white;padding:24px;border-radius:14px;box-shadow:0 4px 24px #0001}
h1{margin-top:0;font-size:1.4rem}.note{color:#52616f;font-size:.92rem;line-height:1.5}
input[type=file]{display:block;width:100%;margin:16px 0;font:inherit}
button,.btn{display:inline-block;background:#1769aa;color:white;border:0;border-radius:8px;padding:13px 20px;font:inherit;font-weight:700;cursor:pointer;text-decoration:none}
button:disabled{background:#9aa7b3}.warn{background:#fff5d6;padding:12px;border-radius:8px}
#msg{margin-top:12px;font-weight:600}.ok{color:#1b7f3b}.bad{color:#a4262c}a{color:#1769aa}
</style></head><body><main><h1>Конфигурация</h1>
<p><a class="btn" href="/config.json">Сохранить конфиг в файл</a></p>
<p class="note warn">В файле есть пароль Wi-Fi и токен бота — храните его как пароль.</p>
<h2>Загрузить конфиг</h2>
<p class="note">Файл .json, сохранённый с этой или другой камеры. Поля, которых нет в файле, не меняются.
Имя устройства (<code>deviceName</code>) лучше удалить из общего файла — иначе все камеры получат одно имя.
После загрузки камера перезапустится.</p>
<form id="f"><input type="file" id="file" accept=".json,application/json" required>
<button id="go" type="submit">Загрузить и применить</button></form><div id="msg"></div>
<p class="note"><a href="/">← к настройкам</a></p>
<script>
const f=document.getElementById('f'),file=document.getElementById('file'),go=document.getElementById('go'),msg=document.getElementById('msg');
function say(t,c){msg.textContent=t;msg.className=c||''}
f.onsubmit=async e=>{e.preventDefault();const x=file.files[0];if(!x)return;
 if(x.size>4096){say('Файл слишком большой для конфига','bad');return}
 const text=await x.text();try{JSON.parse(text)}catch(_){say('Это не JSON','bad');return}
 go.disabled=true;say('Применяю…');
 try{const r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:text});
  const t=await r.text();
  if(r.ok){say('Готово. Перезапуск, страница обновится через 25 с…','ok');setTimeout(()=>location.href='/',25000)}
  else{say('Ошибка: '+t,'bad');go.disabled=false}}
 catch(_){say('Связь прервалась','bad');go.disabled=false}};
</script></main></body></html>)HTML";
  server_.send(200, "text/html; charset=utf-8", page);
}

void WebConfigServer::handleConfigImport() {
  if (!authenticate()) return;
  if (config_ == nullptr || store_ == nullptr) {
    server_.send(500, "text/plain; charset=utf-8", "Нет хранилища настроек");
    return;
  }
  const String body = server_.arg("plain");
  if (body.isEmpty() || body.length() > 4096) {
    server_.send(400, "text/plain; charset=utf-8", "Пустой или слишком большой файл.");
    return;
  }
  DynamicJsonDocument doc(3072);
  const DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server_.send(400, "text/plain; charset=utf-8", String("Ошибка JSON: ") + error.c_str());
    return;
  }
  if (!doc.is<JsonObject>()) {
    server_.send(400, "text/plain; charset=utf-8", "Ожидается JSON-объект.");
    return;
  }
  const char *format = doc["format"] | "";
  if (strlen(format) > 0 && strcmp(format, "ai-thinker-guard-config") != 0) {
    server_.send(400, "text/plain; charset=utf-8", "Это конфиг не от AI-Thinker Guard.");
    return;
  }

  AppConfig next = *config_;
  JsonObject in = doc.as<JsonObject>();
  auto text = [&](const char *key, String &target) {
    if (in.containsKey(key)) {
      target = in[key].as<String>();
      target.trim();
    }
  };
  auto integer = [&](const char *key, long &target) -> bool {
    if (!in.containsKey(key)) return true;
    if (!in[key].is<long>()) return false;
    target = in[key].as<long>();
    return true;
  };
  text("deviceName", next.deviceName);
  text("mqttHost", next.mqttHost);
  text("mqttUser", next.mqttUser);
  text("mqttPassword", next.mqttPassword);
  if (in.containsKey("armed")) next.armed = in["armed"].as<bool>();
  if (in.containsKey("periodicEnabled")) next.periodicEnabled = in["periodicEnabled"].as<bool>();
  {
    long port = next.mqttPort;
    if (!integer("mqttPort", port) || port < 1 || port > 65535) {
      server_.send(400, "text/plain; charset=utf-8", "mqttPort — число от 1 до 65535.");
      return;
    }
    next.mqttPort = static_cast<uint16_t>(port);
  }
  text("wifiSsid", next.wifiSsid);
  text("wifiPassword", next.wifiPassword);
  text("telegramToken", next.telegramToken);
  text("chatId", next.chatId);
  text("telegramApiHost", next.telegramApiHost);
  text("ownerIp", next.ownerIp);
  text("quietStart", next.quietStart);
  text("quietEnd", next.quietEnd);
  if (in.containsKey("quietEnabled")) next.quietEnabled = in["quietEnabled"].as<bool>();
  if (in.containsKey("otaEnabled")) next.otaEnabled = in["otaEnabled"].as<bool>();
  if (in.containsKey("devicePassword")) {
    next.devicePassword = in["devicePassword"].as<String>();
    next.devicePasswordCustom = true;
  }

  long motion = next.motionCount, manual = next.manualCount, periodic = next.periodicMinutes,
       timeout = next.timeoutSeconds, away = next.ownerAwayMinutes, tz = next.timezoneOffsetMinutes;
  if (!integer("motionCount", motion) || !integer("manualCount", manual) ||
      !integer("periodicMinutes", periodic) || !integer("timeoutSeconds", timeout) ||
      !integer("ownerAwayMinutes", away) || !integer("timezoneOffsetMinutes", tz)) {
    server_.send(400, "text/plain; charset=utf-8", "Числовые поля должны быть числами.");
    return;
  }
  next.motionCount = static_cast<uint8_t>(constrain(motion, 0, 255));
  next.manualCount = static_cast<uint8_t>(constrain(manual, 0, 255));
  next.periodicMinutes = static_cast<uint16_t>(constrain(periodic, 0, 65535));
  next.timeoutSeconds = static_cast<uint32_t>(constrain(timeout, 0, 100000));
  next.ownerAwayMinutes = static_cast<uint16_t>(constrain(away, 0, 65535));
  next.timezoneOffsetMinutes = static_cast<int16_t>(constrain(tz, -32768, 32767));

  Serial.println("Загрузка конфига через веб-панель.");
  storeAndRestart(next, "Конфиг применён. Перезапуск…");
}
