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

void TelegramService::configure(const String &botToken, const String &chatId) {
  botToken_ = botToken;
  chatId_ = chatId;
  botToken_.trim();
  chatId_.trim();
}

bool TelegramService::isConfigured() const {
  return validBotToken() && validChatId();
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

bool TelegramService::validChatId() const {
  if (chatId_.isEmpty()) return false;
  const size_t firstDigit = chatId_[0] == '-' ? 1 : 0;
  if (firstDigit == chatId_.length()) return false;
  for (size_t i = firstDigit; i < chatId_.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(chatId_[i]))) return false;
  }
  return true;
}

String TelegramService::configurationProblem() const {
  if (botToken_.isEmpty()) return "не задан токен бота";
  if (!validBotToken()) return "неверный формат токена бота";
  if (chatId_.isEmpty()) return "не задан числовой chat ID";
  if (!validChatId()) return "chat ID должен быть числовым";
  return "";
}

String TelegramService::apiUrl(const String &method) const {
  return "https://api.telegram.org/bot" + botToken_ + "/" + method;
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

bool TelegramService::sendMessage(const String &text) {
  if (!isConfigured() || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure tls;
  prepareTls(tls);
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(tls, apiUrl("sendMessage"))) {
    noteResult(-1, "sendMessage");
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument payload(512 + text.length() * 2);
  payload["chat_id"] = chatId_;
  payload["text"] = text;
  String body;
  serializeJson(payload, body);
  const int status = http.POST(body);
  http.end();
  noteResult(status, "sendMessage");
  return status == 200;
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

bool TelegramService::sendPhoto(camera_fb_t *frame, const String &caption) {
  if (!isConfigured() || WiFi.status() != WL_CONNECTED || frame == nullptr ||
      frame->format != PIXFORMAT_JPEG) return false;

  WiFiClientSecure tls;
  prepareTls(tls);
  if (!tls.connect("api.telegram.org", 443)) {
    noteResult(-1, "sendPhoto");
    return false;
  }
  feedWatchdog();

  const String boundary = "----ESP32GuardBoundary7MA4YWxk";
  const String prefix =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
      chatId_ + "\r\n--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
      caption + "\r\n--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"photo\"; filename=\"capture.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";
  const String suffix = "\r\n--" + boundary + "--\r\n";
  const size_t contentLength = prefix.length() + frame->len + suffix.length();

  String header = "POST /bot" + botToken_ + "/sendPhoto HTTP/1.1\r\n"
                  "Host: api.telegram.org\r\n"
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

  const uint32_t startedAt = millis();
  while (!tls.available() && tls.connected() && millis() - startedAt < TLS_TIMEOUT_S * 1000UL) {
    feedWatchdog();
    delay(10);
  }
  // Строка статуса: "HTTP/1.1 200 OK"
  const String statusLine = tls.readStringUntil('\n');
  tls.stop();
  int status = -3;
  const int space = statusLine.indexOf(' ');
  if (space > 0) status = statusLine.substring(space + 1, space + 4).toInt();
  noteResult(status, "sendPhoto");
  return status == 200;
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

bool TelegramService::pollCommand(int64_t &nextOffset, String &command) {
  command = "";
  if (!isConfigured() || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure tls;
  prepareTls(tls);
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  const String url = apiUrl("getUpdates") + "?offset=" + String(static_cast<long long>(nextOffset)) +
                     "&limit=5&timeout=0&allowed_updates=%5B%22message%22%5D";
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
  // больше не переполняют документ и не «застревают» в очереди навсегда.
  StaticJsonDocument<192> filter;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["message"]["chat"]["id"] = true;
  filter["result"][0]["message"]["text"] = true;

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

    JsonObject message = update["message"];
    if (message.isNull() || message["text"].isNull()) continue;
    const long long numericChatId = message["chat"]["id"] | static_cast<long long>(0);
    char chatIdBuffer[24];
    snprintf(chatIdBuffer, sizeof(chatIdBuffer), "%lld", numericChatId);
    if (chatId_ != String(chatIdBuffer)) continue;

    String text = message["text"].as<String>();
    text.trim();
    if (text.startsWith("/")) text.remove(0, 1);
    const int mentionAt = text.indexOf('@');
    if (mentionAt >= 0) text = text.substring(0, mentionAt);
    text.toLowerCase();
    if (text == "getcapture") {
      command = text;
      return true;
    }
  }
  return false;
}
