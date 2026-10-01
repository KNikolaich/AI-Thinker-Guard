#include "WifiPresence.h"

#include <WiFi.h>

extern "C" {
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
}
#include "lwip/priv/tcpip_priv.h"

namespace {

const uint32_t PROBE_INTERVAL_MS = 15000;

struct ArpCall {
  struct tcpip_api_call_data call;  // должен идти первым
  ip4_addr_t target;
  bool sendRequest;
  bool found;
  bool sameSubnet;
};

// Выполняется в потоке TCP/IP-стека (tcpip_api_call): таблицу ARP нельзя
// трогать из loop() напрямую.
err_t arpInTcpipThread(struct tcpip_api_call_data *data) {
  ArpCall *call = reinterpret_cast<ArpCall *>(data);
  call->found = false;
  call->sameSubnet = false;
  struct netif *nif = nullptr;
  struct netif *candidate = nullptr;
  NETIF_FOREACH(candidate) {
    if (!netif_is_up(candidate) || ip4_addr_isany_val(*netif_ip4_addr(candidate))) continue;
    if (ip4_addr_netcmp(&call->target, netif_ip4_addr(candidate), netif_ip4_netmask(candidate))) {
      nif = candidate;
      break;
    }
  }
  if (nif == nullptr) return ERR_OK;
  call->sameSubnet = true;

  struct eth_addr *eth = nullptr;
  const ip4_addr_t *ipInTable = nullptr;
  // Запись есть и стабильна = телефон отвечал на ARP за последние ~5 минут.
  call->found = etharp_find_addr(nif, &call->target, &eth, &ipInTable) >= 0;
  if (call->sendRequest) etharp_request(nif, &call->target);
  return ERR_OK;
}

}  // namespace

bool WifiPresence::isValidIp(const String &value) {
  String trimmed = value;
  trimmed.trim();
  IPAddress parsed;
  return !trimmed.isEmpty() && parsed.fromString(trimmed) && parsed != IPAddress(0, 0, 0, 0) &&
         parsed != IPAddress(255, 255, 255, 255);
}

void WifiPresence::begin(const String &ownerIp, uint16_t awayMinutes) {
  enabled_ = false;
  if (!isValidIp(ownerIp)) return;
  String trimmed = ownerIp;
  trimmed.trim();
  ip_.fromString(trimmed);
  if (awayMinutes < 1) awayMinutes = 1;
  awayMs_ = static_cast<uint32_t>(awayMinutes) * 60000UL;
  startedMs_ = millis();
  lastProbeMs_ = millis() - PROBE_INTERVAL_MS;  // первая проверка сразу
  enabled_ = true;
}

bool WifiPresence::probe(bool sendRequest) {
  ArpCall call = {};
  IP4_ADDR(&call.target, ip_[0], ip_[1], ip_[2], ip_[3]);
  call.sendRequest = sendRequest;
  tcpip_api_call(arpInTcpipThread, reinterpret_cast<struct tcpip_api_call_data *>(&call));
  sameSubnet_ = call.sameSubnet;
  return call.found;
}

void WifiPresence::loop() {
  if (!enabled_ || WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (now - lastProbeMs_ < PROBE_INTERVAL_MS) return;
  lastProbeMs_ = now;

  // Сначала смотрим результат прошлого запроса, потом отправляем новый.
  if (probe(true)) {
    lastSeenMs_ = now;
    everSeen_ = true;
  }
}

bool WifiPresence::hasVerdict() const {
  if (!enabled_) return false;
  return everSeen_ || millis() - startedMs_ >= awayMs_;
}

bool WifiPresence::isPresent() const {
  if (!enabled_ || !everSeen_) return false;
  return millis() - lastSeenMs_ < awayMs_;
}

String WifiPresence::status() const {
  if (!enabled_) return "выключено (IP телефона не задан)";
  String text = "телефон " + ip_.toString() + ": ";
  if (WiFi.status() != WL_CONNECTED) return text + "проверка ждёт подключения к Wi-Fi";
  if (!sameSubnet_) return text + "не в подсети камеры — проверьте IP";
  if (!everSeen_) {
    return text + (hasVerdict() ? "не отвечает, капитан не на мостике"
                                : "ещё не отвечал, жду до " + String(awayMs_ / 60000) + " мин");
  }
  const uint32_t ago = (millis() - lastSeenMs_) / 1000;
  return text + (isPresent() ? "в сети, капитан на мостике" : "пропал, капитан ушёл") +
         " (последний ответ " + String(ago) + " с назад)";
}
