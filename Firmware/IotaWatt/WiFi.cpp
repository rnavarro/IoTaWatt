#include "IotaWatt.h"
#include <lwip/netif.h>
#if LWIP_MDNS_RESPONDER
#include <lwip/apps/mdns.h>
#else
#include "ESP8266mDNS.h"
#endif
#include <ESP8266LLMNR.h>

#if LWIP_IPV6
/********************************************************************************************
 * preferredGlobalIPv6 - the station interface's usable global IPv6 address.
 *
 * Returns the first PREFERRED (DAD complete, not deprecated) non-link-local
 * address on the up station interface, or an unset IPAddress if none.
 * Address state matters: a TENTATIVE address (DAD still in flight) is not
 * yet usable, and a DEPRECATED one (prefix being retired via RA lifetimes)
 * is on its way out - neither should drive localIPv6, which feeds the
 * localAccess auth bypass in auth.cpp.
 *******************************************************************************************/
static IPAddress preferredGlobalIPv6(){
  for(netif* nif = netif_list; nif; nif = nif->next){
    if(nif->num != STATION_IF || !netif_is_up(nif)) continue;
    for(int s = 0; s < LWIP_IPV6_NUM_ADDRESSES; s++){
      if(ip6_addr_ispreferred(netif_ip6_addr_state(nif, s)) &&
         !ip6_addr_islinklocal(netif_ip6_addr(nif, s))){
        return IPAddress(netif_ip_addr6(nif, s));
      }
    }
  }
  return IPAddress();
}
#endif

/********************************************************************************************
 * wifiIsOperational - true when the network is usable.
 *
 * WiFi.status() only reports WL_CONNECTED when the SDK reaches STATION_GOT_IP,
 * which requires an IPv4 address (DHCP lease or static config). On an
 * IPv6-only network that never happens even though SLAAC connectivity is
 * fully usable, so every WL_CONNECTED gate in the firmware would see the
 * network as down forever (see esp8266/Arduino PR #5136 discussion - the
 * core never addressed this). Treat a usable (preferred, non-link-local)
 * address on the up station interface as operational too.
 *******************************************************************************************/

bool wifiIsOperational(){
  if(WiFi.status() == WL_CONNECTED){
    return true;
  }
#if LWIP_IPV6
  return preferredGlobalIPv6().isSet();
#else
  return false;
#endif
}

uint32_t WiFiService(struct serviceBlock* _serviceBlock) {
  static uint32_t lastDisconnect = UTCtime();       // Time of last disconnect
  const uint32_t restartInterval = 60*60;           // Restart if disconnected this many seconds
  static bool mDNSstarted = false;
  static bool LLMNRstarted = false;
#if LWIP_IPV6
  static uint32_t ipv6LastPoll = 0;                 // Last SLAAC poll time
#endif

  trace(T_WiFi,0);
  if(wifiIsOperational()){
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
#if LWIP_MDNS_RESPONDER
        // lwIP's native mDNS responder (compiled into the rebuilt
        // liblwip6, see scripts/build_lwip6_rdnss.sh). Unlike LEAmDNS it
        // answers over both IPv4 (224.0.0.251) and IPv6 (ff02::fb) and
        // serves A + AAAA records. Event-driven inside lwIP - no
        // update() polling.
    if( ! mDNSstarted){
      static bool mdnsInitDone = false;
      if( ! mdnsInitDone){
        mdns_resp_init();                           // Once per boot - a second call asserts
        mdnsInitDone = true;
      }
      if(netif_default && mdns_resp_add_netif(netif_default, deviceName, 3600) == ERR_OK){
        mdns_resp_add_service(netif_default, deviceName, "_http", DNSSD_PROTO_TCP, 80, 3600, NULL, NULL);
        mDNSstarted = true;
      }
    }
#else
    if( ! mDNSstarted){
      if (MDNS.begin(deviceName)) {
        MDNS.addService("http", "tcp", 80);
        mDNSstarted = true;
      }
    }
    else {
      MDNS.update();
    }
#endif
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
      IPAddress current = preferredGlobalIPv6();
      if(current.isSet() && current != localIPv6){
        localIPv6 = current;
        log("WiFi: IPv6 global address %s", localIPv6.toString().c_str());
#if LWIP_MDNS_RESPONDER
            // SLAAC addresses arrive after the netif was registered
            // with mDNS (and change on prefix renumbering). Our lwIP
            // build has no ext-status callback, so re-announce the
            // new AAAA explicitly.
        if(mDNSstarted && netif_default){
          mdns_resp_announce(netif_default);
        }
#endif
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
#if LWIP_MDNS_RESPONDER
      if(mDNSstarted && netif_default){
        mdns_resp_remove_netif(netif_default);      // Re-add (and re-probe) after reconnect
        mDNSstarted = false;
      }
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