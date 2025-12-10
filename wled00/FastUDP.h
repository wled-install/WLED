#ifndef FastUDP_h
#define FastUDP_h

#include "Arduino.h"
#include "IPAddress.h"
#include <functional>

// FIX 1: Include LwIP headers here so 'err_t' and 'struct udp_pcb' are known
extern "C" {
  #include "lwip/err.h"
  #include "lwip/udp.h" 
}

class FastUDP;
class FastUDPPacket;

typedef enum {
  FAST_IF_STA = 0,     /**< Wi-Fi STA (station) interface */
  FAST_IF_AP,          /**< Wi-Fi soft-AP interface */
  FAST_IF_ETH,         /**< Ethernet interface */
  FAST_IF_PPP,         /**< PPP interface */
  FAST_IF_MAX
} FastUDPAdapterIf;

typedef std::function<void(FastUDPPacket& packet)> FastPacketHandlerFunction;
typedef std::function<void(void* arg, FastUDPPacket& packet)> FastPacketHandlerFunctionWithArg;

class FastUDPMessage {
  size_t _size;
  size_t _index;
  uint8_t* _buffer;
public:
  FastUDPMessage(size_t size = 1460);
  virtual ~FastUDPMessage();
  size_t write(const uint8_t* data, size_t len);
  size_t write(uint8_t data);
  size_t space();
  uint8_t* data();
  size_t length();
  void flush();
  operator bool() const {
    return _buffer != NULL;
  }
};

class FastUDPPacket {
  FastUDP* _udp;
  struct pbuf* _pb;
  FastUDPAdapterIf _if;
  uint8_t* _data;
  size_t _len;
  size_t _index;
  struct ip_addr _remoteIp;
  struct ip_addr _localIp;
  uint16_t _localPort;
  uint16_t _remotePort;
  uint8_t _remoteMac[6];
public:
  FastUDPPacket(FastUDP* udp, struct pbuf* pb, const struct ip_addr* remote_addr, uint16_t remote_port, struct netif* netif);
  FastUDPPacket(FastUDPPacket& packet);
  virtual ~FastUDPPacket();

  uint8_t* data();
  size_t length();
  int available();
  size_t read(uint8_t* data, size_t len);
  int read();
  int peek();
  void flush();

  FastUDPAdapterIf interface();

  IPAddress localIP();
  IPAddress localIPv6();
  uint16_t localPort();
  IPAddress remoteIP();
  IPAddress remoteIPv6();
  uint16_t remotePort();
  void remoteMac(uint8_t* mac);

  bool isIPv6();
  bool isBroadcast();
  bool isMulticast();

  size_t write(const uint8_t* data, size_t len);
  size_t write(uint8_t data);
  size_t send(FastUDPMessage& message);

  FastUDPPacket& operator=(const FastUDPPacket& packet);
};

class FastUDP {
  struct udp_pcb* _pcb;
  bool _connected;
  err_t _lastErr;
  FastPacketHandlerFunction _handler;

  void _recv(struct udp_pcb* upcb, struct pbuf* p, const struct ip_addr* addr, u16_t port, struct netif* netif);

  // FIX 2: Added missing private declaration
  bool _init();

public:
  FastUDP();
  virtual ~FastUDP();

  bool listen(const ip_addr_t* addr, uint16_t port);
  bool listen(const IPAddress addr, uint16_t port);
  bool listen(uint16_t port);

  bool listenMulticast(const ip_addr_t* addr, uint16_t port, uint8_t ttl = 1, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);
  bool listenMulticast(const IPAddress addr, uint16_t port, uint8_t ttl = 1, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);

  bool connect(const ip_addr_t* addr, uint16_t port);
  bool connect(const IPAddress addr, uint16_t port);

  void close();

  size_t writeTo(const uint8_t* data, size_t len, const ip_addr_t* addr, uint16_t port, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);
  size_t writeTo(const uint8_t* data, size_t len, const IPAddress addr, uint16_t port, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);

  size_t write(const uint8_t* data, size_t len);
  size_t write(uint8_t data);

  size_t broadcastTo(uint8_t* data, size_t len, uint16_t port, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);
  size_t broadcastTo(const char* data, uint16_t port, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);
  size_t broadcast(uint8_t* data, size_t len);
  size_t broadcast(const char* data);

  size_t sendTo(FastUDPMessage& message, const ip_addr_t* addr, uint16_t port, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);
  size_t sendTo(FastUDPMessage& message, const IPAddress addr, uint16_t port, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);
  size_t send(FastUDPMessage& message);
  size_t broadcastTo(FastUDPMessage& message, uint16_t port, FastUDPAdapterIf tcpip_if = FAST_IF_MAX);
  size_t broadcast(FastUDPMessage& message);

  IPAddress listenIP();
  IPAddress listenIPv6();
  bool connected();
  esp_err_t lastErr();

  operator bool();

  void onPacket(FastPacketHandlerFunction cb);
  void onPacket(FastPacketHandlerFunctionWithArg cb, void* arg);

  static void _s_recv(void* arg, struct udp_pcb* upcb, struct pbuf* p, const struct ip_addr* addr, u16_t port, struct netif* netif);
};

#endif