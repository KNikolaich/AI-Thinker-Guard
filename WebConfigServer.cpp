#include "WebConfigServer.h"
#include <ctype.h>
#include <stdlib.h>

static bool parseIntegerValue(String value, long &parsed) {
  value.trim();
  if (value.isEmpty()) return false;
  char *end = nullptr;
  parsed = strtol(value.c_str(), &end, 10);
  return end != value.c_str() && *end == '\0';
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

static bool isValidMacValue(String value) {
  value.trim();
  if (value.isEmpty()) return true;
  String hex;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (isxdigit(static_cast<unsigned char>(c))) hex += static_cast<char>(tolower(c));
    else if (c != ':' && c != '-') return false;
  }
  return hex.length() == 12 && hex != "000000000000" && hex != "ffffffffffff";
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

static bool isValidChatIdValue(const String &chatId) {
  if (chatId.isEmpty() || chatId.length() > 20) return false;
  const size_t firstDigit = chatId[0] == '-' ? 1 : 0;
  if (firstDigit == chatId.length()) return false;
  for (size_t i = firstDigit; i < chatId.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(chatId[i]))) return false;
  }
  return true;
}

void WebConfigServer::begin(ConfigStore &store, AppConfig &config) {
  store_ = &store;
  config_ = &config;
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { handleSave(); });
  server_.onNotFound([this]() {
    server_.sendHeader("Location", "/", true);
    server_.send(302, "text/plain", "");
  });
  server_.begin();
}

void WebConfigServer::handleClient() {
  server_.handleClient();
}

bool WebConfigServer::authenticate() {
  if (config_ == nullptr) return false;
  if (config_->devicePassword.length() >= 12 &&
      server_.authenticate("admin", config_->devicePassword.c_str())) return true;
  server_.requestAuthentication(BASIC_AUTH, "AI-Thinker Guard");
  return false;
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
                                 ? " minlength=\"12\" maxlength=\"63\""
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
.note{color:#52616f;font-size:.92rem;line-height:1.5}.warn{background:#fff5d6;padding:12px;border-radius:8px}
@media(max-width:600px){body{padding:10px}.row{grid-template-columns:1fr}}
</style></head><body><main><h1>AI-Thinker Guard</h1>
<p class="note">Настройки сохраняются во внутреннюю NVS-память. После сохранения камера перезапустится.</p>
<form method="post" action="/save">
<h2>Wi-Fi</h2>)HTML";
  page += textField("Имя сети (SSID)", "wifiSsid", config_->wifiSsid);
  page += passwordField("Пароль Wi-Fi", "wifiPassword");
  page += R"HTML(<h2>Telegram</h2>)HTML";
  page += passwordField("Токен бота", "telegramToken");
  page += textField("ID чата владельца", "chatId", config_->chatId);
  page += R"HTML(<p class="note">Бот должен быть запущен, а владелец должен написать ему первым. Используется только указанный chat ID.</p>
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
  page += R"HTML(</div><p class="note">Детектор сравнивает последовательные монохромные кадры QQVGA. Снимки отправляются в VGA.</p>
<h2>Bluetooth и расписание тишины</h2>)HTML";
  page += textField("MAC BLE-устройства капитана (пусто — отключено)", "ownerMac",
                    config_->ownerMac);
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
  page += passwordField("Новый общий пароль устройства (минимум 12 символов)", "devicePassword");
  page += R"HTML(<label class="check"><input type="checkbox" name="otaEnabled" value="1")HTML";
  if (config_->otaEnabled) page += " checked";
  page += R"HTML(> Разрешить OTA-прошивку с этим же паролем</label>
<p class="note warn">Один пароль используется для сети настройки, веб-страницы admin и OTA. OTA по умолчанию выключена. Пароль не отображается на странице; сохраните его отдельно.</p>
<button type="submit">Сохранить и перезапустить</button></form></main></body></html>)HTML";
  server_.send(200, "text/html; charset=utf-8", page);
}

void WebConfigServer::handleSave() {
  if (!authenticate()) return;
  if (config_ == nullptr || store_ == nullptr) {
    server_.send(500, "text/plain; charset=utf-8", "Нет хранилища настроек");
    return;
  }

  AppConfig next = *config_;
  if (server_.hasArg("wifiSsid")) next.wifiSsid = server_.arg("wifiSsid");
  if (server_.hasArg("chatId")) next.chatId = server_.arg("chatId");
  if (server_.hasArg("ownerMac")) next.ownerMac = server_.arg("ownerMac");
  if (server_.hasArg("quietStart")) next.quietStart = server_.arg("quietStart");
  if (server_.hasArg("quietEnd")) next.quietEnd = server_.arg("quietEnd");

  if (server_.hasArg("wifiPassword") && server_.arg("wifiPassword").length())
    next.wifiPassword = server_.arg("wifiPassword");
  if (server_.hasArg("telegramToken") && server_.arg("telegramToken").length())
    next.telegramToken = server_.arg("telegramToken");

  if (next.wifiSsid.length() > 32 ||
      (next.wifiPassword.length() > 0 &&
       (next.wifiPassword.length() < 8 || next.wifiPassword.length() > 64))) {
    server_.send(400, "text/plain; charset=utf-8",
                 "Проверьте Wi-Fi: SSID до 32 символов, пароль 8–64 символа.");
    return;
  }
  if (!next.telegramToken.isEmpty() &&
      (next.telegramToken.length() > 128 || !isValidBotTokenValue(next.telegramToken))) {
    server_.send(400, "text/plain; charset=utf-8",
                 "Токен Telegram имеет неверный формат. Он должен содержать числовой ID и двоеточие.");
    return;
  }
  if (!next.chatId.isEmpty() && !isValidChatIdValue(next.chatId)) {
    server_.send(400, "text/plain; charset=utf-8",
                 "chat ID должен быть числом, например отрицательным ID группы.");
    return;
  }
  if (!isValidMacValue(next.ownerMac)) {
    server_.send(400, "text/plain; charset=utf-8",
                 "MAC должен содержать 12 шестнадцатеричных цифр; оставьте поле пустым, чтобы отключить BLE.");
    return;
  }

  const String requestedDevicePassword = server_.arg("devicePassword");
  if (requestedDevicePassword.length() > 0) {
    if (requestedDevicePassword.length() < 12 || requestedDevicePassword.length() > 63) {
      server_.send(400, "text/plain; charset=utf-8",
                   "Пароль устройства должен содержать от 12 до 63 символов.");
      return;
    }
    next.devicePassword = requestedDevicePassword;
  }
  next.otaEnabled = server_.hasArg("otaEnabled");

  next.quietEnabled = server_.hasArg("quietEnabled");
  long number = 0;
  if (!parseIntegerValue(server_.arg("motionCount"), number) || number < 1 || number > 5) {
    server_.send(400, "text/plain; charset=utf-8", "N должно быть от 1 до 5.");
    return;
  }
  next.motionCount = static_cast<uint8_t>(number);
  if (!parseIntegerValue(server_.arg("manualCount"), number) || number < 1 || number > 5) {
    server_.send(400, "text/plain; charset=utf-8", "M должно быть от 1 до 5.");
    return;
  }
  next.manualCount = static_cast<uint8_t>(number);
  if (!parseIntegerValue(server_.arg("periodicMinutes"), number) ||
      number < 1 || number > 1440) {
    server_.send(400, "text/plain; charset=utf-8", "X должно быть от 1 до 1440 минут.");
    return;
  }
  next.periodicMinutes = static_cast<uint16_t>(number);
  if (!parseIntegerValue(server_.arg("timeoutSeconds"), number) ||
      number < 1 || number > 86400) {
    server_.send(400, "text/plain; charset=utf-8", "TO должно быть от 1 до 86400 секунд.");
    return;
  }
  next.timeoutSeconds = static_cast<uint32_t>(number);
  if (!parseIntegerValue(server_.arg("timezoneOffsetMinutes"), number) ||
      number < -720 || number > 840) {
    server_.send(400, "text/plain; charset=utf-8", "Часовой пояс должен быть в диапазоне от -720 до 840 минут.");
    return;
  }
  next.timezoneOffsetMinutes = static_cast<int16_t>(number);
  if (next.quietEnabled &&
      (!isValidClockValue(next.quietStart) || !isValidClockValue(next.quietEnd) ||
       next.quietStart == next.quietEnd)) {
    server_.send(400, "text/plain; charset=utf-8",
                 "Для тихих часов задайте разные значения времени в формате ЧЧ:ММ.");
    return;
  }

  if (!store_->save(next)) {
    server_.send(500, "text/plain; charset=utf-8", "Не удалось сохранить настройки");
    return;
  }
  server_.send(200, "text/html; charset=utf-8",
               "<!doctype html><meta charset=\"utf-8\"><p>Настройки сохранены. Перезапуск…</p>");
  delay(700);
  ESP.restart();
}