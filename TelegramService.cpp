#include "TelegramService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>

void TelegramService::configure(const String &botToken, const String &chatId) {
  botToken_ = botToken;
  chatId_ = chatId;
}

bool TelegramService::isConfigured() const {
  return botToken_.length() > 0 && chatId_.length() > 0;
}

String TelegramService::apiUrl(const String &method) const {
  return "https://api.telegram.org/bot" + botToken_ + "/" + method;
}

bool TelegramService::sendMessage(const String &text) {
  if (!isConfigured() || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, apiUrl("sendMessage"))) return false;
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<768> payload;
  payload["chat_id"] = chatId_;
  payload["text"] = text;
  String body;
  serializeJson(payload, body);
  const int status = http.POST(body);
  http.end();
  return status == 200;
}

bool TelegramService::writeAll(WiFiClientSecure &client, const uint8_t *data, size_t length) {
  size_t sent = 0;
  const uint32_t startedAt = millis();
  while (sent < length && millis() - startedAt < 15000) {
    const size_t written = client.write(data + sent, length - sent);
    if (written == 0) {
      delay(2);
      continue;
    }
    sent += written;
  }
  return sent == length;
}

bool TelegramService::sendPhoto(camera_fb_t *frame, const String &caption) {
  if (!isConfigured() || WiFi.status() != WL_CONNECTED || frame == nullptr ||
      frame->format != PIXFORMAT_JPEG) return false;

  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(15000);
  if (!tls.connect("api.telegram.org", 443)) return false;

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

  tls.print("POST /bot");
  tls.print(botToken_);
  tls.println("/sendPhoto HTTP/1.1");
  tls.println("Host: api.telegram.org");
  tls.println("Connection: close");
  tls.print("Content-Type: multipart/form-data; boundary=");
  tls.println(boundary);
  tls.print("Content-Length: ");
  tls.println(contentLength);
  tls.println();

  bool requestWritten =
      writeAll(tls, reinterpret_cast<const uint8_t *>(prefix.c_str()), prefix.length()) &&
      writeAll(tls, frame->buf, frame->len) &&
      writeAll(tls, reinterpret_cast<const uint8_t *>(suffix.c_str()), suffix.length());
  if (!requestWritten) {
    tls.stop();
    return false;
  }

  const uint32_t startedAt = millis();
  while (!tls.available() && tls.connected() && millis() - startedAt < 15000) delay(10);
  const String statusLine = tls.readStringUntil('\n');
  const bool ok = statusLine.indexOf(" 200 ") >= 0;
  tls.stop();
  return ok;
}

bool TelegramService::pollCommand(int64_t &nextOffset, String &command) {
  command = "";
  if (!isConfigured() || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  const String url = apiUrl("getUpdates") + "?offset=" + String(static_cast<long long>(nextOffset)) +
                     "&limit=5&timeout=0";
  if (!http.begin(tls, url)) return false;
  const int status = http.GET();
  if (status != 200) {
    http.end();
    return false;
  }

  DynamicJsonDocument response(8192);
  const DeserializationError error = deserializeJson(response, http.getString());
  http.end();
  if (error) return false;

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