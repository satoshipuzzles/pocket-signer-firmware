#include "usb_net.h"

#include "esp32-hal-tinyusb.h"  // tinyusb_enable_interface + tusb.h
#include "device/usbd_pvt.h"    // usbd_class_driver_t (app driver registration)
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"

// ---------------------------------------------------------------------------
// Vendored CDC-ECM class driver (ecm_device.c). The Arduino core's prebuilt
// TinyUSB only bakes in the NCM network driver, but iOS only brings up
// CDC-ECM gadgets — so we carry our own ECM driver and register it through
// TinyUSB's app-driver hook. Symbols are prefixed ecm_ to avoid colliding
// with the NCM driver inside libarduino_tinyusb.a.
// ---------------------------------------------------------------------------
extern "C" {
void     ecm_netd_init(void);
bool     ecm_netd_deinit(void);
void     ecm_netd_reset(uint8_t rhport);
uint16_t ecm_netd_open(uint8_t rhport, tusb_desc_interface_t const* itf_desc, uint16_t max_len);
bool     ecm_netd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request);
bool     ecm_netd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void     ecm_network_recv_renew(void);
bool     ecm_network_can_xmit(uint16_t size);
void     ecm_network_xmit(void* ref, uint16_t arg);
void     ecm_netd_announce(void);
}

extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
  static const usbd_class_driver_t ecm_driver = {
    .name = "ECM",
    .init = ecm_netd_init,
    .deinit = ecm_netd_deinit,
    .reset = ecm_netd_reset,
    .open = ecm_netd_open,
    .control_xfer_cb = ecm_netd_control_xfer_cb,
    .xfer_cb = ecm_netd_xfer_cb,
    .xfer_isr = nullptr,
    .sof = nullptr,
  };
  *driver_count = 1;
  return &ecm_driver;
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static esp_netif_t* s_netif = nullptr;
static volatile bool s_link_up = false;
static volatile bool s_data_itf_active = false;  // host selected the data alt
static volatile uint32_t s_rx = 0, s_tx = 0;

// MAC referenced by the ECM driver; also rendered into the mandatory MAC
// string descriptor. Must differ from the MAC our own lwIP netif uses
// (two ends of the same wire can't share an address).
uint8_t tud_network_mac_address[6] = {0};

static void compute_macs(uint8_t dev_mac[6], uint8_t host_mac[6]) {
  uint8_t base[6];
  esp_read_mac(base, ESP_MAC_WIFI_STA);
  base[0] |= 0x02;  // locally administered, keeps us off real OUI space
  memcpy(dev_mac, base, 6);
  memcpy(host_mac, base, 6);
  host_mac[5] ^= 0x01;
}

// ---------------------------------------------------------------------------
// USB descriptor — registered before USB.begin() via global constructor
// ---------------------------------------------------------------------------
extern "C" uint16_t tusb_ecm_load_descriptor(uint8_t* dst, uint8_t* itf) {
  uint8_t dev_mac[6];
  compute_macs(dev_mac, tud_network_mac_address);

  // static: the HAL stores the *pointer* to string descriptors, and the host
  // fetches them long after this callback returns. A stack buffer here means
  // a garbage MAC string — which makes Apple's ECM driver silently refuse to
  // publish the network interface.
  static char mac_str[13];
  snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
           tud_network_mac_address[0], tud_network_mac_address[1],
           tud_network_mac_address[2], tud_network_mac_address[3],
           tud_network_mac_address[4], tud_network_mac_address[5]);

  uint8_t str_idx = tinyusb_add_string_descriptor("Signer Link");
  uint8_t mac_idx = tinyusb_add_string_descriptor(mac_str);

  uint8_t ep_notif = tinyusb_get_free_in_endpoint();
  TU_VERIFY(ep_notif != 0);
  uint8_t ep_data = tinyusb_get_free_duplex_endpoint();
  TU_VERIFY(ep_data != 0);

  uint8_t descriptor[TUD_CDC_ECM_DESC_LEN] = {
    TUD_CDC_ECM_DESCRIPTOR(*itf, str_idx, mac_idx,
                           (uint8_t)(0x80 | ep_notif), 64,
                           ep_data, (uint8_t)(0x80 | ep_data),
                           CFG_TUD_NET_ENDPOINT_SIZE, CFG_TUD_NET_MTU)
  };
  *itf += 2;  // ECM claims a control + a data interface
  memcpy(dst, descriptor, TUD_CDC_ECM_DESC_LEN);
  return TUD_CDC_ECM_DESC_LEN;
}

namespace {
struct EcmRegistrar {
  EcmRegistrar() {
    tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_ECM_DESC_LEN,
                             tusb_ecm_load_descriptor);
  }
};
EcmRegistrar s_registrar;  // runs before app_main() -> USB.begin()
}  // namespace

// ---------------------------------------------------------------------------
// TinyUSB network callbacks (device -> lwIP inbound path)
// ---------------------------------------------------------------------------
extern "C" bool tud_network_recv_cb(const uint8_t* src, uint16_t size) {
  if (s_netif && size) {
    // Copy: lwIP may hold the frame past this callback (zero-copy pbuf path),
    // and TinyUSB reuses its buffer as soon as we renew below. esp_netif
    // releases our copy through driver_free_rx_buffer.
    void* buf = malloc(size);
    if (buf) {
      memcpy(buf, src, size);
      if (esp_netif_receive(s_netif, buf, size, nullptr) == ESP_OK) {
        s_rx = s_rx + 1;
      }
    }
  }
  ecm_network_recv_renew();
  return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg) {
  memcpy(dst, ref, arg);  // synchronous copy inside ecm_network_xmit()
  return arg;
}

// The ECM driver fires this when the host activates the data interface —
// our reliable "the host actually opened the network" signal.
extern "C" void tud_network_init_cb(void) {
  s_data_itf_active = true;
}

// ---------------------------------------------------------------------------
// esp_netif driver (lwIP -> device outbound path)
// ---------------------------------------------------------------------------
static esp_err_t ecm_transmit(void* h, void* buffer, size_t len) {
  (void)h;
  if (!s_link_up || !tud_ready()) return ESP_FAIL;
  const uint32_t t0 = millis();
  while (!ecm_network_can_xmit((uint16_t)len)) {
    if (millis() - t0 > 50) return ESP_FAIL;  // host stalled; drop the frame
    vTaskDelay(1);
  }
  ecm_network_xmit(buffer, (uint16_t)len);
  s_tx = s_tx + 1;
  return ESP_OK;
}

static void ecm_free_rx(void* h, void* buffer) {
  (void)h;
  free(buffer);
}

typedef struct {
  esp_netif_driver_base_t base;
} ecm_driver_glue_t;
static ecm_driver_glue_t s_driver;

static esp_err_t ecm_post_attach(esp_netif_t* netif, void* args) {
  ecm_driver_glue_t* drv = (ecm_driver_glue_t*)args;
  drv->base.netif = netif;
  esp_netif_driver_ifconfig_t ifcfg = {};
  ifcfg.handle = drv;
  ifcfg.transmit = ecm_transmit;
  ifcfg.driver_free_rx_buffer = ecm_free_rx;
  return esp_netif_set_driver_config(netif, &ifcfg);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace usbnet {

bool begin() {
  if (esp_netif_init() != ESP_OK) return false;
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

  static esp_netif_ip_info_t ip_info;
  IP4_ADDR(&ip_info.ip, 10, 77, 7, 1);
  IP4_ADDR(&ip_info.gw, 0, 0, 0, 0);
  IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

  esp_netif_inherent_config_t base_cfg = {};
  base_cfg.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
  base_cfg.ip_info = &ip_info;
  base_cfg.if_key = "USBECM";
  base_cfg.if_desc = "usb-ecm";
  base_cfg.route_prio = 10;

  esp_netif_config_t cfg = {};
  cfg.base = &base_cfg;
  cfg.driver = nullptr;
  cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;

  s_netif = esp_netif_new(&cfg);
  if (!s_netif) {
    Serial.println("[usbnet] esp_netif_new failed");
    return false;
  }

  s_driver.base.post_attach = ecm_post_attach;
  if (esp_netif_attach(s_netif, &s_driver) != ESP_OK) {
    Serial.println("[usbnet] esp_netif_attach failed");
    return false;
  }

  uint8_t dev_mac[6], host_mac[6];
  compute_macs(dev_mac, host_mac);
  esp_netif_set_mac(s_netif, dev_mac);

  // No router / no DNS in the DHCP offer: the host keeps its own default
  // route and resolver; only our /24 rides the cable.
  uint8_t opt = 0;
  esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                         ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &opt, sizeof(opt));
  esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                         ESP_NETIF_DOMAIN_NAME_SERVER, &opt, sizeof(opt));

  esp_netif_action_start(s_netif, nullptr, 0, nullptr);
  Serial.println("[usbnet] ECM netif up, DHCP server on 10.77.7.1/24");
  return true;
}

void tick() {
  if (!s_netif) return;
  const bool up = tud_ready();
  static uint32_t link_t0 = 0;
  if (up && !s_link_up) {
    s_link_up = true;
    link_t0 = millis();
    esp_netif_action_connected(s_netif, nullptr, 0, nullptr);
    Serial.println("[usbnet] link UP");
  } else if (!up && s_link_up) {
    s_link_up = false;
    s_data_itf_active = false;
    esp_netif_action_disconnected(s_netif, nullptr, 0, nullptr);
    Serial.println("[usbnet] link DOWN");
  }

  // Keep announcing "network connected" until the host actually sends us a
  // frame. The one-shot notify the class driver sends during enumeration is
  // easy to lose, and without it macOS/iOS leave the interface on
  // "media inactive", never select the data alt-setting, and never DHCP.
  static uint32_t last_announce = 0;
  if (s_link_up && s_rx == 0 && millis() - last_announce > 1000) {
    last_announce = millis();
    ecm_netd_announce();
  }

  // Last-resort self-heal: if the host never sends a single frame, the
  // connected-notify was lost in a way announces can't fix. Re-enumerating
  // is exactly what a cable replug does, and a replug always fixes it.
  static uint8_t rebounds = 0;
  if (s_link_up && s_rx == 0 && rebounds < 3 && millis() - link_t0 > 8000) {
    rebounds++;
    Serial.printf("[usbnet] no host traffic %lus after link — re-enumerating (%u/3)\n",
                  (unsigned long)((millis() - link_t0) / 1000), rebounds);
    tud_disconnect();
    delay(400);
    tud_connect();
    link_t0 = millis();
  }
  if (s_rx > 0) rebounds = 0;
}

bool mounted() { return tud_mounted(); }
bool linkUp() { return s_link_up && s_data_itf_active; }
bool dataActive() { return s_data_itf_active; }
IPAddress ip() { return IPAddress(10, 77, 7, 1); }
uint32_t rxPackets() { return s_rx; }
uint32_t txPackets() { return s_tx; }

}  // namespace usbnet
