// USB-NCM "Ethernet gadget" glue for ESP32-S3 (Arduino core 3.x).
//
// Enumerates the device as a CDC-NCM USB network adapter. macOS, iOS,
// Linux, and Android bring the interface up natively (no driver install).
// A private two-node subnet runs over the cable:
//
//     device  10.77.7.1   <—USB-C—>   host  10.77.7.2 (via our DHCP)
//
// The DHCP offer deliberately carries no router and no DNS, so the host
// keeps its own default route (a phone keeps cellular/WiFi internet) and
// only 10.77.7.0/24 traffic crosses the cable. Unplug = the signer's
// network ceases to exist.
//
// Requires FQBN option USBMode=default (USB-OTG / TinyUSB). Interface
// registration happens in a global constructor, before USB.begin().
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

namespace usbnet {

// Create the lwIP interface + DHCP server. Call once in setup().
bool begin();

// Poll USB/link state; call every loop() iteration.
void tick();

bool mounted();       // USB enumerated by a host
bool linkUp();        // ECM link announced up (host can reach us)
bool dataActive();    // host selected the ECM data alt-setting
IPAddress ip();       // our address on the USB subnet
uint32_t rxPackets();
uint32_t txPackets();

} // namespace usbnet
