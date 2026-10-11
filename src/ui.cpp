#include "ui.h"
#include <Arduino.h>

// Forward from main
extern struct ChargerData charger;
extern void sendControl(uint8_t cmd, uint8_t param);
extern void scanAndConnect();
extern volatile bool bleConnected;
extern SemaphoreHandle_t logMutex;
extern int opCount, opHead;
#define LOG_ROWS 16
extern struct OpEntry opLog[LOG_ROWS];

lv_obj_t *scr_main = nullptr;
lv_obj_t *scr_menu = nullptr;
lv_obj_t *scr_log = nullptr;
lv_obj_t *scr_control = nullptr;
lv_obj_t *scr_sys = nullptr;

lv_obj_t *lbl_status, *lbl_pack, *lbl_curr, *lbl_err;
lv_obj_t *lbl_ctrl_status;
lv_obj_t *lbl_type, *lbl_cells, *lbl_cap, *lbl_power, *lbl_temp;
lv_obj_t *log_table;
int backlightLevel = 1;

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

void createAllScreens() {
    createMainScreen();
    createMenuScreen();
    createLogScreen();
    createControlScreen();
    createSystemScreen();
    lv_scr_load(scr_main);
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
