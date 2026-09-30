#include "WebConfigServer.h"
#include <WiFi.h>
#include <Update.h>
#include "SystemHealth.h"
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
  server_.on("/retry", HTTP_POST, [this]() { handleRetry(); });
  server_.on("/update", HTTP_GET, [this]() { handleUpdatePage(); });
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
.note{color:#52616f;font-size:.92rem;line-height:1.5}.warn{background:#fff5d6;padding:12px;border-radius:8px}
@media(max-width:600px){body{padding:10px}.row{grid-template-columns:1fr}}
</style></head><body><main><h1>AI-Thinker Guard</h1>
<p class="note">Настройки сохраняются во внутреннюю NVS-память. После сохранения камера перезапустится.</p>)HTML";
  page += statusHtml();
  page += R"HTML(<form method="post" action="/save">
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
  page += passwordField("Новый общий пароль устройства (8–63 символа)", "devicePassword");
  page += R"HTML(<label class="check"><input type="checkbox" name="otaEnabled" value="1")HTML";
  if (config_->otaEnabled) page += " checked";
  page += R"HTML(> Разрешить обновление прошивки (веб-страница /update и Arduino OTA) с этим же паролем</label>
<p class="note warn">Один пароль используется для сети настройки, веб-страницы admin и OTA. OTA по умолчанию выключена. Пароль не отображается на странице; сохраните его отдельно.</p>
<button type="submit">Сохранить и перезапустить</button></form>)HTML";
  if (config_->otaEnabled) {
    page += R"HTML(<p><a href="/update">Обновить прошивку…</a></p>)HTML";
  }
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
    if (!isValidDevicePassword(requestedDevicePassword)) {
      server_.send(400, "text/plain; charset=utf-8",
                   "Пароль устройства: 8–63 символа, только латиница, цифры и знаки (без кириллицы и пробелов по краям).");
      return;
    }
    next.devicePassword = requestedDevicePassword;
    next.devicePasswordCustom = true;
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

  if (!store_->ready()) {
    server_.send(500, "text/plain; charset=utf-8",
                 "Память настроек (NVS) недоступна: сохранить нельзя. Подробности — в Serial Monitor.");
    return;
  }
  if (!store_->save(next)) {
    server_.send(500, "text/plain; charset=utf-8", "Не удалось сохранить настройки");
    return;
  }
  restartSoon("Настройки сохранены. Перезапуск…");
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
