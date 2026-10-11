#pragma once
#include <lvgl.h>

extern lv_obj_t *scr_main;
extern lv_obj_t *scr_menu;
extern lv_obj_t *scr_log;
extern lv_obj_t *scr_control;
extern lv_obj_t *scr_sys;

void createAllScreens();
void updateUI();
