#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "DeviceCommand.h"
#include "esp_camera.h"

// Связь с хабом роя через MQTT-брокер.
//
// Топики (prefix = "swarm"):
//   swarm/dev/<id>/status  "online" / "offline" (retained, offline — Last Will)
//   swarm/dev/<id>/meta    {"id","name","type","fw","ip"} (retained)
//   swarm/dev/<id>/state   состояние устройства, JSON (retained)
//   swarm/dev/<id>/event   {"kind","text","chat"} — сообщения для Telegram
//   swarm/dev/<id>/photo   первая строка — JSON {"caption","chat","kind"}, дальше JPEG
// Команды (устройство подписано на все три):
//   swarm/dev/<id>/cmd, swarm/type/<type>/cmd, swarm/all/cmd
//   {"cmd":"arm","arg":"","chat":"123"}
//
// Порт 1883 — без шифрования, любой другой (обычно 8883) — TLS.
class MqttLink {
 public:
  void configure(const String &host, uint16_t port, const String &user, const String &password,
                 const String &deviceId, const String &deviceName, const String &deviceType,
                 const String &firmware);
  bool isConfigured() const { return !host_.isEmpty(); }
  void loop();
  bool connected();
  bool popCommand(DeviceCommand &command);

  bool publishEvent(const char *kind, const String &text, const String &chat = "");
  bool publishPhoto(camera_fb_t *frame, const String &caption, const char *kind,
                    const String &chat = "");
  bool publishState(const String &json);
  String status() const;
  String serverText() const;

 private:
  bool connectNow();
  void onMessage(char *topic, uint8_t *payload, unsigned int length);
  String topic(const char *leaf) const;

  WiFiClientSecure tls_;
  WiFiClient plain_;
  PubSubClient client_;
  String host_;
  uint16_t port_ = 8883;
  String user_;
  String password_;
  String deviceId_;
  String deviceName_;
  String deviceType_;
  String firmware_;
  String lastState_;
  uint32_t lastAttemptMs_ = 0;
  uint32_t retryDelayMs_ = 5000;
  int lastError_ = 0;
  uint32_t connectedSinceMs_ = 0;
  bool everConnected_ = false;

  static const uint8_t QUEUE_SIZE = 4;
  DeviceCommand queue_[QUEUE_SIZE];
  uint8_t queueHead_ = 0;
  uint8_t queueCount_ = 0;
};
