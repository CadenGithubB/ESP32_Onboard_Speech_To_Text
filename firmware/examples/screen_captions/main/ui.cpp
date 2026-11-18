#include "ui.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "settings.h"

namespace ui {
namespace {
// ESP32-P4-EYE wiring: 240 x 240 ST7789 on SPI2; GPIO12 powers the screen (and the
// microphone); the backlight is on when GPIO20 is low. The wheel is a quadrature
// encoder with a push switch, all three pulled low when active.
constexpr gpio_num_t kLcdClk = GPIO_NUM_17, kLcdMosi = GPIO_NUM_16, kLcdDc = GPIO_NUM_19;
constexpr gpio_num_t kLcdCs = GPIO_NUM_18, kLcdReset = GPIO_NUM_15, kLcdBacklight = GPIO_NUM_20;
constexpr gpio_num_t kLcdPower = GPIO_NUM_12;
constexpr gpio_num_t kWheelA = GPIO_NUM_48, kWheelB = GPIO_NUM_47, kWheelPress = GPIO_NUM_2;
constexpr int kSize = 240, kBufferRows = 40;
constexpr int kStatusHeight = 24;
constexpr int kMaxCaptions = 12;

bool gReady = false;
lv_obj_t *gStatus = nullptr, *gLevel = nullptr, *gList = nullptr, *gDraft = nullptr;
lv_obj_t *gMenu = nullptr, *gRows[8] = {};
struct Shown {
  uint32_t id = 0;
  lv_obj_t* label = nullptr;
};
Shown gShown[kMaxCaptions];
uint32_t gNextCaption = 1;
char gStatusText[48] = "Starting";  // what the top line says when the board is not paused

const lv_color_t kSpeakerColors[8] = {
    lv_color_hex(0x6EC6FF), lv_color_hex(0xFFD166), lv_color_hex(0x9BE28A), lv_color_hex(0xF78FB3),
    lv_color_hex(0xC3A6FF), lv_color_hex(0xFF9F6E), lv_color_hex(0x7FE0D0), lv_color_hex(0xE0E0E0)};

struct Lock {  // LVGL is not thread-safe: every call goes through this
  bool held;
  Lock() : held(gReady && lvgl_port_lock(0)) {}
  ~Lock() {
    if (held) lvgl_port_unlock();
  }
};

// ---------------------------------------------------------------- settings menu
enum Row { kGain, kLm, kSpeakers, kDrafts, kNoisy, kPause, kBack, kRowCount };
int gSelected = 0;
bool gMenuOpen = false, gNeedsRestart = false;

void rowText(int row, char* text, size_t capacity) {
  using namespace settings;
  switch (row) {
    case kGain:
      if (kGainChoices[gain]) snprintf(text, capacity, "Mic gain: x%u", unsigned(kGainChoices[gain]));
      else snprintf(text, capacity, "Mic gain: auto (x%u)", unsigned(gainNow));
      break;
    case kLm: snprintf(text, capacity, "Language model: %u%%", unsigned(lmStrength) * 25); break;
    case kSpeakers: snprintf(text, capacity, "Speaker labels: %s", speakers ? "on" : "off"); break;
    case kDrafts: snprintf(text, capacity, "Live drafts: %s *", drafts ? "on" : "off"); break;
    case kNoisy: snprintf(text, capacity, "Noisy room: %s *", noisyRoom ? "on" : "off"); break;
    case kPause: snprintf(text, capacity, "End pause: %u.%u s *", kEndPauseMs[endPause] / 1000, kEndPauseMs[endPause] % 1000 / 100); break;
    default: snprintf(text, capacity, gNeedsRestart ? "Back (restarts)" : "Back"); break;
  }
}

void drawMenu() {
  char text[40];
  for (int row = 0; row < kRowCount; ++row) {
    rowText(row, text, sizeof(text));
    lv_label_set_text(gRows[row], text);
    const bool selected = row == gSelected;
    lv_obj_set_style_bg_opa(gRows[row], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(gRows[row], selected ? lv_color_black() : lv_color_white(), 0);
  }
}

void showStatus() {
  lv_label_set_text(gStatus, settings::listening ? gStatusText : "Paused: press wheel");
}

void changeSelected() {
  using namespace settings;
  switch (gSelected) {
    case kGain: gain = (gain + 1) % 5; break;
    case kLm: lmStrength = (lmStrength + 1) % 5; break;
    case kSpeakers: speakers = !speakers; break;
    case kDrafts: drafts = !drafts; gNeedsRestart = true; break;
    case kNoisy: noisyRoom = !noisyRoom; gNeedsRestart = true; break;
    case kPause: endPause = (endPause + 1) % 3; gNeedsRestart = true; break;
    default:
      save();
      if (gNeedsRestart) {
        lv_label_set_text(gRows[kBack], "Restarting...");
        lvgl_port_unlock();  // let the screen show it
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
      }
      gMenuOpen = false;
      lv_obj_add_flag(gMenu, LV_OBJ_FLAG_HIDDEN);
      return;
  }
  drawMenu();
}

// Captions showing: press pauses or resumes, a turn opens the settings.
// Settings showing: a turn moves the highlight, press changes the highlighted value.
void onWheel(int turned, bool pressed) {
  Lock lock;
  if (!lock.held) return;
  if (!gMenuOpen) {
    if (pressed) {
      settings::listening = !settings::listening;
      showStatus();
    } else if (turned) {
      gMenuOpen = true;
      gSelected = 0;
      drawMenu();
      lv_obj_remove_flag(gMenu, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  if (turned) {
    gSelected = (gSelected + turned % kRowCount + kRowCount) % kRowCount;
    drawMenu();
  }
  if (pressed) changeSelected();
}

// ---------------------------------------------------------------- wheel
std::atomic<int> gSteps{0};
uint8_t gWheelState = 0;

void IRAM_ATTR wheelEdge(void*) {
  // Quadrature: each valid transition is one step; four steps make one click of the wheel.
  static const int8_t kStep[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  const uint8_t now = uint8_t((gpio_get_level(kWheelA) << 1) | gpio_get_level(kWheelB));
  gSteps.fetch_add(kStep[(gWheelState << 2) | now], std::memory_order_relaxed);
  gWheelState = now;
}

void wheelTask(void*) {
  int carried = 0;
  bool wasDown = false;
  uint8_t stable = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    carried += gSteps.exchange(0, std::memory_order_relaxed);
    const int clicks = carried / 4;
    carried -= clicks * 4;
    const bool down = gpio_get_level(kWheelPress) == 0;
    stable = down == wasDown ? 0 : uint8_t(stable + 1);
    bool pressed = false;
    if (stable >= 2) {  // 40 ms debounce
      wasDown = down;
      stable = 0;
      pressed = down;
    }
    if (clicks || pressed) onWheel(clicks, pressed);
  }
}

bool startWheel() {
  gpio_config_t pins = {};
  pins.pin_bit_mask = (1ULL << kWheelA) | (1ULL << kWheelB);
  pins.mode = GPIO_MODE_INPUT;
  pins.pull_up_en = GPIO_PULLUP_ENABLE;
  pins.intr_type = GPIO_INTR_ANYEDGE;
  if (gpio_config(&pins) != ESP_OK) return false;
  pins.pin_bit_mask = 1ULL << kWheelPress;
  pins.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&pins) != ESP_OK) return false;
  gWheelState = uint8_t((gpio_get_level(kWheelA) << 1) | gpio_get_level(kWheelB));
  const esp_err_t service = gpio_install_isr_service(0);
  if (service != ESP_OK && service != ESP_ERR_INVALID_STATE) return false;
  gpio_isr_handler_add(kWheelA, wheelEdge, nullptr);
  gpio_isr_handler_add(kWheelB, wheelEdge, nullptr);
  return xTaskCreatePinnedToCore(wheelTask, "wheel", 4096, nullptr, 2, nullptr, 0) == pdPASS;
}

// ---------------------------------------------------------------- screen
lv_obj_t* textLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, lv_pct(100));
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_text(label, "");
  return label;
}

void plainBox(lv_obj_t* box) {
  lv_obj_set_style_bg_color(box, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_pad_all(box, 4, 0);
  lv_obj_set_style_pad_row(box, 6, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
}

void build() {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  gStatus = lv_label_create(screen);
  lv_obj_set_style_text_font(gStatus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(gStatus, lv_color_hex(0x9BE28A), 0);
  lv_obj_set_pos(gStatus, 4, 4);
  lv_label_set_text(gStatus, gStatusText);

  gLevel = lv_bar_create(screen);  // how loud the microphone hears the room
  lv_obj_set_size(gLevel, 56, 8);
  lv_obj_set_pos(gLevel, kSize - 60, 8);
  lv_bar_set_range(gLevel, 0, 100);

  gList = lv_obj_create(screen);
  lv_obj_set_pos(gList, 0, kStatusHeight);
  lv_obj_set_size(gList, kSize, kSize - kStatusHeight);
  plainBox(gList);
  gDraft = textLabel(gList, &lv_font_montserrat_16, lv_color_hex(0x808080));

  gMenu = lv_obj_create(screen);
  lv_obj_set_pos(gMenu, 0, kStatusHeight);
  lv_obj_set_size(gMenu, kSize, kSize - kStatusHeight);
  plainBox(gMenu);
  for (int row = 0; row < kRowCount; ++row) {
    gRows[row] = textLabel(gMenu, &lv_font_montserrat_16, lv_color_white());
    lv_obj_set_style_bg_color(gRows[row], lv_color_hex(0x6EC6FF), 0);
    lv_obj_set_style_pad_all(gRows[row], 2, 0);
  }
  lv_obj_t* note = textLabel(gMenu, &lv_font_montserrat_14, lv_color_hex(0x808080));
  lv_label_set_text(note, "* restarts the board to apply");
  lv_obj_add_flag(gMenu, LV_OBJ_FLAG_HIDDEN);
}
}  // namespace

bool begin() {
  // The screen shares its supply with the microphone. Another firmware may have left
  // the pin held through sleep, so release it before driving it.
  rtc_gpio_hold_dis(kLcdPower);
  rtc_gpio_deinit(kLcdPower);
  gpio_set_level(kLcdPower, 1);
  gpio_set_direction(kLcdPower, GPIO_MODE_OUTPUT);
  gpio_set_level(kLcdBacklight, 1);  // off until there is a picture
  gpio_set_direction(kLcdBacklight, GPIO_MODE_OUTPUT);

  spi_bus_config_t bus = {};
  bus.sclk_io_num = kLcdClk;
  bus.mosi_io_num = kLcdMosi;
  bus.miso_io_num = bus.quadwp_io_num = bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kSize * kBufferRows * 2;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t ioConfig = {};
  ioConfig.cs_gpio_num = kLcdCs;
  ioConfig.dc_gpio_num = kLcdDc;
  ioConfig.spi_mode = 0;
  ioConfig.pclk_hz = 20 * 1000 * 1000;
  ioConfig.trans_queue_depth = 10;
  ioConfig.lcd_cmd_bits = 8;
  ioConfig.lcd_param_bits = 8;
  if (esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t(SPI2_HOST), &ioConfig, &io) != ESP_OK) return false;

  esp_lcd_panel_handle_t panel = nullptr;
  esp_lcd_panel_dev_config_t panelConfig = {};
  panelConfig.reset_gpio_num = kLcdReset;
  panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panelConfig.bits_per_pixel = 16;
  if (esp_lcd_new_panel_st7789(io, &panelConfig, &panel) != ESP_OK || esp_lcd_panel_reset(panel) != ESP_OK ||
      esp_lcd_panel_init(panel) != ESP_OK || esp_lcd_panel_invert_color(panel, true) != ESP_OK ||
      esp_lcd_panel_disp_on_off(panel, true) != ESP_OK)
    return false;

  lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
  port.task_priority = 2;  // below the microphone, level with transcription
  port.task_affinity = 0;
  if (lvgl_port_init(&port) != ESP_OK) return false;
  lvgl_port_display_cfg_t display = {};
  display.io_handle = io;
  display.panel_handle = panel;
  display.buffer_size = kSize * kBufferRows;
  display.hres = kSize;
  display.vres = kSize;
  display.color_format = LV_COLOR_FORMAT_RGB565;
  display.flags.buff_dma = true;
  display.flags.swap_bytes = true;
  if (!lvgl_port_add_disp(&display)) return false;

  gReady = true;
  {
    Lock lock;
    build();
  }
  vTaskDelay(pdMS_TO_TICKS(100));  // first frame drawn before the light comes on
  gpio_set_level(kLcdBacklight, 0);
  if (!startWheel()) printf("Wheel did not start: settings are fixed\n");
  return true;
}

void status(const char* text) {
  snprintf(gStatusText, sizeof(gStatusText), "%s", text);
  Lock lock;
  if (lock.held) showStatus();
}

void level(uint16_t level, uint16_t noise) {
  // Decibels above the room's own noise, 0..40 dB across the bar.
  const float db = 20.0f * std::log10((float(level) + 1.0f) / (float(noise) + 1.0f));
  const int value = db <= 0 ? 0 : db >= 40 ? 100 : int(db * 2.5f);
  Lock lock;
  if (lock.held) lv_bar_set_value(gLevel, settings::listening ? value : 0, LV_ANIM_OFF);
}

uint32_t caption(const char* text) {
  const uint32_t id = gNextCaption++;
  Lock lock;
  if (!lock.held) return id;
  if (gShown[0].label) lv_obj_delete(gShown[0].label);  // oldest caption leaves
  for (int i = 0; i + 1 < kMaxCaptions; ++i) gShown[i] = gShown[i + 1];
  lv_obj_t* label = textLabel(gList, &lv_font_montserrat_16, lv_color_white());
  lv_label_set_text(label, text);
  gShown[kMaxCaptions - 1] = {id, label};
  lv_label_set_text(gDraft, "");
  lv_obj_move_foreground(gDraft);  // the draft stays underneath the newest caption
  lv_obj_update_layout(gList);
  lv_obj_scroll_to_view(gDraft, LV_ANIM_OFF);
  return id;
}

void speaker(uint32_t captionId, int who) {
  if (who < 0 || who > 7) return;
  Lock lock;
  if (!lock.held) return;
  for (const Shown& shown : gShown) {
    if (shown.id != captionId || !shown.label) continue;
    static char text[600];
    snprintf(text, sizeof(text), "%d: %s", who + 1, lv_label_get_text(shown.label));
    lv_label_set_text(shown.label, text);
    lv_obj_set_style_text_color(shown.label, kSpeakerColors[who], 0);
    lv_obj_update_layout(gList);
    lv_obj_scroll_to_view(gDraft, LV_ANIM_OFF);
  }
}

void draft(const char* text) {
  Lock lock;
  if (!lock.held) return;
  lv_label_set_text(gDraft, text);
  lv_obj_update_layout(gList);
  lv_obj_scroll_to_view(gDraft, LV_ANIM_OFF);
}

void fatal(const char* text) {
  Lock lock;
  if (!lock.held) return;
  lv_obj_set_style_text_color(gStatus, lv_color_hex(0xFF6E6E), 0);
  lv_label_set_text(gStatus, "Stopped");
  lv_label_set_text(gDraft, text);
  lv_obj_set_style_text_color(gDraft, lv_color_white(), 0);
}
}  // namespace ui
