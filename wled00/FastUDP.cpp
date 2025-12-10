#include "Arduino.h"
#include "FastUDP.h"

extern "C" {
  #include "lwip/opt.h"
  #include "lwip/inet.h"
  #include "lwip/udp.h"
  #include "lwip/igmp.h"
  #include "lwip/ip_addr.h"
  #include "lwip/mld6.h"
  #include "lwip/prot/ethernet.h"
  #include <esp_err.h>
  #include <esp_wifi.h>
  #include <esp_netif.h>
}

#include "lwip/priv/tcpip_priv.h"

// ... Macros remain same ...
#ifndef CONFIG_ARDUINO_UDP_TASK_STACK_SIZE
#define CONFIG_ARDUINO_UDP_TASK_STACK_SIZE 4096
#endif
#ifndef ARDUINO_UDP_TASK_STACK_SIZE
#define ARDUINO_UDP_TASK_STACK_SIZE CONFIG_ARDUINO_UDP_TASK_STACK_SIZE
#endif

#ifndef CONFIG_ARDUINO_UDP_TASK_PRIORITY
#define CONFIG_ARDUINO_UDP_TASK_PRIORITY 3
#endif
#ifndef ARDUINO_UDP_TASK_PRIORITY
#define ARDUINO_UDP_TASK_PRIORITY CONFIG_ARDUINO_UDP_TASK_PRIORITY
#endif

#ifndef CONFIG_ARDUINO_UDP_RUNNING_CORE
#define CONFIG_ARDUINO_UDP_RUNNING_CORE -1
#endif
#ifndef ARDUINO_UDP_RUNNING_CORE
#define ARDUINO_UDP_RUNNING_CORE CONFIG_ARDUINO_UDP_RUNNING_CORE
#endif

#ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING
#define UDP_MUTEX_LOCK()                                \
  if (!sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) { \
    LOCK_TCPIP_CORE();                                  \
  }

#define UDP_MUTEX_UNLOCK()                             \
  if (sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) { \
    UNLOCK_TCPIP_CORE();                               \
  }
#else 
#define UDP_MUTEX_LOCK()
#define UDP_MUTEX_UNLOCK()
#endif

// Helper for Interfaces
static const char* netif_ifkeys[FAST_IF_MAX] = { "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF", "PPP_DEF" };

static esp_err_t tcpip_adapter_get_netif(FastUDPAdapterIf tcpip_if, void** netif) {
  *netif = NULL;
  if (tcpip_if < FAST_IF_MAX) {
    esp_netif_t* esp_netif = esp_netif_get_handle_from_ifkey(netif_ifkeys[tcpip_if]);
    if (esp_netif == NULL) {
      return ESP_FAIL;
    }
    int netif_index = esp_netif_get_netif_impl_index(esp_netif);
    if (netif_index < 0) {
      return ESP_FAIL;
    }
    UDP_MUTEX_LOCK();
    *netif = (void*)netif_get_by_index(netif_index);
    UDP_MUTEX_UNLOCK();
  } else {
    *netif = netif_default;
  }
  return (*netif != NULL) ? ESP_OK : ESP_FAIL;
}

// ... API Structures ...

typedef struct {
  struct tcpip_api_call_data call;
  udp_pcb* pcb;
  const ip_addr_t* addr;
  uint16_t port;
  struct pbuf* pb;
  struct netif* netif;
  err_t err;
} udp_api_call_t;

static err_t _udp_connect_api(struct tcpip_api_call_data* api_call_msg) {
  udp_api_call_t* msg = (udp_api_call_t*)api_call_msg;
  msg->err = udp_connect(msg->pcb, msg->addr, msg->port);
  return msg->err;
}

static err_t _udp_connect(struct udp_pcb* pcb, const ip_addr_t* addr, u16_t port) {
  udp_api_call_t msg;
  msg.pcb = pcb;
  msg.addr = addr;
  msg.port = port;
  tcpip_api_call(_udp_connect_api, (struct tcpip_api_call_data*)&msg);
  // FIX 3: msg is a struct, use dot operator
  return msg.err;
}

static err_t _udp_disconnect_api(struct tcpip_api_call_data* api_call_msg) {
  udp_api_call_t* msg = (udp_api_call_t*)api_call_msg;
  msg->err = 0;
  udp_disconnect(msg->pcb);
  return msg->err;
}

static void _udp_disconnect(struct udp_pcb* pcb) {
  udp_api_call_t msg;
  msg.pcb = pcb;
  tcpip_api_call(_udp_disconnect_api, (struct tcpip_api_call_data*)&msg);
}

static err_t _udp_remove_api(struct tcpip_api_call_data* api_call_msg) {
  udp_api_call_t* msg = (udp_api_call_t*)api_call_msg;
  msg->err = 0;
  udp_remove(msg->pcb);
  return msg->err;
}

static void _udp_remove(struct udp_pcb* pcb) {
  udp_api_call_t msg;
  msg.pcb = pcb;
  tcpip_api_call(_udp_remove_api, (struct tcpip_api_call_data*)&msg);
}

static err_t _udp_bind_api(struct tcpip_api_call_data* api_call_msg) {
  udp_api_call_t* msg = (udp_api_call_t*)api_call_msg;
  msg->err = udp_bind(msg->pcb, msg->addr, msg->port);
  return msg->err;
}

static err_t _udp_bind(struct udp_pcb* pcb, const ip_addr_t* addr, u16_t port) {
  udp_api_call_t msg;
  msg.pcb = pcb;
  msg.addr = addr;
  msg.port = port;
  tcpip_api_call(_udp_bind_api, (struct tcpip_api_call_data*)&msg);
  // FIX 3: msg is a struct, use dot operator
  return msg.err;
}


// ... Task Handling ...
typedef struct {
  void* arg;
  udp_pcb* pcb;
  pbuf* pb;
  const ip_addr_t* addr;
  uint16_t port;
  struct netif* netif;
} lwip_event_packet_t;

static QueueHandle_t _udp_queue;
static volatile TaskHandle_t _udp_task_handle = NULL;

static void _udp_task(void* pvParameters) {
  (void)pvParameters;
  lwip_event_packet_t* e = NULL;
  for (;;) {
    if (xQueueReceive(_udp_queue, &e, portMAX_DELAY) == pdTRUE) {
      if (!e->pb) {
        free((void*)(e));
        continue;
      }
      FastUDP::_s_recv(e->arg, e->pcb, e->pb, e->addr, e->port, e->netif);
      free((void*)(e));
    }
  }
  _udp_task_handle = NULL;
  vTaskDelete(NULL);
}

static bool _udp_task_start() {
  if (!_udp_queue) {
    _udp_queue = xQueueCreate(32, sizeof(lwip_event_packet_t*));
    if (!_udp_queue) {
      return false;
    }
  }
  if (!_udp_task_handle) {
    xTaskCreateUniversal(
      _udp_task, "fast_udp", ARDUINO_UDP_TASK_STACK_SIZE, NULL, ARDUINO_UDP_TASK_PRIORITY, (TaskHandle_t*)&_udp_task_handle, ARDUINO_UDP_RUNNING_CORE
    );
    if (!_udp_task_handle) {
      return false;
    }
  }
  return true;
}

static bool _udp_task_post(void* arg, udp_pcb* pcb, pbuf* pb, const ip_addr_t* addr, uint16_t port, struct netif* netif) {
  if (!_udp_task_handle || !_udp_queue) {
    return false;
  }
  lwip_event_packet_t* e = (lwip_event_packet_t*)malloc(sizeof(lwip_event_packet_t));
  if (!e) {
    return false;
  }
  e->arg = arg;
  e->pcb = pcb;
  e->pb = pb;
  e->addr = addr;
  e->port = port;
  e->netif = netif;
  if (xQueueSend(_udp_queue, &e, portMAX_DELAY) != pdPASS) {
    free((void*)(e));
    return false;
  }
  return true;
}

static void _udp_recv(void* arg, udp_pcb* pcb, pbuf* pb, const ip_addr_t* addr, uint16_t port) {
  while (pb != NULL) {
    pbuf* this_pb = pb;
    pb = pb->next;
    this_pb->next = NULL;
    if (!_udp_task_post(arg, pcb, this_pb, addr, port, ip_current_input_netif())) {
      pbuf_free(this_pb);
    }
  }
}

// ... FastUDPMessage (Unchanged) ...
FastUDPMessage::FastUDPMessage(size_t size) {
  _index = 0;
  if (size > CONFIG_TCP_MSS) {
    size = CONFIG_TCP_MSS;
  }
  _size = size;
  _buffer = (uint8_t*)malloc(size);
}
FastUDPMessage::~FastUDPMessage() {
  if (_buffer) free(_buffer);
}
size_t FastUDPMessage::write(const uint8_t* data, size_t len) {
  if (_buffer == NULL) return 0;
  size_t s = space();
  if (len > s) len = s;
  memcpy(_buffer + _index, data, len);
  _index += len;
  return len;
}
size_t FastUDPMessage::write(uint8_t data) {
  return write(&data, 1);
}
size_t FastUDPMessage::space() {
  if (_buffer == NULL) return 0;
  return _size - _index;
}
uint8_t* FastUDPMessage::data() {
  return _buffer;
}
size_t FastUDPMessage::length() {
  return _index;
}
void FastUDPMessage::flush() {
  _index = 0;
}

// ... FastUDPPacket (Unchanged except FastUDPAdapterIf) ...
FastUDPPacket::FastUDPPacket(FastUDPPacket& packet) {
  _udp = packet._udp;
  _pb = packet._pb;
  _if = packet._if;
  _data = packet._data;
  _len = packet._len;
  _index = 0;
  memcpy(&_remoteIp, &packet._remoteIp, sizeof(ip_addr_t));
  memcpy(&_localIp, &packet._localIp, sizeof(ip_addr_t));
  _localPort = packet._localPort;
  _remotePort = packet._remotePort;
  memcpy(_remoteMac, packet._remoteMac, 6);
  pbuf_ref(_pb);
}
FastUDPPacket& FastUDPPacket::operator=(const FastUDPPacket& packet) {
  if (this != &packet) {
    if (_pb) pbuf_free(_pb);
    _udp = packet._udp;
    _pb = packet._pb;
    _if = packet._if;
    _data = packet._data;
    _len = packet._len;
    _index = 0;
    memcpy(&_remoteIp, &packet._remoteIp, sizeof(ip_addr_t));
    memcpy(&_localIp, &packet._localIp, sizeof(ip_addr_t));
    _localPort = packet._localPort;
    _remotePort = packet._remotePort;
    memcpy(_remoteMac, packet._remoteMac, 6);
    pbuf_ref(_pb);
  }
  return *this;
}
FastUDPPacket::FastUDPPacket(FastUDP* udp, pbuf* pb, const ip_addr_t* raddr, uint16_t rport, struct netif* ntif) {
  _udp = udp;
  _pb = pb;
  _if = FAST_IF_MAX;
  _data = (uint8_t*)(pb->payload);
  _len = pb->len;
  _index = 0;
  pbuf_ref(_pb);
  #if CONFIG_LWIP_IPV6
  _remoteIp.type = raddr->type;
  _localIp.type = _remoteIp.type;
  #endif
  eth_hdr* eth = NULL;
  udp_hdr* udphdr = (udp_hdr*)(_data - UDP_HLEN);
  _localPort = ntohs(udphdr->dest);
  _remotePort = ntohs(udphdr->src);
  #if CONFIG_LWIP_IPV6
  if (_remoteIp.type == IPADDR_TYPE_V4) {
    #endif
    eth = (eth_hdr*)(_data - UDP_HLEN - IP_HLEN - SIZEOF_ETH_HDR);
    struct ip_hdr* iphdr = (struct ip_hdr*)(_data - UDP_HLEN - IP_HLEN);
    #if CONFIG_LWIP_IPV6
    _localIp.u_addr.ip4.addr = iphdr->dest.addr;
    _remoteIp.u_addr.ip4.addr = iphdr->src.addr;
    #else
    _localIp.addr = iphdr->dest.addr;
    _remoteIp.addr = iphdr->src.addr;
    #endif
    #if CONFIG_LWIP_IPV6
  } else {
    eth = (eth_hdr*)(_data - UDP_HLEN - IP6_HLEN - SIZEOF_ETH_HDR);
    struct ip6_hdr* ip6hdr = (struct ip6_hdr*)(_data - UDP_HLEN - IP6_HLEN);
    memcpy(&_localIp.u_addr.ip6.addr, (uint8_t*)ip6hdr->dest.addr, 16);
    memcpy(&_remoteIp.u_addr.ip6.addr, (uint8_t*)ip6hdr->src.addr, 16);
  }
  #endif
  memcpy(_remoteMac, eth->src.addr, 6);
  struct netif* netif = NULL;
  void* nif = NULL;
  int i;
  for (i = 0; i < FAST_IF_MAX; i++) {
    tcpip_adapter_get_netif((FastUDPAdapterIf)i, &nif);
    netif = (struct netif*)nif;
    if (netif && netif == ntif) {
      _if = (FastUDPAdapterIf)i;
      break;
    }
  }
}
FastUDPPacket::~FastUDPPacket() {
  pbuf_free(_pb);
}
uint8_t* FastUDPPacket::data() { return _data; }
size_t FastUDPPacket::length() { return _len; }
int FastUDPPacket::available() { return _len - _index; }
size_t FastUDPPacket::read(uint8_t* data, size_t len) {
  size_t i;
  size_t a = _len - _index;
  if (len > a) len = a;
  for (i = 0; i < len; i++) data[i] = read();
  return len;
}
int FastUDPPacket::read() {
  if (_index < _len) return _data[_index++];
  return -1;
}
int FastUDPPacket::peek() {
  if (_index < _len) return _data[_index];
  return -1;
}
void FastUDPPacket::flush() { _index = _len; }
FastUDPAdapterIf FastUDPPacket::interface() { return _if; }
IPAddress FastUDPPacket::localIP() {
  #if CONFIG_LWIP_IPV6
  if (_localIp.type != IPADDR_TYPE_V4) return IPAddress();
  return IPAddress(_localIp.u_addr.ip4.addr);
  #else
  return IPAddress(_localIp.addr);
  #endif
}
#if CONFIG_LWIP_IPV6
IPAddress FastUDPPacket::localIPv6() {
  if (_localIp.type != IPADDR_TYPE_V6) return IPAddress(IPv6);
  return IPAddress(IPv6, (const uint8_t*)_localIp.u_addr.ip6.addr, _localIp.u_addr.ip6.zone);
}
#endif
uint16_t FastUDPPacket::localPort() { return _localPort; }
IPAddress FastUDPPacket::remoteIP() {
  #if CONFIG_LWIP_IPV6
  if (_remoteIp.type != IPADDR_TYPE_V4) return IPAddress();
  return IPAddress(_remoteIp.u_addr.ip4.addr);
  #else
  return IPAddress(_remoteIp.addr);
  #endif
}
#if CONFIG_LWIP_IPV6
IPAddress FastUDPPacket::remoteIPv6() {
  if (_remoteIp.type != IPADDR_TYPE_V6) return IPAddress(IPv6);
  return IPAddress(IPv6, (const uint8_t*)_remoteIp.u_addr.ip6.addr, _remoteIp.u_addr.ip6.zone);
}
#endif
uint16_t FastUDPPacket::remotePort() { return _remotePort; }
void FastUDPPacket::remoteMac(uint8_t* mac) { memcpy(mac, _remoteMac, 6); }
bool FastUDPPacket::isIPv6() {
  #if CONFIG_LWIP_IPV6
  return _localIp.type == IPADDR_TYPE_V6;
  #else
  return false;
  #endif
}
bool FastUDPPacket::isBroadcast() {
  #if CONFIG_LWIP_IPV6
  if (_localIp.type == IPADDR_TYPE_V6) return false;
  uint32_t ip = _localIp.u_addr.ip4.addr;
  #else
  uint32_t ip = _localIp.addr;
  #endif
  return ip == 0xFFFFFFFF || ip == 0 || (ip & 0xFF000000) == 0xFF000000;
}
bool FastUDPPacket::isMulticast() {
  return ip_addr_ismulticast(&(_localIp));
}
size_t FastUDPPacket::write(const uint8_t* data, size_t len) {
  if (!data) return 0;
  return _udp->writeTo(data, len, &_remoteIp, _remotePort, _if);
}
size_t FastUDPPacket::write(uint8_t data) { return write(&data, 1); }
size_t FastUDPPacket::send(FastUDPMessage& message) {
  return write(message.data(), message.length());
}


// --- FastUDP Main Class ---

bool FastUDP::_init() {
  if (_pcb) {
    return true;
}
  UDP_MUTEX_LOCK();
  _pcb = udp_new();
  if (!_pcb) {
    UDP_MUTEX_UNLOCK();
    return false;
  }
  udp_recv(_pcb, &_udp_recv, (void*)this);
  UDP_MUTEX_UNLOCK();
  return true;
}

FastUDP::FastUDP() {
  _pcb = NULL;
  _connected = false;
  _lastErr = ERR_OK;
  _handler = NULL;
}

FastUDP::~FastUDP() {
  close();
  UDP_MUTEX_LOCK();
  udp_recv(_pcb, NULL, NULL);
  UDP_MUTEX_UNLOCK();
  _udp_remove(_pcb);
  _pcb = NULL;
}

void FastUDP::close() {
  if (_pcb != NULL) {
    if (_connected) {
      _udp_disconnect(_pcb);
    }
    _connected = false;
  }
}

bool FastUDP::connect(const ip_addr_t* addr, uint16_t port) {
  if (!_udp_task_start()) {
    return false;
  }
  if (!_init()) {
    return false;
  }
  close();
  _lastErr = _udp_connect(_pcb, addr, port);
  if (_lastErr != ERR_OK) {
    return false;
  }
  _connected = true;
  return true;
}

bool FastUDP::listen(const ip_addr_t* addr, uint16_t port) {
  if (!_udp_task_start()) {
    return false;
  }
  if (!_init()) {
    return false;
  }
  close();
  if (addr) {
    IP_SET_TYPE_VAL(_pcb->local_ip, IP_GET_TYPE(addr));
    IP_SET_TYPE_VAL(_pcb->remote_ip, IP_GET_TYPE(addr));
  }
  if (_udp_bind(_pcb, addr, port) != ERR_OK) {
    return false;
  }
  _connected = true;
  return true;
}

static esp_err_t joinMulticastGroup(const ip_addr_t* addr, bool join, FastUDPAdapterIf tcpip_if = FAST_IF_MAX) {
  struct netif* netif = NULL;
  if (tcpip_if < FAST_IF_MAX) {
    void* nif = NULL;
    esp_err_t err = tcpip_adapter_get_netif(tcpip_if, &nif);
    if (err) return ESP_ERR_INVALID_ARG;
    netif = (struct netif*)nif;
    UDP_MUTEX_LOCK();

    #if CONFIG_LWIP_IPV6
    if (addr->type == IPADDR_TYPE_V4) {
      if (join) igmp_joingroup_netif(netif, (const ip4_addr*)&(addr->u_addr.ip4));
      else igmp_leavegroup_netif(netif, (const ip4_addr*)&(addr->u_addr.ip4));
    } else {
      if (join) mld6_joingroup_netif(netif, &(addr->u_addr.ip6));
      else mld6_leavegroup_netif(netif, &(addr->u_addr.ip6));
    }
    #else
    if (join) igmp_joingroup_netif(netif, (const ip4_addr*)(addr));
    else igmp_leavegroup_netif(netif, (const ip4_addr*)(addr));
    #endif
    UDP_MUTEX_UNLOCK();
  } else {
    UDP_MUTEX_LOCK();
    #if CONFIG_LWIP_IPV6
    if (addr->type == IPADDR_TYPE_V4) {
      if (join) igmp_joingroup((const ip4_addr*)IP4_ADDR_ANY, (const ip4_addr*)&(addr->u_addr.ip4));
      else igmp_leavegroup((const ip4_addr*)IP4_ADDR_ANY, (const ip4_addr*)&(addr->u_addr.ip4));
    } else {
      if (join) mld6_joingroup((const ip6_addr*)IP6_ADDR_ANY, &(addr->u_addr.ip6));
      else mld6_leavegroup((const ip6_addr*)IP6_ADDR_ANY, &(addr->u_addr.ip6));
    }
    #else
    if (join) igmp_joingroup((const ip4_addr*)IP4_ADDR_ANY, (const ip4_addr*)(addr));
    else igmp_leavegroup((const ip4_addr*)IP4_ADDR_ANY, (const ip4_addr*)(addr));
    #endif
    UDP_MUTEX_UNLOCK();
  }
  return ESP_OK;
}

bool FastUDP::listenMulticast(const ip_addr_t* addr, uint16_t port, uint8_t ttl, FastUDPAdapterIf tcpip_if) {
  ip_addr_t bind_addr;
  if (!ip_addr_ismulticast(addr)) return false;
  if (joinMulticastGroup(addr, true, tcpip_if) != ERR_OK) return false;

  IP_SET_TYPE(&bind_addr, IP_GET_TYPE(addr));
  ip_addr_set_any(IP_IS_V6(addr), &bind_addr);
  if (!listen(&bind_addr, port)) return false;

  _pcb->mcast_ttl = ttl;
  _pcb->remote_port = port;
  ip_addr_copy(_pcb->remote_ip, *addr);
  return true;
}

// *** THE OPTIMIZED SEND FUNCTION ***
size_t FastUDP::writeTo(const uint8_t* data, size_t len, const ip_addr_t* addr, uint16_t port, FastUDPAdapterIf tcpip_if) {
  if (!_pcb) {
    UDP_MUTEX_LOCK();
    _pcb = udp_new();
    UDP_MUTEX_UNLOCK();
    if (_pcb == NULL) return 0;
  }

  if (len > CONFIG_TCP_MSS) len = CONFIG_TCP_MSS;

  // 1. Allocate
  pbuf* pbt = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);

  if (pbt != NULL) {
    uint8_t* dst = reinterpret_cast<uint8_t*>(pbt->payload);
    memcpy(dst, data, len);

    err_t err = ERR_MEM;

    // 2. Try to Send ONCE. No retries inside the library.
    UDP_MUTEX_LOCK();

    if (tcpip_if < FAST_IF_MAX) {
      void* nif = NULL;
      tcpip_adapter_get_netif((FastUDPAdapterIf)tcpip_if, &nif);
      if (nif) {
        err = udp_sendto_if(_pcb, pbt, addr, port, (struct netif*)nif);
      } else {
        err = udp_sendto(_pcb, pbt, addr, port);
      }
    } else {
      err = udp_sendto(_pcb, pbt, addr, port);
    }

    UDP_MUTEX_UNLOCK();

    // 3. Cleanup
    pbuf_free(pbt);

    // 4. Report Result
    if (err != ERR_OK) {
      _lastErr = err;
      return 0; // Failed
    }

    _lastErr = ERR_OK;
    return len; // Success
  }

  return 0; // Alloc failed
}

void FastUDP::_recv(udp_pcb* upcb, pbuf* pb, const ip_addr_t* addr, uint16_t port, struct netif* netif) {
  while (pb != NULL) {
    pbuf* this_pb = pb;
    pb = pb->next;
    this_pb->next = NULL;
    if (_handler) {
      FastUDPPacket packet(this, this_pb, addr, port, netif);
      _handler(packet);
    }
    pbuf_free(this_pb);
  }
  }

void FastUDP::_s_recv(void* arg, udp_pcb* upcb, pbuf* p, const ip_addr_t* addr, u16_t port, struct netif* netif) {
  reinterpret_cast<FastUDP*>(arg)->_recv(upcb, p, addr, port, netif);
}

// ... Rest of wrappers ...
bool FastUDP::listen(uint16_t port) { return listen(IP_ANY_TYPE, port); }
bool FastUDP::listen(const IPAddress addr, uint16_t port) {
  ip_addr_t laddr;
  addr.to_ip_addr_t(&laddr);
  return listen(&laddr, port);
}
bool FastUDP::listenMulticast(const IPAddress addr, uint16_t port, uint8_t ttl, FastUDPAdapterIf tcpip_if) {
  ip_addr_t laddr;
  addr.to_ip_addr_t(&laddr);
  return listenMulticast(&laddr, port, ttl, tcpip_if);
}
bool FastUDP::connect(const IPAddress addr, uint16_t port) {
  ip_addr_t daddr;
  addr.to_ip_addr_t(&daddr);
  return connect(&daddr, port);
}
size_t FastUDP::writeTo(const uint8_t* data, size_t len, const IPAddress addr, uint16_t port, FastUDPAdapterIf tcpip_if) {
  ip_addr_t daddr;
  addr.to_ip_addr_t(&daddr);
  return writeTo(data, len, &daddr, port, tcpip_if);
}
IPAddress FastUDP::listenIP() {
  #if CONFIG_LWIP_IPV6
  if (!_pcb || _pcb->remote_ip.type != IPADDR_TYPE_V4) return IPAddress();
  return IPAddress(_pcb->remote_ip.u_addr.ip4.addr);
  #else
  return IPAddress(_pcb->remote_ip.addr);
  #endif
}
#if CONFIG_LWIP_IPV6
IPAddress FastUDP::listenIPv6() {
  if (!_pcb || _pcb->remote_ip.type != IPADDR_TYPE_V6) return IPAddress(IPv6);
  return IPAddress(IPv6, (const uint8_t*)_pcb->remote_ip.u_addr.ip6.addr, _pcb->remote_ip.u_addr.ip6.zone);
}
#endif
size_t FastUDP::write(const uint8_t* data, size_t len) {
  return writeTo(data, len, &(_pcb->remote_ip), _pcb->remote_port);
}
size_t FastUDP::write(uint8_t data) { return write(&data, 1); }
size_t FastUDP::broadcastTo(uint8_t* data, size_t len, uint16_t port, FastUDPAdapterIf tcpip_if) {
  return writeTo(data, len, IP_ADDR_BROADCAST, port, tcpip_if);
}
size_t FastUDP::broadcastTo(const char* data, uint16_t port, FastUDPAdapterIf tcpip_if) {
  return broadcastTo((uint8_t*)data, strlen(data), port, tcpip_if);
}
size_t FastUDP::broadcast(uint8_t* data, size_t len) {
  if (_pcb->local_port != 0) return broadcastTo(data, len, _pcb->local_port);
  return 0;
}
size_t FastUDP::broadcast(const char* data) { return broadcast((uint8_t*)data, strlen(data)); }
size_t FastUDP::sendTo(FastUDPMessage& message, const ip_addr_t* addr, uint16_t port, FastUDPAdapterIf tcpip_if) {
  if (!message) return 0;
  return writeTo(message.data(), message.length(), addr, port, tcpip_if);
}
size_t FastUDP::sendTo(FastUDPMessage& message, const IPAddress addr, uint16_t port, FastUDPAdapterIf tcpip_if) {
  if (!message) return 0;
  return writeTo(message.data(), message.length(), addr, port, tcpip_if);
}
size_t FastUDP::send(FastUDPMessage& message) {
  if (!message) return 0;
  return writeTo(message.data(), message.length(), &(_pcb->remote_ip), _pcb->remote_port);
}
size_t FastUDP::broadcastTo(FastUDPMessage& message, uint16_t port, FastUDPAdapterIf tcpip_if) {
  if (!message) return 0;
  return broadcastTo(message.data(), message.length(), port, tcpip_if);
}
size_t FastUDP::broadcast(FastUDPMessage& message) {
  if (!message) return 0;
  return broadcast(message.data(), message.length());
}
FastUDP::operator bool() { return _connected; }
bool FastUDP::connected() { return _connected; }
esp_err_t FastUDP::lastErr() { return _lastErr; }
void FastUDP::onPacket(FastPacketHandlerFunctionWithArg cb, void* arg) {
  onPacket(std::bind(cb, arg, std::placeholders::_1));
}
void FastUDP::onPacket(FastPacketHandlerFunction cb) { _handler = cb; }