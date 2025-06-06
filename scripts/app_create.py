import os
import sys

TEMPLATE_H = '''
/**
 * @file sample.h
 * @brief Sample app for C3 UI
 * Generated by app_create.py
 */

#ifndef _SAMPLE_APP_H
#define _SAMPLE_APP_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
#include "app_hal.h"
#include "../../common/app_manager.h"

// #define ENABLE_APP_SAMPLE // Uncomment or define this to enable the sample app

#ifdef ENABLE_APP_SAMPLE

    // LV_IMAGE_DECLARE(sample_icon);

    void sample_screen_init(void);

    void ui_app_load(lv_obj_t **screen, void (*screen_init)(void));
    void ui_app_exit(void);

#endif

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*_SAMPLE_APP_H*/
'''

TEMPLATE_C = '''
/**
 * @file sample.c
 * @brief Sample app for C3 UI
 * Generated by app_create.py
 */

#include "sample.h"

#ifdef ENABLE_APP_SAMPLE

/* Replace NULL with your app icon eg &sample_icon */
REGISTER_APP("Sample App", NULL, sample_screen_main, sample_screen_init);

void sample_screen_event_cb(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_SCREEN_LOAD_START)
    {
        /* Do something before the screen is loaded */
    }
    if (event_code == LV_EVENT_SCREEN_LOADED)
    {
        /* Do something after the screen is loaded */
        /* This is a good place to start animations or timers */
    }
    if (event_code == LV_EVENT_SCREEN_UNLOAD_START)
    {
        /* Do something before the screen is unloaded */
        /* This is a good place to save data or stop timers */
    }
    if (event_code == LV_EVENT_SCREEN_UNLOADED)
    {
        /* Do something after the screen is unloaded */
        /* This is a good place to clean up resources */

        /* Clean and delete screen if needed */
        lv_obj_delete(sample_screen_main);
        sample_screen_main = NULL;
    }

    if (event_code == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT)
    {
        ui_app_exit(); /* exit to app list */
        /* Call this function to close the app, you can even use button instead of gesture */
    }
}

void sample_screen_init(void)
{
    /* create the screen */
    sample_screen_main = lv_obj_create(NULL);
    lv_obj_remove_flag(sample_screen_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sample_screen_main, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sample_screen_main, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(sample_screen_main);
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_label_set_text(label, "Hello world!\\nSample App");

    lv_obj_add_event_cb(sample_screen_main, sample_screen_event_cb, LV_EVENT_ALL, NULL);
}

#endif

'''

def generate_app_files(app_name, output_dir="src/apps"):
    folder_path = os.path.join(output_dir, app_name)
    os.makedirs(folder_path, exist_ok=True)

    assets_path = os.path.join(folder_path, "assets")
    os.makedirs(assets_path, exist_ok=True)

    h_file = os.path.join(folder_path, f"{app_name}.h")
    c_file = os.path.join(folder_path, f"{app_name}.c")

    new_name = app_name.lower()

    with open(h_file, "w") as f:
        f.write(TEMPLATE_H.replace("sample", new_name).replace("SAMPLE", new_name.upper()).replace("Sample", new_name.capitalize()))

    with open(c_file, "w") as f:
        f.write(TEMPLATE_C.replace("sample", new_name).replace("SAMPLE", new_name.upper()).replace("Sample", new_name.capitalize()))

    print(f"✅ Generated {h_file} and {c_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python generate_app.py <app_name>")
        sys.exit(1)

    app_name = sys.argv[1]
    generate_app_files(app_name)
