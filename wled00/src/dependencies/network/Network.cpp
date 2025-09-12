#include "wled.h"
#include "Network.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

IPAddress NetworkClass::localIP() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");

  uint32_t wifi_metric = 999, eth_metric = 999;
  if (wifi_netif) wifi_metric = esp_netif_get_route_prio(wifi_netif);
  if (eth_netif) eth_metric = esp_netif_get_route_prio(eth_netif);

  // Check the preferred interface first (Ethernet by default)
  if (eth_metric < wifi_metric) {
    if (eth_netif && esp_netif_get_ip_info(eth_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      return IPAddress(ip_info.ip.addr); // Return Ethernet IP if valid
    }
  }
  if (wifi_netif && esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    return IPAddress(ip_info.ip.addr); // Return Wi-Fi IP if valid
  }
  return INADDR_NONE;
}

IPAddress NetworkClass::subnetMask() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* netif = esp_netif_get_default_netif();
  if (netif) {
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      return IPAddress(ip_info.netmask.addr);
    }
  }
  return IPAddress(0, 0, 0, 0);
}

IPAddress NetworkClass::gatewayIP() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t* netif = esp_netif_get_default_netif();
  if (netif) {
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      return IPAddress(ip_info.gw.addr);
    }
  }
  return INADDR_NONE;
}

void NetworkClass::localMAC(uint8_t* MAC) {

  memset(MAC, 0, 6);

  esp_netif_t* default_netif = esp_netif_get_default_netif();
  if (default_netif == NULL) {
    return; // No default interface is active
  }

  esp_netif_t* wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_t* eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");

  if (default_netif == wifi_netif) {
    esp_wifi_get_mac(WIFI_IF_STA, MAC);
  } else {
    esp_netif_get_mac(eth_netif, MAC);
  }
  return;
}

bool NetworkClass::isConnected() {
  esp_netif_t* netif = esp_netif_get_default_netif();

  if (netif == NULL) {
    return false;
  }

  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    return (ip_info.ip.addr != 0);
  }

  return false;
}

IPAddress NetworkClass::hostByName(const char* hostname) {
  // Note: Arduino IPAddress is IPv4 only. This function will return
  // INADDR_NONE if the hostname resolves only to an IPv6 address.

  // Use getaddrinfo to perform the DNS lookup for ANY address family
  struct addrinfo hints = {
      .ai_family = AF_UNSPEC, // Allow either IPv4 or IPv6
      .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo* res;

  if (getaddrinfo(hostname, NULL, &hints, &res) == 0 && res != NULL) {
    IPAddress result = INADDR_NONE;
    // Check the address family of the first result
    if (res->ai_family == AF_INET) {
      // It's an IPv4 address, which IPAddress can handle.
      struct in_addr* addr = &((struct sockaddr_in*)res->ai_addr)->sin_addr;
      result = IPAddress(addr->s_addr);
    } else if (res->ai_family == AF_INET6) {
      // It's an IPv6 address. The Arduino IPAddress object cannot store it.
      // You could log this if needed.
      ESP_LOGE("Network", "Hostname '%s' resolved to an IPv6 address, which is not supported.", hostname);
    }
    freeaddrinfo(res);
    return result;
  }

  // If we get here, the lookup failed
  return INADDR_NONE;
}

bool NetworkClass::isEthernet() {
  return eth_is_connected;
}

#ifdef ARDUINO_ARCH_ESP32
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
NetworkClass WL_Network;
#else
NetworkClass Network;
#endif
#else
NetworkClass Network;
#endif