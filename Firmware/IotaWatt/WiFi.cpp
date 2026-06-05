#include "IotaWatt.h"
#include "ESP8266mDNS.h"
#include <ESP8266LLMNR.h>
#if LWIP_IPV6
#include <AddrList.h>
#endif

uint32_t WiFiService(struct serviceBlock* _serviceBlock) {
  static uint32_t lastDisconnect = UTCtime();       // Time of last disconnect
  const uint32_t restartInterval = 60*60;           // Restart if disconnected this many seconds
  static bool mDNSstarted = false;
  static bool LLMNRstarted = false;
#if LWIP_IPV6
  static uint32_t ipv6LastPoll = 0;                 // Last SLAAC poll time
#endif

  trace(T_WiFi,0);
  if(WiFi.status() == WL_CONNECTED){
    trace(T_WiFi,1);
    if(!wifiConnectTime){
      trace(T_WiFi,1);
      wifiConnectTime = UTCtime();
      localIPv4 = WiFi.localIP();
      gatewayIPv4 = WiFi.gatewayIP();
      subnetMaskIPv4 = WiFi.subnetMask();
      WiFi.hostname(deviceName);
      log("WiFi connected. SSID=%s, IP=%s, channel=%d, RSSI %ddb", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
    }
    if( ! mDNSstarted){
      if (MDNS.begin(deviceName)) {
        MDNS.addService("http", "tcp", 80);
        mDNSstarted = true;
      }
    }
    else {
      MDNS.update();
    }
    if( ! LLMNRstarted){
      if (LLMNR.begin(deviceName)){
        LLMNRstarted = true;
      }
    }
        // Log resolver changes. Servers arrive via DHCPv4 and, on the
        // RDNSS-enabled lwIP build, via IPv6 Router Advertisements
        // (RFC 8106) - this makes both visible.
    {
      static IPAddress lastDns[2];
      IPAddress dns0 = WiFi.dnsIP(0);
      IPAddress dns1 = WiFi.dnsIP(1);
      if(dns0 != lastDns[0] || dns1 != lastDns[1]){
        lastDns[0] = dns0;
        lastDns[1] = dns1;
        log("WiFi: DNS servers %s, %s",
            dns0.isSet() ? dns0.toString().c_str() : "none",
            dns1.isSet() ? dns1.toString().c_str() : "none");
      }
    }
#if LWIP_IPV6
        // SLAAC assigns global IPv6 addresses asynchronously, seconds after
        // DHCP, and there is no got-IPv6 event on the ESP8266. Poll every
        // dispatch until one appears, then every 60 seconds for prefix
        // changes (ISP renumbering, RA changes after AP reconnect).
    if( ! localIPv6.isSet() || (UTCtime() - ipv6LastPoll) >= 60){
      ipv6LastPoll = UTCtime();
      for (auto entry : addrList){
        if(entry.isV6() && !entry.isLocal() && entry.ifUp()){
          IPAddress current = entry.addr();
          if(current != localIPv6){
            localIPv6 = current;
            log("WiFi: IPv6 global address %s", localIPv6.toString().c_str());
          }
          break;
        }
      }
    }
#endif
  }
  else {
    trace(T_WiFi,2);
    if(wifiConnectTime){
      trace(T_WiFi,2);
      wifiConnectTime = 0;
      lastDisconnect = UTCtime();
#if LWIP_IPV6
      localIPv6 = IPAddress();                      // Re-detect (and re-log) after reconnect:
      ipv6LastPoll = 0;                             // SLAAC recovery is not event-driven
#endif
      log("WiFi disconnected.");
    }
    else if((UTCtime() - lastDisconnect) >= restartInterval){
      log("WiFi disconnected more than %d minutes, restarting.", restartInterval / 60);
      delay(500);
      ESP.restart();
    }
  }

    // Check for degraded heap.

  trace(T_WiFi,10);
  if(ESP.getFreeHeap() < 7000){
    trace(T_WiFi,10);
    log("Heap memory has degraded below safe minimum, restarting.");
    delay(500);
    ESP.restart();
  }

      // Check for expired HTTP request.

  trace(T_WiFi,20);
  for(int i=0; i<HTTPrequestMax; i++){
    trace(T_WiFi,21,i);
    if(HTTPrequestStart[i] && (millis() - HTTPrequestStart[i]) > 900000UL){
      trace(T_WiFi,22,i);
      log("Incomplete HTTP request detected, id %d, restarting.", HTTPrequestId[i]);
      delay(500);
      ESP.restart();
    }
  }    

      // Purge any timed out authorization sessions.

  trace(T_WiFi,30);
  purgeAuthSessions();
 
  trace(T_WiFi,99);
  return UTCtime() + 1;  
}

uint32_t HTTPreserve(uint16_t id, bool lock){
  trace(T_WiFi,100,id);
  if(HTTPrequestFree == 0 || HTTPlock) return 0;
  HTTPrequestFree--;
  for(int i=0; i<HTTPrequestMax; i++){
    trace(T_WiFi,101,i);
    if(HTTPrequestStart[i] == 0){
      HTTPrequestStart[i] = millis();
      HTTPrequestId[i] = id;
      if(lock){
        HTTPlock = HTTPrequestStart[i];
      }
      return HTTPrequestStart[i];
    }
  }
  return 0;
}

void HTTPrelease(uint32_t HTTPtoken){
  trace(T_WiFi,110);
  for(int i=0; i<HTTPrequestMax; i++){
    trace(T_WiFi,110,i);
    if(HTTPrequestStart[i] == HTTPtoken){
      HTTPrequestStart[i] = 0;
      HTTPrequestFree++;
      if(HTTPtoken == HTTPlock){
        HTTPlock = 0;
      }
    }
  }
}