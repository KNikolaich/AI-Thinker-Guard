#pragma once

#include <Arduino.h>

// Команда устройству — из Telegram (прямой режим) или от хаба (MQTT).
struct DeviceCommand {
  String name;        // getcapture, status, help, start, arm, disarm, periodic, unknown
  String arg;         // остаток строки: «on», «off», имя устройства…
  String chatId;      // кому отвечать (пусто — всем)
  String callbackId;  // нажатие inline-кнопки (только прямой режим)
};

// Нижний регистр для латиницы и кириллицы в UTF-8 (String::toLowerCase
// понимает только ASCII). Нужен для сравнения имён устройств.
String utf8Lower(const String &value);
