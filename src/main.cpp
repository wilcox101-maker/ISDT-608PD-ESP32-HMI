#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;

static const char* SERVICE_UUID = "0000af00-0000-1000-8000-00805f9b34fb";
static const char* CHAR_AF01    = "0000af01-0000-1000-8000-00805f9b34fb";
static const char* CHAR_AF02    = "0000af02-0000-1000-8000-00805f9b34fb";

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pAF01 = nullptr;
BLERemoteCharacteristic* pAF02 = nullptr;
volatile bool bleConnected = false;
uint8_t clientUUID[16];
volatile unsigned long lastPoll = 0;
volatile unsigned long lastReconnect = 0;
volatile int reconnectFails = 0;
SemaphoreHandle_t bleMutex = nullptr;

struct ChargerData {
    float packV = 0.0f;
    float currentA = 0.0f;
    float cells[6] = {0};
    uint8_t state = 0;
    uint8_t capacity = 0;
    uint16_t errorCode = 0;
    String status = "Searching...";
    bool valid = false;
    unsigned long lastData = 0;
} charger;

#define LOG_ROWS 16
struct OpEntry {
    uint32_t ts;
    uint8_t  op;
    uint8_t  len;
    uint16_t key;
    char     note[8];
};
OpEntry opLog[LOG_ROWS];
int opHead = 0;
int opCount = 0;
SemaphoreHandle_t logMutex = nullptr;

void addOp(uint8_t op, uint8_t len, uint16_t key, const char* note) {
    if (!logMutex || xSemaphoreTake(logMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    opLog[opHead].ts = millis() / 1000;
    opLog[opHead].op = op;
    opLog[opHead].len = len;
    opLog[opHead].key = key;
    strncpy(opLog[opHead].note, note ? note : "", 7);
    opLog[opHead].note[7] = '\0';
    opHead = (opHead + 1) % LOG_ROWS;
    if (opCount < LOG_ROWS) opCount++;
    xSemaphoreGive(logMutex);
}

lv_obj_t *scr_main = nullptr;
lv_obj_t *scr_menu = nullptr;
lv_obj_t *scr_log  = nullptr;
lv_obj_t *scr_control = nullptr;
lv_obj_t *scr_sys = nullptr;
lv_obj_t *lbl_status, *lbl_pack, *lbl_curr, *lbl_err;
lv_obj_t *lbl_ctrl_status;
lv_obj_t *lbl_type, *lbl_cells, *lbl_cap, *lbl_power, *lbl_temp;
lv_obj_t *log_table;
int backlightLevel = 1;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
const char* AP_SSID = "ISDT-608PD";
const char* AP_PASS = "isdt608pd";

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ISDT 608PD</title>
<style>
body{background:#111;color:#eee;font-family:monospace;margin:6px}
.status{font-size:18px;color:#0f0}
table{border-collapse:collapse;width:100%;font-size:12px;margin-top:8px}
td,th{border:1px solid #555;padding:2px 4px;text-align:center}
.bar{height:10px;background:#0a0;display:inline-block}
</style></head><body>
<div class="status" id="status">...</div>
<div>Pack <span id="pack">-</span> V &nbsp; I <span id="i">-</span> A &nbsp; State <span id="state">-</span> &nbsp; Cap <span id="cap">-</span>%</div>
<div id="cells"></div>
<table id="log"><tr><th>s</th><th>Op</th><th>Len</th><th>Key</th><th>Note</th></tr></table>
<script>
const ws = new WebSocket('ws://' + location.host + '/ws');
ws.onmessage = e => {
  const d = JSON.parse(e.data);
  document.getElementById('status').textContent = d.status || '';
  document.getElementById('pack').textContent = (d.pack || 0).toFixed(2);
  document.getElementById('i').textContent = (d.current || 0).toFixed(2);
  document.getElementById('state').textContent = d.state || 0;
  document.getElementById('cap').textContent = d.cap || 0;
  let html = '';
  const cells = d.cells || [];
  for (let i = 0; i < 6; i++) {
    const v = cells[i] || 0;
    html += `<div>C${i+1} <div class="bar" style="width:${Math.min(v,100)}px"></div> ${(v/100).toFixed(2)}V</div>`;
  }
  document.getElementById('cells').innerHTML = html;
  let rows = '<tr><th>s</th><th>Op</th><th>Len</th><th>Key</th><th>Note</th></tr>';
  (d.log || []).forEach(r => {
    rows += `<tr><td>${r.ts}</td><td>${r.op.toString(16).toUpperCase()}</td><td>${r.len}</td><td>${r.key}</td><td>${r.note}</td></tr>`;
  });
  document.getElementById('log').innerHTML = rows;
};
</script></body></html>
)rawliteral";

void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* data, size_t len, bool isNotify);
void bleTask(void *param);

void generateUUID() {
    for (int i = 0; i < 16; i++) clientUUID[i] = (uint8_t)random(0, 256);
}

bool doBind() {
    if (!pAF02) return false;
    uint8_t pkt[19] = {0x18};
    memcpy(&pkt[1], clientUUID, 16);
    pkt[17] = 0;
    pkt[18] = 0;
    pAF02->writeValue(pkt, 19, true);
    vTaskDelay(pdMS_TO_TICKS(150));
    addOp(0x18, 19, 0, "BIND");
    return true;
}

void connectTo(BLEAddress addr) {
    if (!bleMutex || xSemaphoreTake(bleMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (pClient) {
        pClient->disconnect();
        delete pClient;
        pClient = nullptr;
        pAF01 = nullptr;
        pAF02 = nullptr;
    }
    pClient = BLEDevice::createClient();
    if (!pClient->connect(addr)) {
        charger.status = "Connect fail";
        reconnectFails++;
        addOp(0x00, 0, 0, "FAIL");
        xSemaphoreGive(bleMutex);
        return;
    }
    bleConnected = true;
    reconnectFails = 0;
    charger.status = "Connected";

    BLERemoteService* svc = pClient->getService(SERVICE_UUID);
    if (!svc) {
        bleConnected = false;
        xSemaphoreGive(bleMutex);
        return;
    }

    pAF02 = svc->getCharacteristic(CHAR_AF02);
    pAF01 = svc->getCharacteristic(CHAR_AF01);
    if (pAF01) pAF01->registerForNotify(notifyCallback);
    doBind();
    xSemaphoreGive(bleMutex);
}

void scanAndConnect() {
    BLEScan* scan = BLEDevice::getScan();
    if (!scan) return;
    scan->setActiveScan(true);
    BLEScanResults res = scan->start(4, false);
    for (int i = 0; i < res.getCount(); i++) {
        BLEAdvertisedDevice d = res.getDevice(i);
        if (!d.haveManufacturerData()) continue;
        std::string md = d.getManufacturerData();
        if (md.length() < 6) continue;
        uint16_t company = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
        if (company == 0xABBA) {
            connectTo(d.getAddress());
            return;
        }
    }
    charger.status = "608PD not found";
    addOp(0x00, 0, 0, "NOSCAN");
}

static uint32_t readLE32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t readLE16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* data, size_t len, bool isNotify) {
    if (len < 3 || data[0] != 0x31) {
        addOp(0xFF, (uint8_t)len, 0, "BAD");
        return;
    }
    uint8_t cmd = data[1];
    uint16_t key = (len > 4) ? readLE16(data + 3) : 0;
    const char* note = "RX";
    if (cmd == 0xE5) note = "Elec";
    else if (cmd == 0xE7) note = "Work";
    addOp(cmd, (uint8_t)len, key, note);

    charger.lastData = millis();
    charger.valid = true;

    if (cmd == 0xE5) {
        if (len > 35) {
            if (len >= 19) {
                charger.packV = readLE32(data + 11) / 1000.0f;
                charger.currentA = readLE32(data + 15) / 1000.0f;
            }
            if (len >= 31) {
                for (int i = 0; i < 6; i++) {
                    charger.cells[i] = readLE16(data + 19 + i * 2) / 1000.0f;
                }
            }
        } else if (len >= 15) {
            charger.packV = readLE16(data + 9) / 1000.0f;
            charger.currentA = readLE32(data + 11) / 1000.0f;
            if (len >= 27) {
                for (int i = 0; i < 6; i++) {
                    charger.cells[i] = readLE16(data + 15 + i * 2) / 1000.0f;
                }
            }
        }
    } else if (cmd == 0xE7 && len >= 6) {
        charger.state = data[3];
        charger.capacity = data[4];
        if (len >= 38) charger.errorCode = readLE16(data + 36);
    }
}

void bleTask(void *param) {
    esp_task_wdt_add(NULL);
    while (true) {
        unsigned long now = millis();
        if (!bleConnected && now - lastReconnect > 6000) {
            lastReconnect = now;
            scanAndConnect();
        }
        if (bleConnected && pAF01 && now - lastPoll > 900) {
            if (bleMutex && xSemaphoreTake(bleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                uint8_t elec[] = {0x12, 0xE4, 0x00};
                pAF01->writeValue(elec, 3, true);
                vTaskDelay(pdMS_TO_TICKS(60));
                uint8_t work[] = {0x13, 0xE6, 0x00};
                pAF01->writeValue(work, 3, true);
                lastPoll = now;
                addOp(0xE4, 3, 0, "POLL");
                xSemaphoreGive(bleMutex);
            }
        }
        if (reconnectFails > 10) {
            charger.status = "BLE fault";
            reconnectFails = 0;
        }
        if (bleConnected && millis() - charger.lastData > 10000) {
            charger.status = "Data timeout";
            bleConnected = false;
            addOp(0x00, 0, 0, "TIMEOUT");
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void sendControl(uint8_t cmd, uint8_t param) {
    if (!bleConnected || !pAF01) return;
    uint8_t pkt[3] = {0x12, cmd, param};
    pAF01->writeValue(pkt, 3, true);
    addOp(cmd, 3, param, "CTRL");
}

void createMainScreen() {
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    lbl_type = lv_label_create(scr_main);
    lv_label_set_text(lbl_type, "LiPo");
    lv_obj_set_style_text_color(lbl_type, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(lbl_type, LV_ALIGN_TOP_LEFT, 10, 6);

    lbl_cells = lv_label_create(scr_main);
    lv_label_set_text(lbl_cells, "6S");
    lv_obj_set_style_text_color(lbl_cells, lv_color_hex(0x00CCFF), 0);
    lv_obj_align(lbl_cells, LV_ALIGN_TOP_LEFT, 68, 6);

    lbl_curr = lv_label_create(scr_main);
    lv_obj_set_style_text_color(lbl_curr, lv_color_hex(0xFFAA00), 0);
    lv_obj_align(lbl_curr, LV_ALIGN_TOP_RIGHT, -8, 6);

    lbl_cap = lv_label_create(scr_main);
    lv_obj_set_style_text_color(lbl_cap, lv_color_hex(0x00CCFF), 0);
    lv_obj_align(lbl_cap, LV_ALIGN_LEFT_MID, 10, -18);

    lbl_pack = lv_label_create(scr_main);
    lv_obj_set_style_text_color(lbl_pack, lv_color_white(), 0);
    lv_obj_align(lbl_pack, LV_ALIGN_RIGHT_MID, -8, -18);

    lbl_power = lv_label_create(scr_main);
    lv_obj_set_style_text_color(lbl_power, lv_color_hex(0xFFAA00), 0);
    lv_obj_align(lbl_power, LV_ALIGN_LEFT_MID, 10, 12);

    lbl_temp = lv_label_create(scr_main);
    lv_label_set_text(lbl_temp, "-- \u00B0C   -- \u00B0F");
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xFF66CC), 0);
    lv_obj_align(lbl_temp, LV_ALIGN_LEFT_MID, 10, 42);

    lbl_status = lv_label_create(scr_main);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 6, -26);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x888888), 0);

    lbl_err = lv_label_create(scr_main);
    lv_obj_align(lbl_err, LV_ALIGN_BOTTOM_LEFT, 6, -10);
    lv_obj_set_style_text_color(lbl_err, lv_color_hex(0xFF4444), 0);

    lv_obj_t *btn = lv_btn_create(scr_main);
    lv_obj_set_size(btn, 68, 24);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "MENU");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, [](lv_event_t*) { lv_scr_load(scr_menu); }, LV_EVENT_CLICKED, NULL);
}

void createMenuScreen() {
    scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_menu, lv_color_hex(0x1a1a1a), 0);
    lv_obj_t *t = lv_label_create(scr_menu);
    lv_label_set_text(t, "MENU");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

    const char* items[] = {"Status", "Opcode Log", "Control", "System Settings", "Reconnect"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_btn_create(scr_menu);
        lv_obj_set_size(b, 180, 30);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 28 + i * 34);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, items[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            intptr_t idx = (intptr_t)lv_event_get_user_data(e);
            if (idx == 0) lv_scr_load(scr_main);
            else if (idx == 1) lv_scr_load(scr_log);
            else if (idx == 2) lv_scr_load(scr_control);
            else if (idx == 3) lv_scr_load(scr_sys);
            else if (idx == 4) {
                bleConnected = false;
                scanAndConnect();
                lv_scr_load(scr_main);
            }
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

void createLogScreen() {
    scr_log = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_log, lv_color_hex(0x111111), 0);
    lv_obj_t *t = lv_label_create(scr_log);
    lv_label_set_text(t, "OPCODE LOG");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);

    log_table = lv_table_create(scr_log);
    lv_obj_set_size(log_table, 300, 170);
    lv_obj_align(log_table, LV_ALIGN_TOP_MID, 0, 22);
    lv_table_set_col_cnt(log_table, 5);
    lv_table_set_row_cnt(log_table, LOG_ROWS + 1);
    lv_table_set_cell_value(log_table, 0, 0, "s");
    lv_table_set_cell_value(log_table, 0, 1, "Op");
    lv_table_set_cell_value(log_table, 0, 2, "Len");
    lv_table_set_cell_value(log_table, 0, 3, "Key");
    lv_table_set_cell_value(log_table, 0, 4, "Note");

    lv_obj_t *back = lv_btn_create(scr_log);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_t *l = lv_label_create(back);
    lv_label_set_text(l, "BACK");
    lv_obj_center(l);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);
}

void createControlScreen() {
    scr_control = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_control, lv_color_hex(0x0a0a0a), 0);

    lv_obj_t *title = lv_label_create(scr_control);
    lv_label_set_text(title, "CONTROL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lbl_ctrl_status = lv_label_create(scr_control);
    lv_label_set_text(lbl_ctrl_status, "Ready");
    lv_obj_align(lbl_ctrl_status, LV_ALIGN_TOP_MID, 0, 22);

    const char* keys[] = {"Adjust -", "Confirm", "Adjust +"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(scr_control);
        lv_obj_set_size(b, 90, 36);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8 + i * 98, 48);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, keys[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            int k = (int)(intptr_t)lv_event_get_user_data(e);
            if (k == 0) sendControl(0x80, 0);
            else if (k == 1) sendControl(0x81, 0);
            else if (k == 2) sendControl(0x82, 0);
            lv_label_set_text(lbl_ctrl_status, "Command sent");
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    lv_obj_t *start = lv_btn_create(scr_control);
    lv_obj_set_size(start, 140, 34);
    lv_obj_align(start, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_t *sl = lv_label_create(start);
    lv_label_set_text(sl, "Start Task");
    lv_obj_center(sl);
    lv_obj_add_event_cb(start, [](lv_event_t*) {
        sendControl(0xA0, 0);
        lv_label_set_text(lbl_ctrl_status, "Start Task");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop = lv_btn_create(scr_control);
    lv_obj_set_size(stop, 140, 34);
    lv_obj_align(stop, LV_ALIGN_TOP_MID, 0, 142);
    lv_obj_t *stl = lv_label_create(stop);
    lv_label_set_text(stl, "Stop Task");
    lv_obj_center(stl);
    lv_obj_add_event_cb(stop, [](lv_event_t*) {
        sendControl(0xA1, 0);
        lv_label_set_text(lbl_ctrl_status, "Stop Task");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(scr_control);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "BACK");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);
}

void createSystemScreen() {
    scr_sys = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_sys, lv_color_hex(0x0a0a0a), 0);

    lv_obj_t *title = lv_label_create(scr_sys);
    lv_label_set_text(title, "SYSTEM SETTINGS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *bl_label = lv_label_create(scr_sys);
    lv_label_set_text(bl_label, "Backlight:");
    lv_obj_align(bl_label, LV_ALIGN_TOP_LEFT, 10, 36);

    const char* bl_opts[] = {"Low", "Med", "High"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(scr_sys);
        lv_obj_set_size(b, 70, 28);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 10 + i * 78, 58);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, bl_opts[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            backlightLevel = (int)(intptr_t)lv_event_get_user_data(e);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    lv_obj_t *save = lv_btn_create(scr_sys);
    lv_obj_set_size(save, 100, 30);
    lv_obj_align(save, LV_ALIGN_TOP_LEFT, 20, 110);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save");
    lv_obj_center(sl);
    lv_obj_add_event_cb(save, [](lv_event_t*) { lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = lv_btn_create(scr_sys);
    lv_obj_set_size(cancel, 100, 30);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -20, 110);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, [](lv_event_t*) { lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *about = lv_label_create(scr_sys);
    lv_label_set_text(about, "About\nISDT-608PD HMI v1.0\nESP32-S3");
    lv_obj_align(about, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *back = lv_btn_create(scr_sys);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "BACK");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);
}

void updateUI() {
    if (lv_scr_act() == scr_main) {
        lv_label_set_text(lbl_status, charger.status.c_str());
        lv_label_set_text_fmt(lbl_pack, "%.1f V", charger.packV);
        lv_label_set_text_fmt(lbl_curr, "%.1f A", charger.currentA);
        lv_label_set_text_fmt(lbl_cap, "%.2f Ah", (charger.capacity / 100.0f) * 5.0f);
        float watts = charger.packV * charger.currentA;
        lv_label_set_text_fmt(lbl_power, "%.1f V   %.1f W", charger.packV, watts);
        if (charger.errorCode)
            lv_label_set_text_fmt(lbl_err, "Err %04X", charger.errorCode);
        else
            lv_label_set_text(lbl_err, "");
    }
    if (lv_scr_act() == scr_log && logMutex) {
        if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            for (int r = 0; r < opCount && r < LOG_ROWS; r++) {
                int idx = (opHead - 1 - r + LOG_ROWS) % LOG_ROWS;
                char buf[12];
                snprintf(buf, sizeof(buf), "%lu", opLog[idx].ts);
                lv_table_set_cell_value(log_table, r + 1, 0, buf);
                snprintf(buf, sizeof(buf), "%02X", opLog[idx].op);
                lv_table_set_cell_value(log_table, r + 1, 1, buf);
                snprintf(buf, sizeof(buf), "%d", opLog[idx].len);
                lv_table_set_cell_value(log_table, r + 1, 2, buf);
                snprintf(buf, sizeof(buf), "%u", opLog[idx].key);
                lv_table_set_cell_value(log_table, r + 1, 3, buf);
                lv_table_set_cell_value(log_table, r + 1, 4, opLog[idx].note);
            }
            xSemaphoreGive(logMutex);
        }
    }
}

void pushWeb() {
    if (ws.count() == 0) return;
    JsonDocument doc;
    doc["status"] = charger.status;
    doc["pack"] = charger.packV;
    doc["current"] = charger.currentA;
    doc["state"] = charger.state;
    doc["cap"] = charger.capacity;
    JsonArray cells = doc["cells"].to<JsonArray>();
    for (int i = 0; i < 6; i++) cells.add((int)(charger.cells[i] * 100));
    JsonArray log = doc["log"].to<JsonArray>();
    if (logMutex && xSemaphoreTake(logMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        for (int i = 0; i < opCount; i++) {
            int idx = (opHead - 1 - i + LOG_ROWS) % LOG_ROWS;
            JsonObject e = log.add<JsonObject>();
            e["ts"] = opLog[idx].ts;
            e["op"] = opLog[idx].op;
            e["len"] = opLog[idx].len;
            e["key"] = opLog[idx].key;
            e["note"] = opLog[idx].note;
        }
        xSemaphoreGive(logMutex);
    }
    String out;
    serializeJson(doc, out);
    ws.textAll(out);
}

void setup() {
    Serial.begin(115200);
    randomSeed(esp_random());
    generateUUID();

    bleMutex = xSemaphoreCreateMutex();
    logMutex = xSemaphoreCreateMutex();

    size_t bufPixels = 320 * 40;
    if (psramFound()) {
        buf1 = (lv_color_t*)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf1) Serial.printf("PSRAM buffer: %u bytes\n", (unsigned)(bufPixels * sizeof(lv_color_t)));
    }
    if (!buf1) {
        bufPixels = 320 * 16;
        buf1 = (lv_color_t*)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        Serial.println("Using internal RAM buffer");
    }
    if (!buf1) {
        Serial.println("Buffer alloc failed");
        while (true) delay(1000);
    }

    tft.init();
    tft.setRotation(1);
    pinMode(45, OUTPUT);
    digitalWrite(45, HIGH);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, bufPixels);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 320;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    createMainScreen();
    createMenuScreen();
    createLogScreen();
    createControlScreen();
    createSystemScreen();
    lv_scr_load(scr_main);

    BLEDevice::init("ISDT-608PD-HMI");

    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 20000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&wdt_config);
    esp_task_wdt_add(NULL);

    xTaskCreatePinnedToCore(bleTask, "bleTask", 8192, NULL, 2, NULL, 1);

    WiFi.softAP(AP_SSID, AP_PASS);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });
    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType t, void*, uint8_t*, size_t) {
        if (t == WS_EVT_CONNECT) pushWeb();
    });
    server.addHandler(&ws);
    server.begin();
    Serial.println("WebUI: http://192.168.4.1");
}

void loop() {
    lv_timer_handler();
    delay(5);
    esp_task_wdt_reset();
    updateUI();
    if ((millis() % 500) < 8) pushWeb();
}
