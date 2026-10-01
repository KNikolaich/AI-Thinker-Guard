#pragma once

#include <Arduino.h>
#include "esp_camera.h"

#include "DeviceCommand.h"

using TelegramCommand = DeviceCommand;

class TelegramService {
 public:
  static const uint8_t MAX_CHATS = 5;

  // chatIds — один или несколько ID через запятую.
  void configure(const String &botToken, const String &chatIds, const String &apiEndpoint = "");
  // Разбирает "host[:port][/префикс]" (можно с https://). Пусто — api.telegram.org.
  static bool parseApiEndpoint(String value, String &host, uint16_t &port, String &prefix);
  // Разбирает список ID чатов через запятую; false — если есть неверный ID.
  static bool parseChatIds(const String &value, String *ids, uint8_t &count);
  String endpointText() const;
  bool isConfigured() const;
  String configurationProblem() const;
  uint8_t chatCount() const { return chatCount_; }

  // Всем подписанным чатам.
  bool sendMessage(const String &text);
  bool sendPhoto(camera_fb_t *frame, const String &caption);
  // Одному чату; keyboardJson — готовый reply_markup (inline-кнопки) или "".
  bool sendMessageTo(const String &chatId, const String &text, const String &keyboardJson = "");
  bool sendPhotoTo(const String &chatId, camera_fb_t *frame, const String &caption);
  void answerCallback(const String &callbackId, const String &text = "");
  // Меню команд бота (кнопка «/» в Telegram). Вызывается один раз после старта.
  bool publishCommands();

  bool pollCommand(int64_t &nextOffset, TelegramCommand &command);
  String lastResult() const;
  bool lastRequestOk() const { return lastStatus_ == 200; }
  // ID последнего чата, который писал боту, но не указан в настройках.
  const String &lastUnknownChat() const { return lastUnknownChat_; }

 private:
  void noteResult(int status, const char *operation);
  bool postJson(const char *method, const String &body, const char *operation);
  bool uploadPhoto(const String &chatId, camera_fb_t *frame, const String &caption, String &fileId);
  bool sendPhotoById(const String &chatId, const String &fileId, const String &caption);
  bool isKnownChat(const String &chatId) const;
  bool validBotToken() const;
  String apiUrl(const String &method) const;
  bool writeAll(class WiFiClientSecure &client, const uint8_t *data, size_t length);

  int lastStatus_ = 0;
  String lastOperation_;
  uint32_t lastResultMs_ = 0;
  uint32_t lastSuccessMs_ = 0;
  String lastUnknownChat_;

  String botToken_;
  String chatIds_[MAX_CHATS];
  uint8_t chatCount_ = 0;
  bool chatListValid_ = true;
  String apiHost_ = "api.telegram.org";
  uint16_t apiPort_ = 443;
  String apiPrefix_;
};
