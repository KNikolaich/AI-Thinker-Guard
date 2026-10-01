#include "TelegramService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include "SystemHealth.h"

// Все таймауты WiFiClientSecure::setTimeout() в Arduino-ESP32 2.0.x задаются
// в СЕКУНДАХ; HTTPClient::setTimeout() — в миллисекундах.
static const uint32_t TLS_TIMEOUT_S = 12;
static const uint16_t HTTP_TIMEOUT_MS = 12000;

static void prepareTls(WiFiClientSecure &tls) {
  tls.setInsecure();
  tls.setTimeout(TLS_TIMEOUT_S);
  tls.setHandshakeTimeout(TLS_TIMEOUT_S);
}

static bool isValidSingleChatId(const String &chatId) {
  if (chatId.isEmpty() || chatId.length() > 20) return false;
  const size_t firstDigit = chatId[0] == '-' ? 1 : 0;
  if (firstDigit == chatId.length()) return false;
  for (size_t i = firstDigit; i < chatId.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(chatId[i]))) return false;
  }
  return true;
}

bool TelegramService::parseChatIds(const String &value, String *ids, uint8_t &count) {
  count = 0;
  int start = 0;
  const int length = value.length();
  while (start <= length) {
    int comma = value.indexOf(',', start);
    if (comma < 0) comma = length;
    String item = value.substring(start, comma);
    item.trim();
    if (!item.isEmpty()) {
      if (!isValidSingleChatId(item) || count >= MAX_CHATS) return false;
      bool duplicate = false;
      for (uint8_t i = 0; i < count; ++i) duplicate |= ids[i] == item;
      if (!duplicate) ids[count++] = item;
    }
    start = comma + 1;
  }
  return true;
}

void TelegramService::configure(const String &botToken, const String &chatIds,
                                const String &apiEndpoint) {
  botToken_ = botToken;
  botToken_.trim();
  chatListValid_ = parseChatIds(chatIds, chatIds_, chatCount_);
  if (!chatListValid_) chatCount_ = 0;
  if (!parseApiEndpoint(apiEndpoint, apiHost_, apiPort_, apiPrefix_)) {
    Serial.println("Telegram: неверный адрес API, использую api.telegram.org");
    parseApiEndpoint("", apiHost_, apiPort_, apiPrefix_);
  }
}

bool TelegramService::parseApiEndpoint(String value, String &host, uint16_t &port,
                                       String &prefix) {
  value.trim();
  if (value.startsWith("https://")) value.remove(0, 8);
  while (value.endsWith("/")) value.remove(value.length() - 1);
  if (value.isEmpty()) {
    host = "api.telegram.org";
    port = 443;
    prefix = "";
    return true;
  }
  if (value.length() > 128) return false;

  String hostPart = value;
  String pathPart;
  const int slash = value.indexOf('/');
  if (slash >= 0) {
    hostPart = value.substring(0, slash);
    pathPart = value.substring(slash);  // начинается с '/'
  }
  uint16_t parsedPort = 443;
  const int colon = hostPart.indexOf(':');
  if (colon >= 0) {
    const String portText = hostPart.substring(colon + 1);
    hostPart = hostPart.substring(0, colon);
    if (portText.isEmpty() || portText.length() > 5) return false;
    for (size_t i = 0; i < portText.length(); ++i) {
      if (!isdigit(static_cast<unsigned char>(portText[i]))) return false;
    }
    const long p = portText.toInt();
    if (p < 1 || p > 65535) return false;
    parsedPort = static_cast<uint16_t>(p);
  }
  if (hostPart.isEmpty() || hostPart.length() > 63) return false;
  for (size_t i = 0; i < hostPart.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(hostPart[i]);
    if (!isalnum(c) && c != '.' && c != '-') return false;
  }
  for (size_t i = 0; i < pathPart.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(pathPart[i]);
    if (!isalnum(c) && c != '/' && c != '-' && c != '_' && c != '.' && c != '~') return false;
  }
  host = hostPart;
  port = parsedPort;
  prefix = pathPart;
  return true;
}

String TelegramService::endpointText() const {
  String text = apiHost_;
  if (apiPort_ != 443) text += ":" + String(apiPort_);
  return text + apiPrefix_;
}

bool TelegramService::isConfigured() const {
  return validBotToken() && chatCount_ > 0;
}

bool TelegramService::validBotToken() const {
  const int separator = botToken_.indexOf(':');
  if (separator <= 0 || separator >= static_cast<int>(botToken_.length()) - 1) return false;
  for (int i = 0; i < separator; ++i) {
    if (!isdigit(static_cast<unsigned char>(botToken_[i]))) return false;
  }
  for (size_t i = separator + 1; i < botToken_.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(botToken_[i]);
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

String TelegramService::configurationProblem() const {
  if (botToken_.isEmpty()) return "не задан токен бота";
  if (!validBotToken()) return "неверный формат токена бота";
  if (!chatListValid_) return "неверный список chat ID (числа через запятую, до 5)";
  if (chatCount_ == 0) return "не задан ни один chat ID";
  return "";
}

bool TelegramService::isKnownChat(const String &chatId) const {
  for (uint8_t i = 0; i < chatCount_; ++i) {
    if (chatIds_[i] == chatId) return true;
  }
  return false;
}

String TelegramService::apiUrl(const String &method) const {
  return "https://" + apiHost_ + (apiPort_ != 443 ? ":" + String(apiPort_) : String("")) +
         apiPrefix_ + "/bot" + botToken_ + "/" + method;
}

void TelegramService::noteResult(int status, const char *operation) {
  lastStatus_ = status;
  lastOperation_ = operation;
  lastResultMs_ = millis();
  if (status == 200) lastSuccessMs_ = lastResultMs_;
}

String TelegramService::lastResult() const {
  if (lastResultMs_ == 0) return "запросов ещё не было";
  String text = lastOperation_ + ": ";
  if (lastStatus_ == 200) text += "OK";
  else if (lastStatus_ == 401 || lastStatus_ == 404) text += "HTTP " + String(lastStatus_) + " (проверьте токен)";
  else if (lastStatus_ == 400 || lastStatus_ == 403) text += "HTTP " + String(lastStatus_) + " (проверьте chat ID и что бот запущен)";
  else if (lastStatus_ > 0) text += "HTTP " + String(lastStatus_);
  else text += "ошибка соединения (" + String(lastStatus_) + ")";
  text += ", " + String((millis() - lastResultMs_) / 1000) + " с назад";
  if (lastSuccessMs_ != 0 && lastStatus_ != 200) {
    text += "; последний успех " + String((millis() - lastSuccessMs_) / 1000) + " с назад";
  }
  return text;
}

bool TelegramService::postJson(const char *method, const String &body, const char *operation) {
  if (!validBotToken() || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure tls;
  prepareTls(tls);
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(tls, apiUrl(method))) {
    noteResult(-1, operation);
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(body);
  http.end();
  noteResult(status, operation);
  feedWatchdog();
  return status == 200;
}

bool TelegramService::sendMessageTo(const String &chatId, const String &text,
                                    const String &keyboardJson) {
  DynamicJsonDocument payload(768 + text.length() * 2 + keyboardJson.length());
  payload["chat_id"] = chatId;
  payload["text"] = text;
  if (!keyboardJson.isEmpty()) payload["reply_markup"] = serialized(keyboardJson);
  String body;
  serializeJson(payload, body);
  return postJson("sendMessage", body, "sendMessage");
}

bool TelegramService::sendMessage(const String &text) {
  if (!isConfigured()) return false;
  bool any = false;
  for (uint8_t i = 0; i < chatCount_; ++i) any |= sendMessageTo(chatIds_[i], text);
  return any;
}

void TelegramService::answerCallback(const String &callbackId, const String &text) {
  if (callbackId.isEmpty()) return;
  StaticJsonDocument<256> payload;
  payload["callback_query_id"] = callbackId;
  if (!text.isEmpty()) payload["text"] = text;
  String body;
  serializeJson(payload, body);
  postJson("answerCallbackQuery", body, "answerCallbackQuery");
}

bool TelegramService::publishCommands() {
  const String body =
      "{\"commands\":["
      "{\"command\":\"getcapture\",\"description\":\"Сделать снимок сейчас\"},"
      "{\"command\":\"arm\",\"description\":\"Поставить на охрану\"},"
      "{\"command\":\"disarm\",\"description\":\"Снять с охраны\"},"
      "{\"command\":\"periodic\",\"description\":\"Плановые снимки: on или off\"},"
      "{\"command\":\"status\",\"description\":\"Состояние камеры\"},"
      "{\"command\":\"help\",\"description\":\"Что умеет камера\"}]}";
  return postJson("setMyCommands", body, "setMyCommands");
}

bool TelegramService::writeAll(WiFiClientSecure &client, const uint8_t *data, size_t length) {
  size_t sent = 0;
  uint32_t lastProgressMs = millis();
  while (sent < length && millis() - lastProgressMs < TLS_TIMEOUT_S * 1000UL) {
    if (!client.connected()) return false;
    const size_t chunk = length - sent > 4096 ? 4096 : length - sent;
    const size_t written = client.write(data + sent, chunk);
    feedWatchdog();
    if (written == 0) {
      delay(5);
      continue;
    }
    sent += written;
    lastProgressMs = millis();
  }
  return sent == length;
}

// Загружает JPEG в чат multipart-запросом и достаёт file_id из ответа, чтобы
// разослать тот же снимок другим чатам без повторной загрузки.
bool TelegramService::uploadPhoto(const String &chatId, camera_fb_t *frame, const String &caption,
                                  String &fileId) {
  fileId = "";
  if (!validBotToken() || WiFi.status() != WL_CONNECTED || frame == nullptr ||
      frame->format != PIXFORMAT_JPEG) return false;

  WiFiClientSecure tls;
  prepareTls(tls);
  if (!tls.connect(apiHost_.c_str(), apiPort_)) {
    noteResult(-1, "sendPhoto");
    return false;
  }
  feedWatchdog();

  const String boundary = "----ESP32GuardBoundary7MA4YWxk";
  const String prefix =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
      chatId + "\r\n--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
      caption + "\r\n--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"photo\"; filename=\"capture.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";
  const String suffix = "\r\n--" + boundary + "--\r\n";
  const size_t contentLength = prefix.length() + frame->len + suffix.length();

  String header = "POST " + apiPrefix_ + "/bot" + botToken_ + "/sendPhoto HTTP/1.1\r\n"
                  "Host: " + apiHost_ + "\r\n"
                  "Connection: close\r\n"
                  "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
                  "Content-Length: " + String(contentLength) + "\r\n\r\n";

  const bool requestWritten =
      writeAll(tls, reinterpret_cast<const uint8_t *>(header.c_str()), header.length()) &&
      writeAll(tls, reinterpret_cast<const uint8_t *>(prefix.c_str()), prefix.length()) &&
      writeAll(tls, frame->buf, frame->len) &&
      writeAll(tls, reinterpret_cast<const uint8_t *>(suffix.c_str()), suffix.length());
  if (!requestWritten) {
    tls.stop();
    noteResult(-2, "sendPhoto");
    return false;
  }

  uint32_t startedAt = millis();
  while (!tls.available() && tls.connected() && millis() - startedAt < TLS_TIMEOUT_S * 1000UL) {
    feedWatchdog();
    delay(10);
  }
  // Строка статуса: "HTTP/1.1 200 OK"
  const String statusLine = tls.readStringUntil('\n');
  int status = -3;
  const int space = statusLine.indexOf(' ');
  if (space > 0) status = statusLine.substring(space + 1, space + 4).toInt();

  // Остаток ответа (заголовки + JSON, обычно 1–2 КБ) — ради file_id.
  String response;
  startedAt = millis();
  while ((tls.connected() || tls.available()) && response.length() < 6144 &&
         millis() - startedAt < TLS_TIMEOUT_S * 1000UL) {
    while (tls.available() && response.length() < 6144) response += static_cast<char>(tls.read());
    feedWatchdog();
    delay(5);
  }
  tls.stop();
  noteResult(status, "sendPhoto");
  if (status != 200) return false;

  // В массиве photo размеры идут по возрастанию — последний file_id самый крупный.
  const String key = "\"file_id\":\"";
  const int at = response.lastIndexOf(key);
  if (at >= 0) {
    const int from = at + key.length();
    const int to = response.indexOf('"', from);
    if (to > from) fileId = response.substring(from, to);
  }
  return true;
}

bool TelegramService::sendPhotoById(const String &chatId, const String &fileId,
                                    const String &caption) {
  DynamicJsonDocument payload(512 + caption.length() * 2 + fileId.length());
  payload["chat_id"] = chatId;
  payload["photo"] = fileId;
  payload["caption"] = caption;
  String body;
  serializeJson(payload, body);
  return postJson("sendPhoto", body, "sendPhoto");
}

bool TelegramService::sendPhotoTo(const String &chatId, camera_fb_t *frame, const String &caption) {
  String fileId;
  return uploadPhoto(chatId, frame, caption, fileId);
}

bool TelegramService::sendPhoto(camera_fb_t *frame, const String &caption) {
  if (!isConfigured()) return false;
  String fileId;
  bool any = false;
  uint8_t i = 0;
  // Загружаем в первый чат, который примет снимок; остальным — по file_id.
  for (; i < chatCount_ && !any; ++i) any = uploadPhoto(chatIds_[i], frame, caption, fileId);
  for (; i < chatCount_; ++i) {
    if (fileId.isEmpty() || !sendPhotoById(chatIds_[i], fileId, caption)) {
      uploadPhoto(chatIds_[i], frame, caption, fileId);
    }
  }
  return any;
}

// Возвращает id первого обновления из «сырого» ответа, если JSON разобрать не удалось.
static bool findFirstUpdateId(const String &payload, int64_t &updateId) {
  const int key = payload.indexOf("\"update_id\":");
  if (key < 0) return false;
  const char *cursor = payload.c_str() + key + 12;
  char *end = nullptr;
  const long long value = strtoll(cursor, &end, 10);
  if (end == cursor) return false;
  updateId = value;
  return true;
}

bool TelegramService::pollCommand(int64_t &nextOffset, TelegramCommand &command) {
  command = TelegramCommand();
  if (!isConfigured() || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure tls;
  prepareTls(tls);
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  const String url = apiUrl("getUpdates") + "?offset=" + String(static_cast<long long>(nextOffset)) +
                     "&limit=5&timeout=0&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D";
  if (!http.begin(tls, url)) {
    noteResult(-1, "getUpdates");
    return false;
  }
  const int status = http.GET();
  noteResult(status, "getUpdates");
  if (status != 200) {
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();
  feedWatchdog();

  // Фильтр оставляет только нужные поля: длинные сообщения, фото и т.п.
  // не переполняют документ и не «застревают» в очереди навсегда.
  StaticJsonDocument<384> filter;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["message"]["chat"]["id"] = true;
  filter["result"][0]["message"]["text"] = true;
  filter["result"][0]["callback_query"]["id"] = true;
  filter["result"][0]["callback_query"]["data"] = true;
  filter["result"][0]["callback_query"]["message"]["chat"]["id"] = true;

  DynamicJsonDocument response(6144);
  const DeserializationError error =
      deserializeJson(response, payload, DeserializationOption::Filter(filter));
  if (error) {
    int64_t brokenId = 0;
    if (findFirstUpdateId(payload, brokenId) && brokenId >= nextOffset) {
      Serial.printf("Telegram: не удалось разобрать обновление %lld (%s), пропускаю его.\n",
                    static_cast<long long>(brokenId), error.c_str());
      nextOffset = brokenId + 1;
    }
    return false;
  }

  JsonArray updates = response["result"].as<JsonArray>();
  for (JsonObject update : updates) {
    const int64_t updateId = update["update_id"] | static_cast<int64_t>(0);
    if (updateId >= nextOffset) nextOffset = updateId + 1;

    String text;
    long long numericChatId = 0;
    String callbackId;
    JsonObject callback = update["callback_query"];
    JsonObject message = update["message"];
    if (!callback.isNull()) {
      callbackId = callback["id"].as<String>();
      text = callback["data"].as<String>();
      numericChatId = callback["message"]["chat"]["id"] | static_cast<long long>(0);
    } else if (!message.isNull() && !message["text"].isNull()) {
      text = message["text"].as<String>();
      numericChatId = message["chat"]["id"] | static_cast<long long>(0);
    } else {
      continue;
    }
    char chatIdBuffer[24];
    snprintf(chatIdBuffer, sizeof(chatIdBuffer), "%lld", numericChatId);
    const String chatId(chatIdBuffer);
    if (!isKnownChat(chatId)) {
      if (lastUnknownChat_ != chatId) {
        Serial.println("Telegram: сообщение из чата " + chatId +
                       ", которого нет в настройках, — игнорирую. Добавьте его ID в панель, если это вы.");
      }
      lastUnknownChat_ = chatId;
      if (!callbackId.isEmpty()) answerCallback(callbackId);
      continue;
    }

    text.trim();
    if (text.startsWith("/")) text.remove(0, 1);
    const int mentionAt = text.indexOf('@');
    if (mentionAt >= 0) text = text.substring(0, mentionAt);
    // «/arm скворечник» -> команда «arm», аргумент «скворечник».
    String arg;
    const int spaceAt = text.indexOf(' ');
    if (spaceAt >= 0) {
      arg = text.substring(spaceAt + 1);
      arg.trim();
      text = text.substring(0, spaceAt);
    }
    text.toLowerCase();
    // Кнопки присылают «periodic_on» и т.п.
    if (text == "periodic_on") { text = "periodic"; arg = "on"; }
    if (text == "periodic_off") { text = "periodic"; arg = "off"; }

    command.chatId = chatId;
    command.callbackId = callbackId;
    command.arg = arg;
    if (text == "getcapture" || text == "capture" || text == "photo") command.name = "getcapture";
    else if (text == "status") command.name = "status";
    else if (text == "start") command.name = "start";
    else if (text == "help") command.name = "help";
    else if (text == "arm") command.name = "arm";
    else if (text == "disarm") command.name = "disarm";
    else if (text == "periodic") command.name = "periodic";
    else command.name = "unknown";
    return true;
  }
  return false;
}
