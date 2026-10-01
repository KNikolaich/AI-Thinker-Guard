#include "DeviceCommand.h"

String utf8Lower(const String &value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c + 32);
    } else if (c == 0xD0 && i + 1 < value.length()) {
      const uint8_t n = static_cast<uint8_t>(value[i + 1]);
      if (n >= 0x90 && n <= 0x9F) {         // А–П -> а–п
        out += static_cast<char>(0xD0);
        out += static_cast<char>(n + 0x20);
      } else if (n >= 0xA0 && n <= 0xAF) {  // Р–Я -> р–я
        out += static_cast<char>(0xD1);
        out += static_cast<char>(n - 0x20);
      } else if (n == 0x81) {               // Ё -> ё
        out += static_cast<char>(0xD1);
        out += static_cast<char>(0x91);
      } else {
        out += static_cast<char>(c);
        out += static_cast<char>(n);
      }
      ++i;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}
