#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// Присутствие капитана по Wi-Fi: раз в 15 секунд камера шлёт ARP-запрос на
// IP телефона в домашней сети. Ответ = телефон подключён к этому Wi-Fi.
// ARP надёжнее пинга: на него отвечает и спящий телефон (Android, iOS),
// а случайный MAC Android не мешает — важен только IP.
//
// «Ушёл» фиксируется после awayMinutes без ответов, «пришёл» — по первому
// ответу. До первого решения (ответ или истёкший таймаут) состояние
// неизвестно, чтобы после перезагрузки не слать ложных уведомлений.
class WifiPresence {
 public:
  // Пустой или неверный IP — функция выключена.
  void begin(const String &ownerIp, uint16_t awayMinutes);
  void loop();
  bool isEnabled() const { return enabled_; }
  bool hasVerdict() const;
  bool isPresent() const;
  String status() const;

  static bool isValidIp(const String &value);

 private:
  bool probe(bool sendRequest);

  bool enabled_ = false;
  IPAddress ip_;
  uint32_t awayMs_ = 10UL * 60UL * 1000UL;
  uint32_t startedMs_ = 0;
  uint32_t lastProbeMs_ = 0;
  uint32_t lastSeenMs_ = 0;
  bool everSeen_ = false;
  bool sameSubnet_ = true;
};
