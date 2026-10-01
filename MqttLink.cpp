#include "MqttLink.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include "SystemHealth.h"

static const uint32_t MQTT_RETRY_MAX_MS = 60000;

void MqttLink::configure(const String &host, uint16_t port, const String &user,
                         const String &password, const String &deviceId,
                         const String &deviceName, const String &deviceType,
                         const String &firmware) {
  host_ = host;
  host_.trim();
  port_ = port == 0 ? 8883 : port;
  user_ = user;
  password_ = password;
  deviceId_ = deviceId;
  deviceName_ = deviceName;
  deviceType_ = deviceType;
  firmware_ = firmware;
  if (!isConfigured()) return;

  if (port_ == 1883) {
    client_.setClient(plain_);
  } else {
    tls_.setInsecure();
    tls_.setTimeout(10);           // секунды
    tls_.setHandshakeTimeout(10);  // секунды
    client_.setClient(tls_);
  }
  client_.setServer(host_.c_str(), port_);  // PubSubClient хранит указатель — host_ живёт с объектом
  client_.setKeepAlive(30);
  client_.setSocketTimeout(10);
  client_.setBufferSize(1024);
  client_.setCallback([this](char *t, uint8_t *p, unsigned int l) { onMessage(t, p, l); });
  lastAttemptMs_ = millis() - retryDelayMs_;  // первая попытка сразу
}

String MqttLink::topic(const char *leaf) const {
  return "swarm/dev/" + deviceId_ + "/" + leaf;
}

String MqttLink::serverText() const {
  return host_ + ":" + String(port_) + (port_ == 1883 ? " (без TLS)" : " (TLS)");
}

bool MqttLink::connected() {
  return isConfigured() && client_.connected();
}

bool MqttLink::connectNow() {
  const String willTopic = topic("status");
  feedWatchdog();
  const bool ok = client_.connect(deviceId_.c_str(),
                                  user_.isEmpty() ? nullptr : user_.c_str(),
                                  password_.isEmpty() ? nullptr : password_.c_str(),
                                  willTopic.c_str(), 1, true, "offline", true);
  feedWatchdog();
  if (!ok) {
    lastError_ = client_.state();
    return false;
  }
  lastError_ = 0;
  connectedSinceMs_ = millis();
  everConnected_ = true;
  client_.publish(willTopic.c_str(), "online", true);

  StaticJsonDocument<384> meta;
  meta["id"] = deviceId_;
  meta["name"] = deviceName_;
  meta["type"] = deviceType_;
  meta["fw"] = firmware_;
  meta["ip"] = WiFi.localIP().toString();
  String metaJson;
  serializeJson(meta, metaJson);
  client_.publish(topic("meta").c_str(), metaJson.c_str(), true);
  if (!lastState_.isEmpty()) client_.publish(topic("state").c_str(), lastState_.c_str(), true);

  client_.subscribe(topic("cmd").c_str(), 1);
  client_.subscribe(("swarm/type/" + deviceType_ + "/cmd").c_str(), 1);
  client_.subscribe("swarm/all/cmd", 1);
  Serial.println("MQTT: подключено к " + serverText());
  return true;
}

void MqttLink::loop() {
  if (!isConfigured()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (client_.connected()) {
    client_.loop();
    retryDelayMs_ = 5000;
    return;
  }
  const uint32_t now = millis();
  if (now - lastAttemptMs_ < retryDelayMs_) return;
  lastAttemptMs_ = now;
  if (!connectNow()) {
    Serial.printf("MQTT: нет подключения к %s (код %d), повтор через %lu с\n",
                  serverText().c_str(), lastError_, static_cast<unsigned long>(retryDelayMs_ / 1000));
    retryDelayMs_ = retryDelayMs_ * 2 > MQTT_RETRY_MAX_MS ? MQTT_RETRY_MAX_MS : retryDelayMs_ * 2;
  }
}

void MqttLink::onMessage(char *topicName, uint8_t *payload, unsigned int length) {
  (void)topicName;
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) return;
  DeviceCommand command;
  command.name = doc["cmd"] | "";
  command.arg = doc["arg"] | "";
  command.chatId = doc["chat"] | "";
  command.name.trim();
  command.name.toLowerCase();
  if (command.name.isEmpty()) return;
  if (queueCount_ == QUEUE_SIZE) {  // переполнение — выбрасываем самую старую
    queueHead_ = (queueHead_ + 1) % QUEUE_SIZE;
    --queueCount_;
  }
  queue_[(queueHead_ + queueCount_) % QUEUE_SIZE] = command;
  ++queueCount_;
}

bool MqttLink::popCommand(DeviceCommand &command) {
  if (queueCount_ == 0) return false;
  command = queue_[queueHead_];
  queueHead_ = (queueHead_ + 1) % QUEUE_SIZE;
  --queueCount_;
  return true;
}

bool MqttLink::publishEvent(const char *kind, const String &text, const String &chat) {
  if (!connected()) return false;
  DynamicJsonDocument doc(256 + text.length() * 2);
  doc["kind"] = kind;
  doc["text"] = text;
  if (!chat.isEmpty()) doc["chat"] = chat;
  String json;
  serializeJson(doc, json);
  return client_.publish(topic("event").c_str(), json.c_str(), false);
}

bool MqttLink::publishState(const String &json) {
  lastState_ = json;
  if (!connected()) return false;
  return client_.publish(topic("state").c_str(), json.c_str(), true);
}

bool MqttLink::publishPhoto(camera_fb_t *frame, const String &caption, const char *kind,
                            const String &chat) {
  if (!connected() || frame == nullptr || frame->format != PIXFORMAT_JPEG) return false;
  StaticJsonDocument<512> header;
  header["caption"] = caption;
  header["kind"] = kind;
  if (!chat.isEmpty()) header["chat"] = chat;
  String headerJson;
  serializeJson(header, headerJson);  // компактный JSON — без переводов строк
  headerJson += '\n';

  const size_t total = headerJson.length() + frame->len;
  if (!client_.beginPublish(topic("photo").c_str(), total, false)) return false;
  size_t written = client_.write(reinterpret_cast<const uint8_t *>(headerJson.c_str()),
                                 headerJson.length());
  const size_t chunk = 4096;
  for (size_t offset = 0; offset < frame->len; offset += chunk) {
    const size_t n = frame->len - offset < chunk ? frame->len - offset : chunk;
    const size_t w = client_.write(frame->buf + offset, n);
    written += w;
    feedWatchdog();
    if (w != n) break;
  }
  const bool ended = client_.endPublish();
  return ended && written == total;
}

String MqttLink::status() const {
  if (!isConfigured()) return "выключен (адрес брокера не задан)";
  String text = serverText() + ": ";
  if (const_cast<PubSubClient &>(client_).connected()) {
    return text + "подключено " + String((millis() - connectedSinceMs_) / 1000) + " с";
  }
  text += everConnected_ ? "связь потеряна" : "нет подключения";
  if (lastError_ != 0) {
    text += " (код " + String(lastError_);
    if (lastError_ == 4 || lastError_ == 5) text += ": неверный логин/пароль";
    else if (lastError_ == -2) text += ": сервер недоступен";
    text += ")";
  }
  return text;
}
