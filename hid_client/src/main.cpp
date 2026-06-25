#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// HID keyboard descriptor (standard boot keyboard, no report ID)
uint8_t const desc_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
Adafruit_USBD_HID usb_hid(desc_hid_report, sizeof(desc_hid_report),
                           HID_ITF_PROTOCOL_KEYBOARD, 2, false);

// Hardware UART to ESP32-C3 (GP0=TX, GP1=RX on RP2040-Zero)
#define ESP_UART  Serial1
#define UART_BAUD 115200

// Cached fingerprint result — set once in setup(), re-broadcast every 5 s in loop()
static String g_osTag      = "UNKNOWN";
static String g_confidence = "0";
static void broadcastStatus();

// ---------------------------------------------------------------------------
// Fingerprint state — written by USB callbacks (interrupt context)
// ---------------------------------------------------------------------------

// MS OS 2.0 descriptor request: Windows-ONLY signal, fires during enumeration.
// Linux and macOS never send this unless explicitly configured to do so.
static volatile bool fp_is_windows = false;

// LED output report timing — Windows pushes LED state fast; Linux syncs from
// X11 within ~1s; macOS rarely sends unsolicited LED updates.
static volatile uint32_t fp_led_count     = 0;
static volatile uint32_t fp_first_led_ms  = 0;

// ---------------------------------------------------------------------------
// BOS descriptor — advertises MS OS 2.0 capability so Windows requests it.
// Without this, Windows won't ask and we can't detect the request.
// ---------------------------------------------------------------------------
static uint8_t const desc_bos[] = {
    // BOS header (5 bytes)
    0x05, 0x0F,        // bLength, bDescriptorType = BOS (0x0F)
    0x21, 0x00,        // wTotalLength = 33 (5 + 28)
    0x01,              // bNumDeviceCaps = 1

    // MS OS 2.0 Platform Capability Descriptor (28 bytes)
    0x1C,              // bLength = 28
    0x10,              // bDescriptorType = Device Capability (0x10)
    0x05,              // bDevCapabilityType = Platform (0x05)
    0x00,              // bReserved
    // PlatformCapabilityUUID: D8DD60DF-4589-4CC7-9CD2-659D9E648A9F (little-endian)
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,  // dwWindowsVersion = 0x06030000 (Windows 8.1 minimum)
    0x0A, 0x00,              // wMSOSDescriptorSetTotalLength = 10
    0x01,                    // bMS_VendorCode — we respond to bRequest=0x01
    0x00                     // bAltEnumCmd
};

// Minimal valid MS OS 2.0 descriptor set (header only — no special driver binding)
static uint8_t const desc_ms_os_20[] = {
    0x0A, 0x00,              // wLength = 10
    0x00, 0x00,              // wDescriptorType = MS_OS_20_SET_HEADER_DESCRIPTOR
    0x00, 0x00, 0x03, 0x06,  // dwWindowsVersion = 0x06030000
    0x0A, 0x00               // wTotalLength = 10
};

// ---------------------------------------------------------------------------
// TinyUSB callbacks
// ---------------------------------------------------------------------------

// Provide BOS descriptor to host during enumeration
uint8_t const* tud_descriptor_bos_cb(void) {
    return desc_bos;
}

// Windows sends bRequest=0x01, wIndex=0x0007 to retrieve the MS OS 2.0 set.
// macOS and Linux do not. This is the most reliable Windows discriminator.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                 tusb_control_request_t const* request) {
    if (request->bRequest == 0x01 && request->wIndex == 0x0007) {
        fp_is_windows = true;
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_xfer(rhport, request,
                                    (void*)desc_ms_os_20, sizeof(desc_ms_os_20));
        }
        return true;
    }
    return false;
}

// LED/output reports registered via setReportCallback() to avoid linker conflict
// with the symbol already defined in Adafruit_USBD_HID.cpp
static void onHidSetReport(uint8_t report_id, hid_report_type_t report_type,
                            uint8_t const* buffer, uint16_t bufsize) {
    (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
    if (!fp_first_led_ms) fp_first_led_ms = millis();
    fp_led_count++;
}

// ---------------------------------------------------------------------------
// OS Fingerprinting
// Returns "OS:score" e.g. "WIN:7", "LINUX:3", "UNKNOWN:0"
// ---------------------------------------------------------------------------
String fingerprintOS() {
    uint32_t boot_ms = millis();

    // Wait for USB enumeration. Allow 15s for Windows first-time driver install.
    while (!TinyUSBDevice.mounted()) {
        if (millis() - boot_ms > 15000) return "UNKNOWN:0";
        delay(10);
    }
    uint32_t mount_elapsed = millis() - boot_ms;

    // Observe 2.5 s post-mount for LED signal (MS OS 2.0 fires before mount)
    delay(2500);

    // --- Confidence scoring (0=WIN, 1=LINUX, 2=MAC) ---
    int scores[3] = {0, 0, 0};

    // Signal 1 — MS OS 2.0 vendor request (weight 6, definitive Windows)
    // Fires during enumeration before TinyUSBDevice.mounted() is true.
    if (fp_is_windows) scores[0] += 6;

    // Signal 2 — Enumeration timing (weight 2)
    // Only meaningful for Linux vs macOS when MS OS 2.0 didn't fire.
    if (mount_elapsed < 2000) {
        scores[1] += 2;     // Linux enumerates faster than macOS
    } else {
        scores[2] += 2;     // macOS tends to be slower
    }
    // Windows with installed driver is also fast, but MS OS 2.0 already covers it.
    if (mount_elapsed < 800 && !fp_is_windows) {
        scores[0] += 1;     // Weak fallback if BOS/vendor path somehow missed
    }

    // Signal 3 — LED report timing/presence (weight 2 / 1)
    if (fp_led_count > 0) {
        uint32_t led_delay = fp_first_led_ms - boot_ms;
        if (led_delay < 800) {
            scores[0] += 2;     // Windows sends LED state very quickly
            scores[1] += 1;     // Linux X11 can also be fast — give partial credit
        } else if (led_delay < 2000) {
            scores[1] += 2;     // Linux X11 sync window
        }
    } else {
        scores[2] += 1;         // macOS sends fewer unsolicited LED updates
    }

    int best = 0;
    for (int i = 1; i < 3; i++) {
        if (scores[i] > scores[best]) best = i;
    }
    if (scores[best] == 0) return "UNKNOWN:0";

    const char* labels[] = {"WIN", "LINUX", "MAC"};
    return String(labels[best]) + ":" + String(scores[best]);
}

// ---------------------------------------------------------------------------
// HID Keyboard Helpers
// ---------------------------------------------------------------------------

struct KeyMapping { uint8_t mod; uint8_t key; };

static KeyMapping charToHID(char c) {
    if (c >= 'a' && c <= 'z') return {0x00, (uint8_t)(0x04 + c - 'a')};
    if (c >= 'A' && c <= 'Z') return {0x02, (uint8_t)(0x04 + c - 'A')};
    if (c >= '1' && c <= '9') return {0x00, (uint8_t)(0x1E + c - '1')};
    if (c == '0') return {0x00, 0x27};
    switch (c) {
        case ' ':  return {0x00, 0x2C};
        case '\n': return {0x00, 0x28};
        case '\t': return {0x00, 0x2B};
        case '-':  return {0x00, 0x2D};
        case '_':  return {0x02, 0x2D};
        case '=':  return {0x00, 0x2E};
        case '+':  return {0x02, 0x2E};
        case '[':  return {0x00, 0x2F};
        case '{':  return {0x02, 0x2F};
        case ']':  return {0x00, 0x30};
        case '}':  return {0x02, 0x30};
        case '\\': return {0x00, 0x31};
        case '|':  return {0x02, 0x31};
        case ';':  return {0x00, 0x33};
        case ':':  return {0x02, 0x33};
        case '\'': return {0x00, 0x34};
        case '"':  return {0x02, 0x34};
        case '`':  return {0x00, 0x35};
        case '~':  return {0x02, 0x35};
        case ',':  return {0x00, 0x36};
        case '<':  return {0x02, 0x36};
        case '.':  return {0x00, 0x37};
        case '>':  return {0x02, 0x37};
        case '/':  return {0x00, 0x38};
        case '?':  return {0x02, 0x38};
        case '!':  return {0x02, 0x1E};
        case '@':  return {0x02, 0x1F};
        case '#':  return {0x02, 0x20};
        case '$':  return {0x02, 0x21};
        case '%':  return {0x02, 0x22};
        case '^':  return {0x02, 0x23};
        case '&':  return {0x02, 0x24};
        case '*':  return {0x02, 0x25};
        case '(':  return {0x02, 0x26};
        case ')':  return {0x02, 0x27};
        default:   return {0x00, 0x00};
    }
}

static void sendKey(uint8_t modifier, uint8_t keycode) {
    if (!TinyUSBDevice.mounted()) return;
    while (!usb_hid.ready()) delay(1);
    uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
    usb_hid.keyboardReport(0, modifier, keycodes);
    delay(20);
    while (!usb_hid.ready()) delay(1);
    usb_hid.keyboardRelease(0);
    delay(20);
}

static void typeString(const String& text) {
    for (size_t i = 0; i < (size_t)text.length(); i++) {
        KeyMapping km = charToHID(text[i]);
        if (km.key) sendKey(km.mod, km.key);
        delay(10);
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    TinyUSBDevice.setVersion(0x0201); // USB 2.1 — required for host to request BOS descriptor
    TinyUSBDevice.setManufacturerDescriptor("Generic Corp");
    TinyUSBDevice.setProductDescriptor("USB Keyboard");

    usb_hid.setReportCallback(NULL, onHidSetReport);
    usb_hid.begin();

    ESP_UART.begin(UART_BAUD);

    String result     = fingerprintOS();
    int    sep        = result.indexOf(':');
    g_osTag      = result.substring(0, sep);
    g_confidence = result.substring(sep + 1);

    broadcastStatus();

    // Request autorun payload from ESP32 for detected OS
    delay(2000);
    ESP_UART.println("[REQUEST_AUTORUN]: " + g_osTag);
}

// ---------------------------------------------------------------------------
// Main loop — command dispatch + periodic status re-broadcast
// ---------------------------------------------------------------------------

static void broadcastStatus() {
    for (int i = 0; i < 4; i++) {
        ESP_UART.println("[DETECTED_OS]: " + g_osTag);
        delay(100);
    }
    ESP_UART.println("[OS_CONFIDENCE]: " + g_confidence);
}

void loop() {
    static uint32_t last_broadcast = 0;
    if (millis() - last_broadcast >= 5000) {
        broadcastStatus();
        last_broadcast = millis();
    }

    if (!ESP_UART.available()) {
        delay(10);
        return;
    }

    String cmd = ESP_UART.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("TYPE ")) {
        typeString(cmd.substring(5));
    } else if (cmd == "PRESS ENTER")    { sendKey(0x00, 0x28); }
    else if (cmd == "PRESS GUI")        { sendKey(0x08, 0x00); }  // Win/Cmd alone
    else if (cmd == "PRESS WIN_R")      { sendKey(0x08, 0x15); }  // Run dialog
    else if (cmd == "PRESS WIN_L")      { sendKey(0x08, 0x0F); }  // Lock screen
    else if (cmd == "PRESS WIN_E")      { sendKey(0x08, 0x08); }  // Explorer
    else if (cmd == "PRESS WIN_D")      { sendKey(0x08, 0x07); }  // Show desktop
    else if (cmd == "PRESS WIN_X")      { sendKey(0x08, 0x1B); }  // Power User menu
    else if (cmd == "PRESS CMD_SPACE")  { sendKey(0x08, 0x2C); }  // Spotlight
    else if (cmd == "PRESS CMD_T")      { sendKey(0x08, 0x17); }  // New tab/terminal
    else if (cmd == "PRESS ESC")        { sendKey(0x00, 0x29); }
    else if (cmd == "PRESS TAB")        { sendKey(0x00, 0x2B); }
    else if (cmd == "DELAY")            { sendKey(0x08, 0x2C); }  // legacy CMD_SPACE alias
}
