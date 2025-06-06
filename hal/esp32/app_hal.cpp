/*
   MIT License

  Copyright (c) 2023 Felix Biego

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

  ______________  _____
  ___  __/___  /_ ___(_)_____ _______ _______
  __  /_  __  __ \__  / _  _ \__  __ `/_  __ \
  _  __/  _  /_/ /_  /  /  __/_  /_/ / / /_/ /
  /_/     /_.___/ /_/   \___/ _\__, /  \____/
                              /____/

*/

#include "Arduino.h"
#include <ChronosESP32.h>
#include <Timber.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "app_hal.h"

#include "feedback.h"

#include <lvgl.h>
#include "ui/ui.h"

#include "ui/custom_face.h"
#include "common/api.h"

#include "main.h"
#include "displays/pins.h"
#include "splash.h"

#include "FS.h"
#include "FFat.h"

#ifdef M5_STACK_DIAL
#include "M5Dial.h"
#define tft M5Dial.Display
#define buf_size 10
#elif defined(VIEWE_SMARTRING) || defined(VIEWE_KNOB_15)
#include "displays/viewe.hpp"
#define buf_size 40
#define SW_ROTATION
#else
#include "displays/generic.hpp"
#define buf_size 10
#endif

#ifdef VIEWE_KNOB_15
#include <Encoder.h>
Encoder myEnc(ENCODER_A, ENCODER_B);
#endif

#ifdef ENABLE_APP_QMI8658C
#include "FastIMU.h"
#define QMI_ADDRESS 0x6B
#endif

#ifdef ENABLE_RTC
#include <RtcPCF8563.h>
RtcPCF8563<TwoWire> Rtc(Wire);
#endif

#define FLASH FFat
#define F_NAME "FATFS"


ChronosESP32 watch("Chronos C3");
Preferences prefs;

#ifdef ENABLE_APP_QMI8658C
QMI8658 qmi8658c;
calData calib = {0};
AccelData acc;
GyroData gyro;
#endif

static const uint32_t screenWidth = SCREEN_WIDTH;
static const uint32_t screenHeight = SCREEN_HEIGHT;

const unsigned int lvBufferSize = screenWidth * buf_size;
uint8_t lvBuffer[2][lvBufferSize];

bool weatherUpdate = true, notificationsUpdate = true, weatherUpdateFace = true;

ChronosTimer screenTimer;
ChronosTimer alertTimer;
ChronosTimer searchTimer;

Navigation nav;
bool navChanged = false;
bool navIcChanged = false;
uint32_t navIcCRC = 0xFFFFFFFF;

lv_obj_t *lastActScr;

// bool circular = false;
bool alertSwitch = false;
bool gameActive = false;
bool readIMU = false;
bool updateSeconds = false;
bool hasUpdatedSec = false;
bool navSwitch = false;

static long oldPosition = 0;

String customFacePaths[15];
int customFaceIndex;
static bool transfer = false;
#ifdef ENABLE_CUSTOM_FACE
#error "Custom Watchface has not been migrated to LVGL 9 yet"
// watchface transfer
int cSize, pos, recv;
uint32_t total, currentRecv;
bool last;
String fName;
uint8_t buf1[1024];
uint8_t buf2[1024];
static bool writeFile = false, wSwitch = true;
static int wLen1 = 0, wLen2 = 0;
bool start = false;
int lastCustom;
#endif

TaskHandle_t gameHandle = NULL;

void showAlert();
bool isDay();
void setTimeout(int i);

void hal_setup(void);
void hal_loop(void);

void update_faces();
void updateQrLinks();

void flashDrive_cb(lv_event_t *e);
void driveList_cb(lv_event_t *e);

void checkLocal(bool faces = false);
void registerWatchface_cb(const char *name, const lv_image_dsc_t *preview, lv_obj_t **watchface, lv_obj_t **second);
void registerCustomFace(const char *name, const lv_image_dsc_t *preview, lv_obj_t **watchface, String path);

String hexString(uint8_t *arr, size_t len, bool caps = false, String separator = "");

bool loadCustomFace(String file);
bool deleteCustomFace(String file);
bool readDialBytes(const char *path, uint8_t *data, size_t offset, size_t size);
bool isKnown(uint8_t id);
void parseDial(const char *path, bool restart = false);
bool lvImgHeader(uint8_t *byteArray, uint8_t cf, uint16_t w, uint16_t h, uint16_t stride);


lv_display_rotation_t getRotation(uint8_t rotation)
{
    if (rotation > 3) return LV_DISPLAY_ROTATION_0;
    return (lv_display_rotation_t)rotation;
}

/* Display flushing */
void my_disp_flush(lv_display_t *display, const lv_area_t *area, unsigned char *data)
{

  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(data, w * h);

#ifdef SW_ROTATION
  lv_display_rotation_t rotation = lv_display_get_rotation(display);
	lv_area_t rotated_area;
  if(rotation != LV_DISPLAY_ROTATION_0) {
    lv_color_format_t cf = lv_display_get_color_format(display);
    /*Calculate the position of the rotated area*/
    rotated_area = *area;
    lv_display_rotate_area(display, &rotated_area);
    /*Calculate the source stride (bytes in a line) from the width of the area*/
    uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
    /*Calculate the stride of the destination (rotated) area too*/
    uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
    /*Have a buffer to store the rotated area and perform the rotation*/
    static uint8_t rotated_buf[lvBufferSize];
    lv_draw_sw_rotate(data, rotated_buf, w, h, src_stride, dest_stride, rotation, cf);
    /*Use the rotated area and rotated buffer from now on*/
    area = &rotated_area;
    data = rotated_buf;
  }
#endif

  if (tft.getStartCount() == 0)
  {
    tft.endWrite();
  }

  tft.pushImageDMA(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1, (uint16_t *)data);
  lv_display_flush_ready(display); /* tell lvgl that flushing is done */
}

void rounder_event_cb(lv_event_t *e)
{
  lv_area_t *area = lv_event_get_invalidated_area(e);
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;

  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  // round the start of coordinate down to the nearest 2M number
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  // round the end of coordinate up to the nearest 2N+1 number
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_t *indev_driver, lv_indev_data_t *data)
{
  bool touched;
  uint8_t gesture;
  uint16_t touchX, touchY;
  // RemoteTouch rt = watch.getTouch(); // remote touch
  // if (rt.state)
  // {
  //   // use remote touch when active
  //   touched = rt.state;
  //   touchX = rt.x;
  //   touchY = rt.y;
  // }
  // else
  // {
  //   touched = tft.getTouch(&touchX, &touchY);
  // }

  touched = tft.getTouch(&touchX, &touchY);

  if (!touched)
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  else
  {
    data->state = LV_INDEV_STATE_PRESSED;

    /*Set the coordinates*/
    data->point.x = touchX;
    data->point.y = touchY;
    screen_on();
  }
}

void screen_on(long extra)
{
  screenTimer.time = millis() + extra;
  screenTimer.active = true;
}

bool check_alert_state(AlertType type)
{
  return (alert_states & type) == type;
}

#ifdef ELECROW_C3
// ELECROW C3 I2C IO extender
#define PI4IO_I2C_ADDR 0x43

// Extended IO function
void init_IO_extender()
{
  Wire.beginTransmission(PI4IO_I2C_ADDR);
  Wire.write(0x01); // test register
  Wire.endTransmission();
  Wire.requestFrom(PI4IO_I2C_ADDR, 1);
  uint8_t rxdata = Wire.read();
  Serial.print("Device ID: ");
  Serial.println(rxdata, HEX);

  Wire.beginTransmission(PI4IO_I2C_ADDR);
  Wire.write(0x03);                                                 // IO direction register
  Wire.write((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4)); // set pins 0, 1, 2 as outputs
  Wire.endTransmission();

  Wire.beginTransmission(PI4IO_I2C_ADDR);
  Wire.write(0x07);                                                    // Output Hi-Z register
  Wire.write(~((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4))); // set pins 0, 1, 2 low
  Wire.endTransmission();
}

void set_pin_io(uint8_t pin_number, bool value)
{

  Wire.beginTransmission(PI4IO_I2C_ADDR);
  Wire.write(0x05); // test register
  Wire.endTransmission();
  Wire.requestFrom(PI4IO_I2C_ADDR, 1);
  uint8_t rxdata = Wire.read();
  Serial.print("Before the change: ");
  Serial.println(rxdata, HEX);

  Wire.beginTransmission(PI4IO_I2C_ADDR);
  Wire.write(0x05); // Output register

  if (!value)
    Wire.write((~(1 << pin_number)) & rxdata); // set pin low
  else
    Wire.write((1 << pin_number) | rxdata); // set pin high
  Wire.endTransmission();

  // Wire.beginTransmission(PI4IO_I2C_ADDR);
  // Wire.write(0x05); // test register
  // Wire.endTransmission();
  // Wire.requestFrom(PI4IO_I2C_ADDR, 1);
  // rxdata = Wire.read();
  // Serial.print("after the change: ");
  // Serial.println(rxdata, HEX);
}
#endif

#ifdef ENABLE_RTC
bool wasError(const char *errorTopic = "")
{
  uint8_t error = Rtc.LastError();
  if (error != 0)
  {
    // we have a communications error
    // see https://www.arduino.cc/reference/en/language/functions/communication/wire/endtransmission/
    // for what the number means
    Serial.print("[");
    Serial.print(errorTopic);
    Serial.print("] WIRE communications error (");
    Serial.print(error);
    Serial.print(") : ");

    switch (error)
    {
    case Rtc_Wire_Error_None:
      Serial.println("(none?!)");
      break;
    case Rtc_Wire_Error_TxBufferOverflow:
      Serial.println("transmit buffer overflow");
      break;
    case Rtc_Wire_Error_NoAddressableDevice:
      Serial.println("no device responded");
      break;
    case Rtc_Wire_Error_UnsupportedRequest:
      Serial.println("device doesn't support request");
      break;
    case Rtc_Wire_Error_Unspecific:
      Serial.println("unspecified error");
      break;
    case Rtc_Wire_Error_CommunicationTimeout:
      Serial.println("communications timed out");
      break;
    }
    return true;
  }
  return false;
}
#endif

String heapUsage()
{
  String usage;
  uint32_t total = ESP.getHeapSize();
  uint32_t free = ESP.getFreeHeap();
  usage += "Total: " + String(total);
  usage += "\tFree: " + String(free);
  usage += "\t" + String(((total - free) * 1.0) / total * 100, 2) + "%";
  return usage;
}

void *sd_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
  char buf[256];
  sprintf(buf, "/%s", path);
  // Serial.print("path : ");
  // Serial.println(buf);

  File f;

  if (mode == LV_FS_MODE_WR)
  {
    f = FLASH.open(buf, FILE_WRITE);
  }
  else if (mode == LV_FS_MODE_RD)
  {
    f = FLASH.open(buf);
  }
  else if (mode == (LV_FS_MODE_WR | LV_FS_MODE_RD))
  {
    f = FLASH.open(buf, FILE_WRITE);
  }

  if (!f)
  {
    return NULL; // Return NULL if the file failed to open
  }

  File *fp = new File(f); // Allocate the File object on the heap
  return (void *)fp;      // Return the pointer to the allocated File object
}

lv_fs_res_t sd_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
  lv_fs_res_t res = LV_FS_RES_NOT_IMP;
  File *fp = (File *)file_p;
  uint8_t *buffer = (uint8_t *)buf;

  // Serial.print("name sd_read_cb : ");
  // Serial.println(fp->name());
  *br = fp->read(buffer, btr);

  res = LV_FS_RES_OK;
  return res;
}

lv_fs_res_t sd_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
  lv_fs_res_t res = LV_FS_RES_OK;
  File *fp = (File *)file_p;

  uint32_t actual_pos;

  switch (whence)
  {
  case LV_FS_SEEK_SET:
    actual_pos = pos;
    break;
  case LV_FS_SEEK_CUR:
    actual_pos = fp->position() + pos;
    break;
  case LV_FS_SEEK_END:
    actual_pos = fp->size() + pos;
    break;
  default:
    return LV_FS_RES_INV_PARAM; // Invalid parameter
  }

  if (!fp->seek(actual_pos))
  {
    return LV_FS_RES_UNKNOWN; // Seek failed
  }

  // Serial.print("name sd_seek_cb : ");
  // Serial.println(fp->name());

  return res;
}

lv_fs_res_t sd_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
  lv_fs_res_t res = LV_FS_RES_NOT_IMP;
  File *fp = (File *)file_p;

  *pos_p = fp->position();
  // Serial.print("name in sd_tell_cb : ");
  // Serial.println(fp->name());
  res = LV_FS_RES_OK;
  return res;
}

lv_fs_res_t sd_close_cb(lv_fs_drv_t *drv, void *file_p)
{
  File *fp = (File *)file_p;

  fp->close();
  // delete fp;  // Free the allocated memory

  return LV_FS_RES_OK;
}

void checkLocal(bool faces)
{

  File root = FLASH.open("/");
  if (!root)
  {
    Serial.println("- failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
    }
    else
    {
#ifdef ENABLE_CUSTOM_FACE
      String nm = String(file.name());
      // addListFile(file.name(), file.size());
      if (faces)
      {
        if (nm.endsWith(".js"))
        {
          Serial.print("Found raw face file: ");
          Serial.println(nm);
          String js = "/" + nm + "on";
          nm = "/" + nm;
          if (!FLASH.exists(js.c_str()))
          {
            Serial.println("Parsing");
            parseDial(nm.c_str(), false);
          }
          else
          {
            Serial.println("Skipping, already parsed");
          }
        }
      }
      else
      {

        if (nm.endsWith(".json"))
        {
          // load watchface elements
          nm = "/" + nm;
          registerCustomFace(nm.c_str(), &ui_img_custom_preview_png, &face_custom_root, nm);
        }
      }
#endif
    }
    file = root.openNextFile();
  }
}

void screenBrightness(uint8_t value)
{

#ifdef ELECROW_C3
  set_pin_io(2, value > 0); // ELECROW C3, no brightness control
#else
  tft.setBrightness(value);
#endif
}

void vibratePin(bool state)
{
#ifdef ELECROW_C3
  set_pin_io(0, state); // ELECROW C3, vibration pin
#elif VIBRATION_PIN
  digitalWrite(VIBRATION_PIN, state);
#endif
}

String readFile(const char *path)
{
  String result;
  File file = FLASH.open(path);
  if (!file || file.isDirectory())
  {
    Serial.println("- failed to open file for reading");
    return result;
  }

  Serial.println("- read from file:");
  while (file.available())
  {
    result += (char)file.read();
  }
  file.close();
  return result;
}

void deleteFile(const char *path)
{
  Serial.printf("Deleting file: %s\r\n", path);
  if (FLASH.remove(path))
  {
    Serial.println("- file deleted");
  }
  else
  {
    Serial.println("- delete failed");
  }
}

bool setupFS()
{

#ifndef ENABLE_CUSTOM_FACE
  return false;
#endif

  if (!FLASH.begin(true, "/ffat", MAX_FILE_OPEN))
  {
    FLASH.format();

    return false;
  }

  static lv_fs_drv_t sd_drv;
  lv_fs_drv_init(&sd_drv);
  sd_drv.cache_size = 512;

  sd_drv.letter = 'S';
  sd_drv.open_cb = sd_open_cb;
  sd_drv.close_cb = sd_close_cb;
  sd_drv.read_cb = sd_read_cb;
  sd_drv.seek_cb = sd_seek_cb;
  sd_drv.tell_cb = sd_tell_cb;
  lv_fs_drv_register(&sd_drv);

  checkLocal(true); // parse new faces

  checkLocal(); // register the local faces

  return true;
}

void listDir(const char *dirname, uint8_t levels)
{

  lv_obj_clean(ui_fileManagerPanel);

  addListBack(driveList_cb);

  File root = FLASH.open(dirname);
  if (!root)
  {
    Serial.println("- failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      addListDir(file.name());
      // if (levels)
      // {
      //   listDir(file.path(), levels - 1);
      // }
    }
    else
    {
      addListFile(file.name(), file.size());
    }
    file = root.openNextFile();
  }
}

void flashDrive_cb(lv_event_t *e)
{
  lv_disp_t *display = lv_display_get_default();
  lv_obj_t *actScr = lv_display_get_screen_active(display);
  if (actScr != ui_filesScreen)
  {
    return;
  }

  listDir("/", 0);
}

void sdDrive_cb(lv_event_t *e)
{
  lv_disp_t *display = lv_display_get_default();
  lv_obj_t *actScr = lv_display_get_screen_active(display);
  if (actScr != ui_filesScreen)
  {
    return;
  }

  showError("Error", "SD card is currently unavaliable");
}

void driveList_cb(lv_event_t *e)
{
  lv_obj_clean(ui_fileManagerPanel);

  addListDrive(F_NAME, FLASH.totalBytes(), FLASH.usedBytes(), flashDrive_cb);
  addListDrive("SD card", 0, 0, sdDrive_cb); // dummy SD card drive
}

bool loadCustomFace(String file)
{
  String path = file;
  if (!path.startsWith("/"))
  {
    path = "/" + path;
  }
  String read = readFile(path.c_str());
  JsonDocument face;
  DeserializationError err = deserializeJson(face, read);
  if (!err)
  {
    if (!face["elements"].is<JsonArray>())
    {
      return false;
    }
    String name = face["name"].as<String>();
    JsonArray elements = face["elements"].as<JsonArray>();
    int sz = elements.size();

    Serial.print(sz);
    Serial.println(" elements");

    invalidate_all();
    lv_obj_clean(face_custom_root);

    for (int i = 0; i < sz; i++)
    {
      JsonObject element = elements[i];
      int id = element["id"].as<int>();
      int x = element["x"].as<int>();
      int y = element["y"].as<int>();
      int pvX = element["pvX"].as<int>();
      int pvY = element["pvY"].as<int>();
      String image = element["image"].as<String>();
      JsonArray group = element["group"].as<JsonArray>();

      const char *group_arr[20];
      int group_size = group.size();
      for (int j = 0; j < group_size && j < 20; j++)
      {
        group_arr[j] = group[j].as<const char *>();
      }

      add_item(face_custom_root, id, x, y, pvX, pvY, image.c_str(), group_arr, group_size);
    }

    return true;
  }
  else
  {
    Serial.println("Deserialize failed");
  }

  return false;
}

bool deleteCustomFace(String file)
{
  String path = file;
  if (!path.startsWith("/"))
  {
    path = "/" + path;
  }
  String read = readFile(path.c_str());
  JsonDocument face;
  DeserializationError err = deserializeJson(face, read);
  if (!err)
  {
    if (!face["assets"].is<JsonArray>())
    {
      return false;
    }

    JsonArray assets = face["assets"].as<JsonArray>();
    int sz = assets.size();

    for (int j = 0; j < sz; j++)
    {
      deleteFile(assets[j].as<const char *>());
    }

    deleteFile(path.c_str());

    return true;
  }
  else
  {
    Serial.println("Deserialize failed");
  }

  return false;
}

void registerCustomFace(const char *name, const lv_image_dsc_t *preview, lv_obj_t **watchface, String path)
{
  if (numFaces >= MAX_FACES)
  {
    return;
  }
  faces[numFaces].name = name;
  faces[numFaces].preview = preview;
  faces[numFaces].watchface = watchface;

  faces[numFaces].customIndex = customFaceIndex;
  faces[numFaces].custom = true;

  addWatchface(faces[numFaces].name, faces[numFaces].preview, numFaces);

  customFacePaths[customFaceIndex] = path;
  customFaceIndex++;

  Timber.i("Custom Watchface: %s registered at %d", name, numFaces);
  numFaces++;
}

void onCustomDelete(lv_event_t *e)
{
  int index = (int)lv_event_get_user_data(e);

  Serial.println("Delete custom watchface");
  Serial.println(customFacePaths[index]);
  showError("Delete", "The watchface will be deleted from storage, ESP32 will restart after deletion");
  if (deleteCustomFace(customFacePaths[index]))
  {
    lv_screen_load_anim(ui_appListScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, false);
    ESP.restart();
  }
  else
  {
    showError("Error", "Failed to delete watchface");
  }
}

void addFaceList(lv_obj_t *parent, Face face)
{

  lv_obj_t *ui_faceItemPanel = lv_obj_create(parent);
  lv_obj_set_width(ui_faceItemPanel, 240);
  lv_obj_set_height(ui_faceItemPanel, 50);
  lv_obj_set_align(ui_faceItemPanel, LV_ALIGN_CENTER);
  lv_obj_remove_flag(ui_faceItemPanel, LV_OBJ_FLAG_SCROLLABLE); /// Flags
  lv_obj_set_style_radius(ui_faceItemPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(ui_faceItemPanel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(ui_faceItemPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(ui_faceItemPanel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_opa(ui_faceItemPanel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(ui_faceItemPanel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_side(ui_faceItemPanel, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
  // lv_obj_set_style_bg_color(ui_faceItemPanel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_PRESSED);
  // lv_obj_set_style_bg_opa(ui_faceItemPanel, 100, LV_PART_MAIN | LV_STATE_PRESSED);

  lv_obj_t *ui_faceItemIcon = lv_image_create(ui_faceItemPanel);
  lv_image_set_src(ui_faceItemIcon, &ui_img_clock_png);
  lv_obj_set_width(ui_faceItemIcon, LV_SIZE_CONTENT);  /// 1
  lv_obj_set_height(ui_faceItemIcon, LV_SIZE_CONTENT); /// 1
  lv_obj_set_x(ui_faceItemIcon, 10);
  lv_obj_set_y(ui_faceItemIcon, 0);
  lv_obj_set_align(ui_faceItemIcon, LV_ALIGN_LEFT_MID);
  lv_obj_add_flag(ui_faceItemIcon, LV_OBJ_FLAG_ADV_HITTEST);   /// Flags
  lv_obj_remove_flag(ui_faceItemIcon, LV_OBJ_FLAG_SCROLLABLE); /// Flags

  lv_obj_t *ui_faceItemName = lv_label_create(ui_faceItemPanel);
  lv_obj_set_width(ui_faceItemName, 117);
  lv_obj_set_height(ui_faceItemName, LV_SIZE_CONTENT); /// 1
  lv_obj_set_x(ui_faceItemName, 50);
  lv_obj_set_y(ui_faceItemName, 0);
  lv_obj_set_align(ui_faceItemName, LV_ALIGN_LEFT_MID);
  lv_label_set_long_mode(ui_faceItemName, LV_LABEL_LONG_CLIP);
  if (face.custom)
  {
    lv_label_set_text(ui_faceItemName, customFacePaths[face.customIndex].c_str());
  }
  else
  {
    lv_label_set_text(ui_faceItemName, face.name);
  }

  lv_obj_set_style_text_font(ui_faceItemName, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

  lv_obj_t *ui_faceItemDelete = lv_image_create(ui_faceItemPanel);
  lv_image_set_src(ui_faceItemDelete, &ui_img_bin_png);
  lv_obj_set_width(ui_faceItemDelete, LV_SIZE_CONTENT);  /// 1
  lv_obj_set_height(ui_faceItemDelete, LV_SIZE_CONTENT); /// 1
  lv_obj_set_x(ui_faceItemDelete, -10);
  lv_obj_set_y(ui_faceItemDelete, 0);
  lv_obj_set_align(ui_faceItemDelete, LV_ALIGN_RIGHT_MID);
  lv_obj_add_flag(ui_faceItemDelete, LV_OBJ_FLAG_CLICKABLE);     /// Flags
  lv_obj_add_flag(ui_faceItemDelete, LV_OBJ_FLAG_ADV_HITTEST);   /// Flags
  lv_obj_remove_flag(ui_faceItemDelete, LV_OBJ_FLAG_SCROLLABLE); /// Flags
  lv_obj_set_style_radius(ui_faceItemDelete, 20, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(ui_faceItemDelete, lv_color_hex(0xF34235), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(ui_faceItemDelete, 255, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_color(ui_faceItemDelete, lv_color_hex(0xF34235), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_opa(ui_faceItemDelete, 255, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(ui_faceItemDelete, 2, LV_PART_MAIN | LV_STATE_PRESSED);

  if (!face.custom)
  {
    lv_obj_add_flag(ui_faceItemDelete, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_add_event_cb(ui_faceItemDelete, onCustomDelete, LV_EVENT_CLICKED, (void *)face.customIndex);
  }
}

void timerEnded(int x)
{
  feedbackRun(T_ALARM);

  screenTimer.time = millis() + 50;
  screenTimer.active = true;
  screen_on();
}

void simonTone(int type, int pitch)
{
  switch (type)
  {
  case 0:
    feedbackTone(tone_simonsays_intro, 4, T_USER);
    break;
  case 1:
    feedbackTone(tone_simonsays_gameover, 4, T_USER);
    feedbackVibrate(v_notif, 2, true);
    break;
  case 2:
  {
    Note note[] = {pitch, 200};
    feedbackTone(note, 1, T_USER);
  }
  break;
  }
}

void connectionCallback(bool state)
{
  Timber.d(state ? "Connected" : "Disconnected");
  if (state)
  {
    lv_obj_remove_state(ui_btStateButton, LV_STATE_CHECKED);
  }
  else
  {
    lv_obj_add_state(ui_btStateButton, LV_STATE_CHECKED);
  }
  lv_label_set_text_fmt(ui_appConnectionText, "Status\n%s", state ? "Connected" : "Disconnected");
}

void ringerCallback(String caller, bool state)
{
  lv_disp_t *display = lv_display_get_default();
  lv_obj_t *actScr = lv_display_get_screen_active(display);

  if (state)
  {
    feedbackRun(T_CALLS);
    screenTimer.time = millis() + 50;

    lastActScr = actScr;
    Serial.print("Ringer: Incoming call from ");
    Serial.println(caller);
    lv_label_set_text(ui_callName, caller.c_str());
    lv_screen_load_anim(ui_callScreen, LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);
  }
  else
  {
    feedbackTone(tone_off, 1, T_USER, true);
    Serial.println("Ringer dismissed");
    // load last active screen
    if (actScr == ui_callScreen && lastActScr != nullptr)
    {
      lv_screen_load_anim(lastActScr, LV_SCR_LOAD_ANIM_FADE_OUT, 500, 0, false);
    }
  }
  screenTimer.active = true;
}

void notificationCallback(Notification notification)
{
  Timber.d("Notification Received from " + notification.app + " at " + notification.time);
  Timber.d(notification.message);
  notificationsUpdate = true;
  // onNotificationsOpen(click);
  feedbackRun(T_NOTIFICATION);
  showAlert();
}

void configCallback(Config config, uint32_t a, uint32_t b)
{
  switch (config)
  {
  case CF_TIME:
    // time has been synced from BLE
#ifdef ENABLE_RTC
    // set the RTC time
    Rtc.SetDateTime(RtcDateTime(watch.getYear(), watch.getMonth() + 1, watch.getDay(), watch.getHour(true), watch.getMinute(), watch.getSecond()));

#endif
    // ui_update_seconds(watch.getSecond());
    if (!hasUpdatedSec)
    {
      hasUpdatedSec = true;
      updateSeconds = true;
    }

    break;
  case CF_FIND:
    feedbackRun(T_TIMER);

    break;
  case CF_RST:

    Serial.println("Reset request, formating storage");
    FLASH.format();
    delay(2000);
    ESP.restart();

    break;
  case CF_WEATHER:

    if (a)
    {
      weatherUpdateFace = true;
    }
    if (a == 2)
    {
      weatherUpdate = true;
    }

    break;
  case CF_FONT:
    screenTimer.time = millis();
    screenTimer.active = true;
    if (((b >> 16) & 0xFFFF) == 0x01)
    { // Style 1
      if ((b & 0xFFFF) == 0x01)
      { // TOP
        lv_obj_set_style_text_color(ui_hourLabel, lv_color_hex(a), LV_PART_MAIN | LV_STATE_DEFAULT);
      }
      if ((b & 0xFFFF) == 0x02)
      { // CENTER
        lv_obj_set_style_text_color(ui_minuteLabel, lv_color_hex(a), LV_PART_MAIN | LV_STATE_DEFAULT);
      }
      if ((b & 0xFFFF) == 0x03)
      { // BOTTOM
        lv_obj_set_style_text_color(ui_dayLabel, lv_color_hex(a), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ui_dateLabel, lv_color_hex(a), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ui_weatherTemp, lv_color_hex(a), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ui_amPmLabel, lv_color_hex(a), LV_PART_MAIN | LV_STATE_DEFAULT);
      }
    }

    break;
  case CF_CAMERA:
  {
    lv_disp_t *display = lv_display_get_default();
    lv_obj_t *actScr = lv_display_get_screen_active(display);

    if (b)
    {
      screenTimer.time = millis() + 50;
      lastActScr = actScr;
      lv_screen_load_anim(ui_cameraScreen, LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);
      screenTimer.active = true;
    }
    else
    {
      if (actScr == ui_cameraScreen && lastActScr != nullptr)
      {
        lv_screen_load_anim(lastActScr, LV_SCR_LOAD_ANIM_FADE_OUT, 500, 0, false);
      }
      screenTimer.active = true;
    }
  }
  break;

  case CF_APP:
    // state is saved internally
    Serial.print("Chronos App; Code: ");
    Serial.print(a); // int code = watch.getAppCode();
    Serial.print(" Version: ");
    Serial.println(watch.getAppVersion());
    lv_label_set_text_fmt(ui_appDetailsText, "Chronos app\nv%s (%d)", watch.getAppVersion().c_str(), a);
    break;
  case CF_QR:
    if (a == 1)
    {
      updateQrLinks();
    }
    break;
  case CF_NAV_DATA:
    navChanged = true;
    break;
  case CF_NAV_ICON:
    if (a == 2)
    {
      navIcChanged = true;
      Timber.w("Navigation icon received. CRC 0x%04X", b);
    }
    break;
  case CF_CONTACT:
    if (a == 0)
    {
      Serial.println("Receiving contacts");
      Serial.print("SOS index: ");
      Serial.print(uint8_t(b >> 8));
      Serial.print("\tSize: ");
      Serial.println(uint8_t(b));
      setNoContacts();
    }
    if (a == 1)
    {
      Serial.println("Received all contacts");
      int n = uint8_t(b);      // contacts size -> watch.getContactCount();
      int s = uint8_t(b >> 8); // sos contact index -> watch.getSOSContactIndex();

      clearContactList();

      for (int i = 0; i < n; i++)
      {
        Contact cn = watch.getContact(i);
        Serial.print("Name: ");
        Serial.print(cn.name);
        Serial.print(s == i ? " [SOS]" : "");
        Serial.print("\tNumber: ");
        Serial.println(cn.number);
        addContact(cn.name.c_str(), cn.number.c_str(), s == i);
      }
    }
    break;
  }
}

void onMessageClick(lv_event_t *e)
{

  lv_disp_t *display = lv_display_get_default();
  lv_obj_t *actScr = lv_display_get_screen_active(display);
  if (actScr != ui_notificationScreen)
  {
    Timber.i("Message screen inactive");
    return;
  }
  // Your code here
  int index = (int)lv_event_get_user_data(e);

  index %= NOTIF_SIZE;
  Timber.i("Message clicked at index %d", index);

  lv_label_set_text(ui_messageTime, watch.getNotificationAt(index).time.c_str());
  lv_label_set_text(ui_messageContent, watch.getNotificationAt(index).message.c_str());
  setNotificationIcon(ui_messageIcon, watch.getNotificationAt(index).icon);

  lv_obj_scroll_to_y(ui_messagePanel, 0, LV_ANIM_ON);
  lv_obj_add_flag(ui_messageList, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(ui_messagePanel, LV_OBJ_FLAG_HIDDEN);
}

void onCaptureClick(lv_event_t *e)
{
  watch.capturePhoto();
}

void onForecastOpen(lv_event_t *e)
{
  // lv_obj_scroll_to_y(ui_forecastList, 0, LV_ANIM_ON);
}

void onScrollMode(lv_event_t *e)
{
  prefs.putBool("circular", circular);
}

void onAlertState(lv_event_t *e)
{
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  alertSwitch = lv_obj_has_state(obj, LV_STATE_CHECKED);

  // prefs.putBool("alert_states", alert_states);
}

void on_alert_state_change(int32_t states)
{
  alert_states = states;
  prefs.putInt("alert_states", alert_states);

  feedbackTone(tone_button, 1, T_SYSTEM);
  feedbackVibrate(pattern, 2, true);
}

void onNavState(lv_event_t *e)
{
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  navSwitch = lv_obj_has_state(obj, LV_STATE_CHECKED);

  prefs.putBool("autonav", navSwitch);
}

void savePrefInt(const char *key, int value)
{
  prefs.putInt(key, value);
}

int getPrefInt(const char *key, int def_value)
{
  return prefs.getInt(key, def_value);
}

void onNotificationsOpen(lv_event_t *e)
{
  if (!notificationsUpdate)
  {
    return;
  }
  notificationsUpdate = false;

  lv_obj_clean(ui_messageList);
  int c = watch.getNotificationCount();
  for (int i = 0; i < c; i++)
  {
    addNotificationList(watch.getNotificationAt(i).icon, watch.getNotificationAt(i).message.c_str(), i);
  }
  // addNotificationList(watch.getNotificationAt(0).icon, watch.getNotificationAt(0).message.c_str(), i);

  lv_obj_scroll_to_y(ui_messageList, 1, LV_ANIM_ON);
  lv_obj_remove_flag(ui_messageList, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_messagePanel, LV_OBJ_FLAG_HIDDEN);
}

void onWeatherLoad(lv_event_t *e)
{
  lv_obj_remove_flag(ui_weatherPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_forecastList, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_hourlyList, LV_OBJ_FLAG_HIDDEN);

  if (!weatherUpdate)
  {
    return;
  }
  weatherUpdate = false;

  if (watch.getWeatherCount() > 0)
  {
    String updateTime = "Updated at\n" + watch.getWeatherTime();
    lv_label_set_text(ui_weatherCity, watch.getWeatherCity().c_str());
    lv_label_set_text(ui_weatherUpdateTime, updateTime.c_str());
    lv_label_set_text_fmt(ui_weatherCurrentTemp, "%d°C", watch.getWeatherAt(0).temp);
    setWeatherIcon(ui_weatherIcon, watch.getWeatherAt(0).icon, isDay());
    setWeatherIcon(ui_weatherCurrentIcon, watch.getWeatherAt(0).icon, isDay());

    lv_obj_clean(ui_forecastList);
    int c = watch.getWeatherCount();
    for (int i = 0; i < c; i++)
    {
      addForecast(watch.getWeatherAt(i).day, watch.getWeatherAt(i).temp, watch.getWeatherAt(i).icon);
    }

    // lv_obj_scroll_by(ui_forecastList, 0, -1, LV_ANIM_OFF);

    lv_obj_clean(ui_hourlyList);
    addHourlyWeather(0, watch.getWeatherAt(0).icon, 0, 0, 0, 0, true);
    for (int h = watch.getHour(true); h < 24; h++)
    {
      HourlyForecast hf = watch.getForecastHour(h);
      addHourlyWeather(hf.hour, hf.icon, hf.temp, hf.humidity, hf.wind, hf.uv, false);
    }
  }
}

void onLoadHome(lv_event_t *e)
{
  // if (isDay())
  // {
  //   lv_obj_set_style_bg_image_src( ui_clockScreen, &ui_img_857483832, LV_PART_MAIN | LV_STATE_DEFAULT);
  // }
  // else
  // {
  //   lv_obj_set_style_bg_image_src( ui_clockScreen, &ui_img_753022056, LV_PART_MAIN | LV_STATE_DEFAULT);
  // }
}

void onBrightnessChange(lv_event_t *e)
{
  // Your code here
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  int v = lv_slider_get_value(slider);
  screenBrightness(v);

  prefs.putInt("brightness", v);
}

void onFaceSelected(lv_event_t *e)
{
  feedbackVibrate(v_notif, 2, true);
  int index = (int)lv_event_get_user_data(e);
  prefs.putInt("watchface", index);
}

void on_watchface_list_open()
{
  feedbackVibrate(v_notif, 2, true);
}

void onCustomFaceSelected(int pathIndex)
{
#ifdef ENABLE_CUSTOM_FACE
  feedbackVibrate(v_notif, 2, true);

  if (pathIndex < 0)
  {
    prefs.putString("custom", "");
    return;
  }
  if (lv_obj_get_child_count(face_custom_root) > 0 && lastCustom == pathIndex)
  {
    ui_home = face_custom_root;
  }
  else if (loadCustomFace(customFacePaths[pathIndex]))
  {
    lastCustom = pathIndex;
    ui_home = face_custom_root;
  }

  lv_screen_load_anim(ui_home, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);

  prefs.putString("custom", customFacePaths[pathIndex]);
#endif
}

void onBatteryChange(lv_event_t *e)
{
  uint8_t lvl = lv_slider_get_value(ui_batterySlider);
  watch.setBattery(lvl);
}

void onStartSearch(lv_event_t *e)
{
  watch.findPhone(true);
}

void onEndSearch(lv_event_t *e)
{
  watch.findPhone(false);
}

void onClickAlert(lv_event_t *e)
{

  // cancel alert timer
  alertTimer.active = false;
  // change screen to notifications
  lv_screen_load(ui_notificationScreen);

  // enable screen for timeout + 5 seconds
  screenTimer.time = millis() + 5000;
  screenTimer.active = true;

  // load the last received message
  lv_label_set_text(ui_messageTime, watch.getNotificationAt(0).time.c_str());
  lv_label_set_text(ui_messageContent, watch.getNotificationAt(0).message.c_str());
  setNotificationIcon(ui_messageIcon, watch.getNotificationAt(0).icon);

  lv_obj_scroll_to_y(ui_messagePanel, 0, LV_ANIM_ON);
  lv_obj_add_flag(ui_messageList, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(ui_messagePanel, LV_OBJ_FLAG_HIDDEN);
}

void onTimeoutChange(lv_event_t *e)
{
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  uint16_t sel = lv_dropdown_get_selected(obj);
  Timber.i("Selected index: %d", sel);

  setTimeout(sel);
  prefs.putInt("timeout", sel);
}

void onRotateChange(lv_event_t *e)
{
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  uint16_t sel = lv_dropdown_get_selected(obj);
  Timber.i("Selected index: %d", sel);

  prefs.putInt("rotate", sel);
#ifdef SW_ROTATION
  lv_display_set_rotation(lv_display_get_default(), getRotation(sel));
#else
  tft.setRotation(sel);
  // screen rotation has changed, invalidate to redraw
  lv_obj_invalidate(lv_screen_active());
#endif
}

void onLanguageChange(lv_event_t *e)
{
}

void setTimeout(int i)
{
  if (i == 4)
  {
    screenTimer.duration = -1; // always on
  }
  else if (i == 0)
  {
    screenTimer.duration = 5000; // 5 seconds
    screenTimer.active = true;
  }
  else if (i < 4)
  {
    screenTimer.duration = 10000 * i; // 10, 20, 30 seconds
    screenTimer.active = true;
  }
}

void onMusicPlay(lv_event_t *e)
{
  watch.musicControl(MUSIC_TOGGLE);
}

void onMusicPrevious(lv_event_t *e)
{
  watch.musicControl(MUSIC_PREVIOUS);
}

void onMusicNext(lv_event_t *e)
{
  watch.musicControl(MUSIC_NEXT);
}

void onVolumeUp(lv_event_t *e)
{
  watch.musicControl(VOLUME_UP);
}

void onVolumeDown(lv_event_t *e)
{
  watch.musicControl(VOLUME_DOWN);
}

void updateQrLinks()
{
#if LV_USE_QRCODE == 1
  lv_obj_clean(ui_qrPanel);
  for (int i = 0; i < 9; i++)
  {
    addQrList(i, watch.getQrAt(i).c_str());
  }
#endif
}

void onRTWState(bool state)
{
}

void gameLoop(void *pvParameters)
{
  for (;;)
  {
    ui_games_update();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void onGameOpened()
{
  gameActive = true;

#ifdef ENABLE_GAME_TASK
  if (gameHandle == NULL)
  {
    // create task to run the game loop
    xTaskCreate(gameLoop, "Game Task", 8192, NULL, 1, &gameHandle);
  }
#endif
}

void onGameClosed()
{
  gameActive = false;
#ifdef ENABLE_GAME_TASK
  if (gameHandle != NULL)
  {
    vTaskDelete(gameHandle);
    gameHandle = NULL;
  }
#endif
  screenTimer.active = true;
}

void showAlert()
{
  lv_disp_t *display = lv_display_get_default();
  lv_obj_t *actScr = lv_display_get_screen_active(display);
  if (actScr == ui_notificationScreen)
  {
    // at notifications screen, switch to message
    // enable screen for timeout + 5 seconds
    screenTimer.time = millis() + 5000;
    screenTimer.active = true;

    // load the last received message
    lv_label_set_text(ui_messageTime, watch.getNotificationAt(0).time.c_str());
    lv_label_set_text(ui_messageContent, watch.getNotificationAt(0).message.c_str());
    setNotificationIcon(ui_messageIcon, watch.getNotificationAt(0).icon);

    lv_obj_scroll_to_y(ui_messagePanel, 0, LV_ANIM_ON);
    lv_obj_add_flag(ui_messageList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui_messagePanel, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    if (!check_alert_state(ALERT_POPUP))
    {
      return;
    }
    // attach the alert panel to current screen
    lv_obj_set_parent(ui_alertPanel, actScr);

    // load the last received message
    lv_label_set_text(ui_alertText, watch.getNotificationAt(0).message.c_str());
    setNotificationIcon(ui_alertIcon, watch.getNotificationAt(0).icon);

    // turn screen for timeout + 5 seconds
    screenTimer.time = millis() + 5000;
    screenTimer.active = true;

    alertTimer.time = millis();
    alertTimer.active = true;

    // show the alert
    lv_obj_remove_flag(ui_alertPanel, LV_OBJ_FLAG_HIDDEN);
  }
}

void rawDataCallback(uint8_t *data, int len)
{

#ifdef ENABLE_CUSTOM_FACE
  if (data[0] == 0xB0)
  {
    // this is a chunk header data command
    cSize = data[1] * 256 + data[2];                                                           // data chunk size
    pos = data[3] * 256 + data[4];                                                             // position of the chunk, ideally sequential 0..
    last = data[7] == 1;                                                                       // whether this is the last chunk (1) or not (0)
    total = (data[8] * 256 * 256 * 256) + (data[9] * 256 * 256) + (data[10] * 256) + data[11]; // total size of the whole file
    recv = 0;                                                                                  // counter for the chunk data

    start = pos == 0;
    if (pos == 0)
    {
      // this is the first chunk
      transfer = true;
      currentRecv = 0;

      fName = "/" + String(total, HEX) + "-" + String(total) + ".cbn";
    }
  }
  if (data[0] == 0xAF)
  {
    // this is the chunk data, line by line. The complete chunk will have several of these
    // actual data starts from index 5
    int ln = ((data[1] * 256 + data[2]) - 5); // byte 1 and 2 make up the (total size of data - 5)

    if (wSwitch)
    {
      memcpy(buf1 + recv, data + 5, ln);
    }
    else
    {
      memcpy(buf2 + recv, data + 5, ln);
    }

    recv += ln; // increment the received chunk data size by current received size

    currentRecv += ln; // track the progress

    if (recv == cSize)
    { // received expected? if data chunk size equals chunk receive size then chunk is complete
      if (wSwitch)
      {
        wLen1 = cSize;
      }
      else
      {
        wLen2 = cSize;
      }

      wSwitch = !wSwitch;
      writeFile = true;

      // write to file (file name -> fName)
      // buf1, wLen1 if wSwitch false
      // buf2, wLen2 if wSwitch true

      pos++;

      // notify if save successful, otherwise ignore
      uint8_t lst = last ? 0x01 : 0x00;
      uint8_t cmd[5] = {0xB0, 0x02, highByte(pos), lowByte(pos), lst};
      watch.sendCommand(cmd, 5); // notify the app that we received the chunk, this will trigger transfer of next chunk
    }

    if (last)
    {
    }
  }
#endif
}

void dataCallback(uint8_t *data, int length)
{
  // Serial.println("Received Data");
  // for (int i = 0; i < length; i++)
  // {
  //   Serial.printf("%02X ", data[i]);
  // }
  // Serial.println();
}

void imu_init()
{
#ifdef ENABLE_APP_QMI8658C
  int err = qmi8658c.init(calib, QMI_ADDRESS);
  if (err != 0)
  {
    showError("IMU State", "Failed to init");
  }
#endif
}

imu_data_t get_imu_data()
{
  imu_data_t qmi;
#ifdef ENABLE_APP_QMI8658C

  qmi8658c.update();
  qmi8658c.getAccel(&acc);
  qmi8658c.getGyro(&gyro);

  qmi.ax = acc.accelX;
  qmi.ay = acc.accelY;
  qmi.az = acc.accelZ;
  qmi.gx = gyro.gyroX;
  qmi.gy = gyro.gyroY;
  qmi.gz = gyro.gyroZ;
  qmi.temp = qmi8658c.getTemp();
  qmi.success = true;
#else
  qmi.success = false;
#endif
  return qmi;
}

void imu_close()
{
#ifdef ENABLE_APP_QMI8658C

#endif
}

void contacts_app_launched()
{
  clearContactList();
  int n = watch.getContactCount();
  int s = watch.getSOSContactIndex();
  int i;
  for (i = 0; i < n; i++)
  {
    Contact cn = watch.getContact(i);
    addContact(cn.name.c_str(), cn.number.c_str(), s == i);
  }
  if (i == 0)
  {
    setNoContacts();
  }
}

void calendar_app_launched(void)
{
  calendar_set_today(watch.getYear(), watch.getMonth() + 1, watch.getDay());
}

int32_t read_encoder_position()
{
#ifdef M5_STACK_DIAL
  M5Dial.update();
  return M5Dial.Encoder.read();
#elif defined(VIEWE_KNOB_15)
  return myEnc.read();
#endif
  return 0;
}

void logCallback(Level level, unsigned long time, String message)
{
  Serial.print(message);
  Serial1.print(message);
}

// void lv_log_register_print_cb(lv_log_print_g_cb_t print_cb) {
//   // Do nothing, not needed here!
// }

// void my_log_cb(lv_log_level_t level, const char *buf)
// {
//   Serial.write(buf, strlen(buf));
//   Serial1.write(buf, strlen(buf));
// }

int putchar(int ch)
{
  Serial.write(ch); // Send character to Serial
  return ch;
}

void loadSplash()
{
  int w = 122;
  int h = 130;
  int x = (SCREEN_WIDTH - w) / 2;
  int y = (SCREEN_HEIGHT - h) / 2;
  tft.fillScreen(TFT_BLACK);
  screenBrightness(200);
  tft.pushImageDMA(x, y, w, h, (uint16_t *)splash);
  delay(2000);
}

static uint32_t my_tick(void)
{
  return millis();
}

void hal_setup()
{

  Serial.begin(115200); /* prepare for possible serial debug */
  Serial1.begin(115200);

  Timber.setLogCallback(logCallback);

  Timber.i("Starting up device");

  prefs.begin("my-app");

  int rt = prefs.getInt("rotate", 0);

#ifdef ELECROW_C3
  Wire.begin(4, 5);
  init_IO_extender();
  delay(100);
  set_pin_io(0, false);
  set_pin_io(2, true);
  set_pin_io(3, true);
  set_pin_io(4, true);
#endif

#ifdef M5_STACK_DIAL
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
#endif
  alert_states = 0x0F; // set default
#if !defined(BUZZER_PIN) || (BUZZER_PIN == -1)
  alert_states &= ~0x04;
#endif
#if !defined(VIBRATION_PIN) || (VIBRATION_PIN == -1)
  alert_states &= ~0x08;
#endif

  tft.init();
  tft.initDMA();
  tft.startWrite();
  tft.fillScreen(TFT_BLACK);
  tft.setRotation(rt);

  loadSplash();


  alert_states = prefs.getInt("alert_states", alert_states);

  startToneSystem();
  startVibrationSystem();

  feedbackTone(tone_startup, 3, T_SYSTEM);
  feedbackVibrate(pattern, 4);

  lv_init();

  lv_tick_set_cb(my_tick);

  static auto *lvDisplay = lv_display_create(screenWidth, screenHeight);
  lv_display_set_color_format(lvDisplay, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(lvDisplay, my_disp_flush);
  lv_display_set_buffers(lvDisplay, lvBuffer[0], lvBuffer[1], lvBufferSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(lvDisplay, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

#ifdef SW_ROTATION
  lv_display_set_rotation(lvDisplay, getRotation(rt));
#endif

  static auto *lvInput = lv_indev_create();
  lv_indev_set_type(lvInput, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(lvInput, my_touchpad_read);

  // lv_log_register_print_cb(my_log_cb);

  // _lv_fs_init();

  ui_init();

  bool fsState = setupFS();
  if (fsState)
  {
    // driveList_cb(NULL);
    Serial.println("Setup FS success");

    Timber.i("Flash: Total %d => Used %d", FLASH.totalBytes(), FLASH.usedBytes());
  }
  else
  {
    Serial.println("Setup FS failed");
    // showError(F_NAME, "Failed to mount the partition");
  }

  int wf = prefs.getInt("watchface", 0);
#ifdef ENABLE_CUSTOM_FACE
  String custom = prefs.getString("custom", "");
  if (wf >= numFaces)
  {
    wf = 0; // default
  }
  currentIndex = wf;
  if (custom != "" && fsState && loadCustomFace(custom))
  {
    ui_home = face_custom_root;
  }
  else
  {
    ui_home = *faces[wf].watchface; // load saved watchface power on
  }
#else
  if (wf >= numFaces)
  {
    wf = 0; // default
  }
  currentIndex = wf;
  ui_home = *faces[wf].watchface; // load saved watchface power on
#endif
  lv_screen_load(ui_home);

  int ch = lv_obj_get_child_count(ui_faceSelect);
  if (wf < ch)
  {
    lv_obj_scroll_to_view(lv_obj_get_child(ui_faceSelect, wf), LV_ANIM_OFF);
  }

#ifdef ESPS3_1_69
  watch.setScreen(CS_240x296_191_RTF);
#elif defined(VIEWE_SMARTRING) || defined(VIEWE_KNOB_15)
  watch.setScreen(CS_466x466_143_CTF);
#endif
  String chip = String(ESP.getChipModel());
  watch.setName(chip);
  watch.setConnectionCallback(connectionCallback);
  watch.setNotificationCallback(notificationCallback);
  watch.setConfigurationCallback(configCallback);
  watch.setRingerCallback(ringerCallback);
  watch.setDataCallback(dataCallback);
  watch.setRawDataCallback(rawDataCallback);
  watch.begin();
  watch.set24Hour(true);
  watch.setBattery(85);

  String about = String(ui_info_text) + "\n" + chip + "\n" + watch.getAddress();
  lv_label_set_text(ui_aboutText, about.c_str());

#if LV_USE_QRCODE == 1
  String address = watch.getAddress();
  address.toUpperCase();
  String qrCode = "{\"Name\":\"" + chip + "\", \"Mac\":\"" + address + "\"}";
  lv_qrcode_update(ui_connectImage, qrCode.c_str(), qrCode.length());
  lv_label_set_text(ui_connectText, "Scan to connect");
#endif
  // bool intro = prefs.getBool("intro", true);

  // if (intro)
  // {
  //   showAlert();
  //   prefs.putBool("intro", false);
  // }
  // else
  // {
  //   lv_obj_add_flag(ui_alertPanel, LV_OBJ_FLAG_HIDDEN);
  // }

  // load saved preferences
  int tm = prefs.getInt("timeout", 0);

  int br = prefs.getInt("brightness", 100);
  circular = prefs.getBool("circular", false);
  alertSwitch = prefs.getBool("alerts", false);
  navSwitch = prefs.getBool("autonav", false);

  lv_obj_scroll_to_y(ui_settingsList, 1, LV_ANIM_ON);
  lv_obj_scroll_to_y(ui_appList, 1, LV_ANIM_ON);
  lv_obj_scroll_to_y(ui_appInfoPanel, 1, LV_ANIM_ON);
  lv_obj_scroll_to_y(ui_gameList, 1, LV_ANIM_ON);

  if (tm > 4)
  {
    tm = 4;
  }
  else if (tm < 0)
  {
    tm = 0;
  }

  screenBrightness(br);

  lv_dropdown_set_selected(ui_timeoutSelect, tm, LV_ANIM_OFF);
  lv_dropdown_set_selected(ui_rotateSelect, rt, LV_ANIM_OFF);
  lv_slider_set_value(ui_brightnessSlider, br, LV_ANIM_OFF);

  set_alert_states(alert_states);

  if (circular)
  {
    lv_obj_add_state(ui_Switch2, LV_STATE_CHECKED);
  }
  else
  {
    lv_obj_remove_state(ui_Switch2, LV_STATE_CHECKED);
  }

#ifdef ENABLE_APP_NAVIGATION
  if (navSwitch)
  {
    lv_obj_add_state(ui_navStateSwitch, LV_STATE_CHECKED);
  }
  else
  {
    lv_obj_remove_state(ui_navStateSwitch, LV_STATE_CHECKED);
  }
#endif

  screenTimer.active = true;
  screenTimer.time = millis();

  setTimeout(tm);

#ifdef ENABLE_CUSTOM_FACE
  if (!fsState)
  {
    showError(F_NAME, "Failed to mount the partition");
  }
#endif
  if (lv_fs_is_ready('S'))
  {
    Serial.println("Drive S is ready");
  }
  else
  {
    Serial.println("Drive S is not ready");
  }

  imu_init();

#ifdef ENABLE_RTC
  Rtc.Begin();

  if (!Rtc.GetIsRunning())
  {
    uint8_t error = Rtc.LastError();
    if (error != 0)
    {
      showError("RTC", "Error on RTC");
    }
    Rtc.SetIsRunning(true);
  }

  RtcDateTime now = Rtc.GetDateTime();

  watch.setTime(now.Second(), now.Minute(), now.Hour(), now.Day(), now.Month(), now.Year());

  Rtc.StopAlarm();
  Rtc.StopTimer();
  Rtc.SetSquareWavePin(PCF8563SquareWavePinMode_None);
#endif

  ui_update_seconds(watch.getSecond());

  lv_rand_set_seed(millis());

  navigateInfo("Navigation", "Chronos", "Start navigation on Google maps");

  watch.clearNotifications();
  notificationsUpdate = false;
  lv_obj_clean(ui_messageList);
  lv_obj_t *info = lv_label_create(ui_messageList);
  lv_obj_set_width(info, 180);
  lv_obj_set_y(info, 20);
  lv_obj_set_height(info, LV_SIZE_CONTENT); /// 1
  lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(info, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_label_set_text(info, "No notifications available. Connect Chronos app to receive phone notifications");

#if !defined(BUZZER_PIN) || (BUZZER_PIN == -1)
  lv_obj_add_state(ui_soundsAlert, LV_STATE_DISABLED);
#endif
#if !defined(VIBRATION_PIN) || (VIBRATION_PIN == -1)
  lv_obj_add_state(ui_vibrateAlert, LV_STATE_DISABLED);
#endif

  ui_setup();

  Serial.println(heapUsage());

  Timber.i("Setup done");
  Timber.i(about);
}

void hal_loop()
{

  if (!transfer)
  {
    lv_timer_handler(); // Update the UI-
    delay(5);

    watch.loop();

#if defined(M5_STACK_DIAL) || defined(VIEWE_KNOB_15)
    long newPosition = read_encoder_position();
    if (newPosition != oldPosition)
    {
      input_bus_emit_encoder_event(newPosition, newPosition - oldPosition);
      oldPosition = newPosition;
    }
#endif
#ifdef M5_STACK_DIAL
    M5Dial.update();
    if (M5Dial.BtnA.wasPressed())
    {
      input_bus_emit_button_event(true);
      M5Dial.Encoder.readAndReset();
    }
#endif

    if (updateSeconds)
    {
      updateSeconds = false;
      ui_update_seconds(watch.getSecond());
    }

    if (ui_home == ui_clockScreen)
    {
      lv_label_set_text(ui_hourLabel, watch.getHourZ().c_str());
      lv_label_set_text(ui_dayLabel, watch.getTime("%A").c_str());
      lv_label_set_text(ui_minuteLabel, watch.getTime("%M").c_str());
      lv_label_set_text(ui_dateLabel, watch.getTime("%d\n%b").c_str());
      lv_label_set_text(ui_amPmLabel, watch.getAmPmC(false).c_str());
    }
    else
    {
      update_faces();
    }

    lv_disp_t *display = lv_display_get_default();
    lv_obj_t *actScr = lv_display_get_screen_active(display);
    if (actScr != ui_home)
    {
    }

    if (weatherUpdateFace)
    {
      lv_label_set_text_fmt(ui_weatherTemp, "%d°C", watch.getWeatherAt(0).temp);
      // set icon ui_weatherIcon
      setWeatherIcon(ui_weatherIcon, watch.getWeatherAt(0).icon, isDay());
      weatherUpdateFace = false;
    }

    if (navChanged)
    {
      navChanged = false;
      nav = watch.getNavigation();
      if (!nav.active)
      {
        nav.directions = "Start navigation on Google maps";
        nav.title = "Chronos";
        nav.duration = watch.isConnected() ? "Inactive" : "Disconnected";
        nav.eta = "Navigation";
        nav.distance = "";
        navIcCRC = 0xFFFFFFFF;
      }

      if (!nav.isNavigation)
      {
        nav.directions = nav.title;
        nav.title = "";
      }

      String navText = nav.eta + "\n" + nav.duration + " " + nav.distance;

#ifdef ENABLE_APP_NAVIGATION
      if (actScr != get_nav_screen() && nav.active && navSwitch)
      {
        lastActScr = actScr;
        if (!get_nav_screen())
        {
          ui_navScreen_screen_init();
        }
        lv_screen_load_anim(get_nav_screen(), LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);
        gameActive = true;
        screenTimer.active = true;
      }
      if (actScr == get_nav_screen() && !nav.active && navSwitch && lastActScr != nullptr)
      {
        screenTimer.active = true;
        lv_screen_load_anim(lastActScr, LV_SCR_LOAD_ANIM_FADE_OUT, 500, 0, false);
      }
#endif
      navIconState(nav.active && nav.hasIcon);
      navigateInfo(navText.c_str(), nav.title.c_str(), nav.directions.c_str());
    }
    if (navIcChanged)
    {
      navIcChanged = false;
      nav = watch.getNavigation();

      if (nav.iconCRC != navIcCRC)
      {
        navIcCRC = nav.iconCRC;
        navIconState(nav.active && nav.hasIcon);
        for (int y = 0; y < 48; y++)
        {
          for (int x = 0; x < 48; x++)
          {
            int byte_index = (y * 48 + x) / 8;
            int bit_pos = 7 - (x % 8);
            bool px_on = (nav.icon[byte_index] >> bit_pos) & 0x01;
            setNavIconPx(x, y, px_on);
          }
        }
      }
    }

    if (actScr == ui_appInfoScreen)
    {
      lv_label_set_text_fmt(ui_appBatteryText, "Battery - %d%%", watch.getPhoneBattery());
      lv_bar_set_value(ui_appBatteryLevel, watch.getPhoneBattery(), LV_ANIM_OFF);
      if (watch.isPhoneCharging())
      {
        lv_image_set_src(ui_appBatteryIcon, &ui_img_battery_plugged_png);
      }
      else
      {
        lv_image_set_src(ui_appBatteryIcon, &ui_img_battery_state_png);
      }
    }

    if (alertTimer.active)
    {
      if (alertTimer.time + alertTimer.duration < millis())
      {
        alertTimer.active = false;
        lv_obj_add_flag(ui_alertPanel, LV_OBJ_FLAG_HIDDEN);
      }
    }

    if (screenTimer.active)
    {
      uint8_t lvl = lv_slider_get_value(ui_brightnessSlider);
      screenBrightness(lvl);

      if (screenTimer.duration < 0)
      {
        Timber.w("Always On active");
        screenTimer.active = false;
      }
      else if (watch.isCameraReady() || gameActive)
      {
        screenTimer.active = false;
      }
      else if (screenTimer.time + screenTimer.duration < millis())
      {
        Timber.w("Screen timeout");
        screenTimer.active = false;

        screenBrightness(0);
        lv_screen_load(ui_home);
      }
    }
  }

#ifdef ENABLE_CUSTOM_FACE
  if (writeFile && transfer)
  {
    if (start)
    {
      screenBrightness(200);
      tft.fillScreen(TFT_BLUE);

      tft.drawRoundRect(70, 120, 100, 20, 5, TFT_WHITE);
    }

    writeFile = false;

    File file = FLASH.open(fName, start ? FILE_WRITE : FILE_APPEND);
    if (file)
    {

      if (!wSwitch)
      {
        file.write(buf1, wLen1);
      }
      else
      {
        file.write(buf2, wLen2);
      }

      file.close();

      // Serial.print(last ? "Complete: " :  "");
      // Serial.print("Receieved ");
      // Serial.print(currentRecv);
      // Serial.print("/");
      // Serial.print(total);
      // Serial.print("   ");
      // Serial.println(hexString(cmd, 5, true, "-"));

      if (total > 0)
      {
        int progress = (100 * currentRecv) / total;

        // Serial.println(String(progress, 2) + "%");
        tft.setTextColor(TFT_WHITE, TFT_BLUE);
        tft.setTextSize(2);
        tft.setCursor(80, 80);
        tft.print(progress);
        tft.print("%");

        tft.fillRoundRect(70, 120, progress, 20, 5, TFT_WHITE);
      }

      if (last)
      {
        // the file transfer has ended
        transfer = false;

        tft.setTextColor(TFT_WHITE, TFT_BLUE);
        tft.setTextSize(2);
        tft.setCursor(60, 80);
        tft.print("Processing");

        parseDial(fName.c_str(), true); // process the file
      }
    }
    else
    {
      Serial.println("- failed to open file for writing");

      transfer = false;

      ESP.restart();
    }
  }

#endif
}

bool isDay()
{
  return watch.getHour(true) > 7 && watch.getHour(true) < 21;
}

void update_faces()
{
  int second = watch.getSecond();
  int minute = watch.getMinute();
  int hour = watch.getHourC();
  bool mode = watch.is24Hour();
  bool am = watch.getHour(true) < 12;
  int day = watch.getDay();
  int month = watch.getMonth() + 1;
  int year = watch.getYear();
  int weekday = watch.getDayofWeek();

  int temp = watch.getWeatherAt(0).temp;
  int icon = watch.getWeatherAt(0).icon;

  int battery = watch.getPhoneBattery();
  bool connection = watch.isConnected();

  int steps = 2735;
  int distance = 17;
  int kcal = 348;
  int bpm = 76;
  int oxygen = 97;

  if (ui_home == face_custom_root)
  {
    update_time_custom(second, minute, hour, mode, am, day, month, year, weekday);
  }
  else
  {

    ui_update_watchfaces(second, minute, hour, mode, am, day, month, year, weekday,
                         temp, icon, battery, connection, steps, distance, kcal, bpm, oxygen);
  }
}

bool readDialBytes(const char *path, uint8_t *data, size_t offset, size_t size)
{
  File file = FLASH.open(path, "r");
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return false;
  }

  if (!file.seek(offset))
  {
    Serial.println("Failed to seek file");
    file.close();
    return false;
  }

  int bytesRead = file.readBytes((char *)data, size);

  if (bytesRead <= 0)
  {
    Serial.println("Error reading file");
    file.close();
    return false;
  }

  file.close();
  return true;
}

bool isKnown(uint8_t id)
{
  if (id < 0x1E)
  {
    if (id != 0x04 || id != 0x05 || id != 0x12 || id != 0x18 || id != 0x20)
    {
      return true;
    }
  }
  else
  {
    if (id == 0xFA || id == 0xFD)
    {
      return true;
    }
  }
  return false;
}

String hexString(uint8_t *arr, size_t len, bool caps, String separator)
{
  String hexString = "";
  for (size_t i = 0; i < len; i++)
  {
    char hex[3];
    sprintf(hex, caps ? "%02X" : "%02x", arr[i]);
    hexString += separator;
    hexString += hex;
  }
  return hexString;
}

String longHexString(unsigned long l)
{
  char buffer[9];             // Assuming a 32-bit long, which requires 8 characters for hex representation and 1 for null terminator
  sprintf(buffer, "%08x", l); // Format as 8-digit hex with leading zeros
  return String(buffer);
}

void parseDial(const char *path, bool restart)
{

#ifdef ENABLE_CUSTOM_FACE
  String name = longHexString(watch.getEpoch());

  Serial.print("Parsing dial:");
  Serial.println(path);

  JsonDocument json;
  JsonDocument elements;
  JsonDocument assetFiles;
  JsonArray elArray = elements.to<JsonArray>();
  JsonArray assetArray = assetFiles.to<JsonArray>();

  json["name"] = name;
  json["file"] = String(path);

  JsonDocument rsc;
  int errors = 0;

  uint8_t az[1];
  if (!readDialBytes(path, az, 0, 1))
  {
    Serial.println("Failed to read watchface header");
    errors++;
  }
  uint8_t j = az[0];

  static uint8_t item[20];
  static uint8_t table[512];

  uint8_t lid = 0;
  int a = 0;
  int lan = 0;
  int tp = 0;
  int wt = 0;

  for (int i = 0; i < j; i++)
  {
    if (i >= 60)
    {
      Serial.println("Too many watchface elements >= 60");
      break;
    }

    JsonDocument element;

    if (!readDialBytes(path, item, (i * 20) + 4, 20))
    {
      Serial.println("Failed to read element properties");
      errors++;
    }

    uint8_t id = item[0];

    element["id"] = id;

    uint16_t xOff = item[5] * 256 + item[4];
    uint16_t yOff = item[7] * 256 + item[6];

    element["x"] = xOff;
    element["y"] = yOff;

    uint16_t xSz = item[9] * 256 + item[8];
    uint16_t ySz = item[11] * 256 + item[10];

    uint32_t clt = item[15] * 256 * 256 * 258 + item[14] * 256 * 256 + item[13] * 256 + item[12];
    uint32_t dat = item[19] * 256 * 256 * 256 + item[18] * 256 * 256 + item[17] * 256 + item[16];

    uint8_t id2 = item[1];

    bool isG = (item[1] & 0x80) == 0x80;

    if (id == 0x08)
    {
      isG = true;
    }

    uint8_t cmp = isG ? (item[1] & 0x7F) : 1;

    int aOff = item[2];

    bool isM = (item[3] & 0x80) == 0x80;
    uint8_t cG = isM ? (item[3] & 0x7F) : 1;

    if (!isKnown(id))
    {
      continue;
    }

    if (id == 0x16 && (item[1] == 0x06 || item[1] == 0x00))
    {
      // weather (-) label
      continue;
    }
    if (isM)
    {
      lan++;
    }

    if (tp == 0x09 && id == 0x09)
    {
      a++;
    }
    else if (tp != id)
    {
      tp = id;
      a++;
    }
    else if (lan == 1)
    {
      a++;
    }

    if (xSz == 0 || ySz == 0)
    {
      continue;
    }

    int z = i;
    int rs = -1;

    bool createFile = false;

    if (rsc.containsKey(String(clt)))
    {
      z = rsc[String(clt)].as<int>();
      rs = z;
    }

    bool drawable = (id == 0x0d) ? (lan == 1 || lan == 17 || lan == 33) : true;

    JsonDocument grp;
    JsonArray grpArr = grp.to<JsonArray>();

    if (rs == -1 && drawable)
    {
      rsc[String(clt)] = i;

      for (int aa = 0; aa < cmp; aa++)
      {
        unsigned long nm = (i * 10000) + (clt * 10) + aa;
        grpArr.add("S:" + name + "_" + longHexString(nm) + ".bin");
      }

      if (id == 0x17)
      {
      }
      else if (id == 0x0A)
      {
      }
      else if (cmp == 1)
      {
      }
      else
      {
      }

      // save asset
      createFile = true;
    }
    else if (id == 0x16 && id2 == 0x00)
    {
      // save asset
      createFile = true;

      for (int aa = 0; aa < cmp; aa++)
      {
        unsigned long nm = (z * 10000) + (clt * 10) + aa;
        grpArr.add("S:" + name + "_" + longHexString(nm) + ".bin");
      }
    }
    else
    {

      for (int aa = 0; aa < cmp; aa++)
      {
        unsigned long nm = (z * 10000) + (clt * 10) + aa;
        grpArr.add("S:" + name + "_" + longHexString(nm) + ".bin");
      }
    }

    if (cmp <= 1)
    {
      // grp is null
      grpArr.clear();
    }

    if (id == 0x0A)
    {
      // if (connIC.count { it == '\n' } < 3) {
      //     continue
      // }
    }

    if (isM)
    {
      if (lan == cG)
      {
        lan = 0;
      }
      else if (id == 0x0d && (lan == 1 || lan == 32 || lan == 40 || lan == 17 || lan == 33))
      {
        yOff -= (ySz - aOff);
        xOff -= aOff;
      }
      else
      {
        continue;
      }
    }
    if (id == 0x17)
    {
      wt++;
      if (wt != 1)
      {
        continue;
      }
    }

    if (id == 0x16 && id2 == 0x06)
    {
      continue;
    }

    if (drawable)
    {
      element["pvX"] = aOff;
      element["pvY"] = ySz - aOff;

      unsigned long nm = (z * 10000) + (clt * 10) + 0;

      element["image"] = "S:" + name + "_" + longHexString(nm) + ".bin";
      element["group"] = grpArr;

      elArray.add(element);
    }

    Serial.printf("i:%d, id:%d, xOff:%d, yOff:%d, xSz:%d, ySz:%d, clt:%d, dat:%d, cmp:%d\n", i, id, xOff, yOff, xSz, ySz, clt, dat, cmp);

    if (!createFile)
    {
      continue;
    }
    uint8_t cf = (id == 0x09 && i == 0) || (id == 0x19) ? 0x12 : 0x13;
    bool tr = cf == 0x13;
    uint16_t st = (cf == 0x12) ? 2 : 3;

    for (int b = 0; b < cmp; b++)
    {
      unsigned long nm = (z * 10000) + (clt * 10) + b;

      String asset = "/" + name + "_" + longHexString(nm) + ".bin";
      Serial.print("Create asset-> ");
      Serial.print(asset);

      assetArray.add(asset);

      uint8_t header[12];

      lvImgHeader(header, cf, xSz, ySz / cmp, xSz * st);

      Serial.print("\t");
      Serial.println(hexString(header, 12));

      File ast = FLASH.open(asset.c_str(), FILE_WRITE);
      if (ast)
      {
        ast.write(header, 12);

        if (!readDialBytes(path, table, clt, 512))
        {
          Serial.println("Could not read color table bytes from file");
          errors++;
          break;
        }

        uint16_t yZ = uint16_t(ySz / cmp); // height of individual element

        File file = FLASH.open(path, "r");
        if (!file)
        {
          Serial.println("Failed to open file for reading");
          errors++;
          break;
        }
        int offset = (xSz * yZ) * b;

        if (!file.seek(dat + offset))
        {
          Serial.println("Failed to seek file");
          file.close();
          errors++;
          break;
        }

        int x = 0;
        if (id == 0x19)
        {
          for (int z = 0; z < (xSz * yZ); z++)
          {
            uint8_t pixel[2];
            pixel[0] = item[13];
            pixel[1] = item[12];
            ast.write(pixel, 2);
          }
        }
        else
        {
          while (file.available())
          {
            uint16_t index = file.read();

            uint8_t pixel[3];
            if (tr)
            {
              pixel[1] = table[index * 2];
              pixel[2] = table[(index * 2) + 1];
              pixel[0] = (uint16_t(pixel[1] * 256 + pixel[2]) == 0) ? 0x00 : 0xFF; // alpha byte (black pixel [0] is transparent)
              ast.write(pixel, 3);
            }
            else
            {
              pixel[0] = table[index * 2];
              pixel[1] = table[(index * 2) + 1];

              ast.write(pixel, 2);
            }
            x++;
            if (x >= (xSz * yZ))
            {
              break;
            }
          }
        }
        file.close();

        ast.close();
      }
      else
      {
        errors++;
      }
    }
  }

  json["elements"] = elements;
  json["assets"] = assetFiles;

  serializeJsonPretty(json, Serial);

  String jsnFile = "/" + name + ".json";
  assetArray.add(jsnFile);
  File jsn = FLASH.open(jsnFile, FILE_WRITE);

  if (jsn)
  {
    serializeJsonPretty(json, jsn);
    jsn.flush();
    jsn.close();
  }
  else
  {
    errors++;
  }

  if (errors > 0)
  {
    // failed to parse watchface files
    // probably delete assetfiles
    Serial.print(errors);
    Serial.println(" errors encountered when parsing watchface");
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextSize(2);
    tft.setCursor(80, 80);
    tft.print("Failed");
  }
  else
  {
    // success
    // probably delete source file

    tft.fillScreen(TFT_GREEN);
    tft.setTextColor(TFT_WHITE, TFT_GREEN);
    tft.setTextSize(2);
    tft.setCursor(80, 80);
    tft.print("Success");

    deleteFile(path);
    Serial.println("Watchface parsed successfully");

    prefs.putString("custom", jsnFile);
  }

  if (restart)
  {
    delay(500);
    ESP.restart();
  }

#endif
}

bool lvImgHeader(uint8_t *byteArray, uint8_t cf, uint16_t w, uint16_t h, uint16_t stride)
{

  byteArray[0] = LV_IMAGE_HEADER_MAGIC;
  byteArray[1] = cf;
  byteArray[2] = 0;
  byteArray[3] = 0;

  byteArray[4] = (w & 0xFF);
  byteArray[5] = (w >> 8) & 0xFF;

  byteArray[6] = (h & 0xFF);
  byteArray[7] = (h >> 8) & 0xFF;

  byteArray[8] = (stride & 0xFF);
  byteArray[9] = (stride >> 8) & 0xFF;

  byteArray[10] = 0;
  byteArray[11] = 0;

  return true;
}
