#pragma once

#include <Arduino.h>
#include "esp_camera.h"

class TelegramService {
 public:
  void configure(const String &botToken, const String &chatId);
  bool isConfigured() const;
  String configurationProblem() const;
  bool sendMessage(const String &text);
  bool sendPhoto(camera_fb_t *frame, const String &caption);
  bool pollCommand(int64_t &nextOffset, String &command);

 private:
  String botToken_;
  String chatId_;
  bool validBotToken() const;
  bool validChatId() const;
  String apiUrl(const String &method) const;
  bool writeAll(class WiFiClientSecure &client, const uint8_t *data, size_t length);
};