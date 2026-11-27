/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "recovery_ui/screen_ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <aidl/android/hardware/health/BatteryStatus.h>

#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <health/utils.h>
#include <healthd/BatteryMonitor.h>

#include "minui/minui.h"
#include "otautil/paths.h"
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"

enum DirectRenderManager {
    DRM_INNER,
    DRM_OUTER,
};

// Return the current time as a double (including fractions of a second).
static double now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

Menu::Menu(size_t initial_selection, const DrawInterface& draw_func)
    : selection_(initial_selection), draw_funcs_(draw_func) {}

int Menu::selection() const {
  return selection_;
}

TextMenu::TextMenu(bool wrappable, size_t max_length,
                   const std::vector<std::string>& headers, const std::vector<std::string>& items,
                   size_t initial_selection, int char_height, const DrawInterface& draw_funcs)
    : Menu(initial_selection, draw_funcs),
      wrappable_(wrappable),
      calibrated_height_(false),
      max_item_length_(max_length),
      text_headers_(headers),
      char_height_(char_height) {

  size_t items_count = items.size();
  for (size_t i = 0; i < items_count; ++i) {
    text_items_.emplace_back(items[i].substr(0, max_item_length_));
  }

  CHECK(!text_items_.empty());
}

const std::vector<std::string>& TextMenu::text_headers() const {
  return text_headers_;
}

std::string TextMenu::TextItem(size_t index) const {
  CHECK_LT(index, text_items_.size());

  return text_items_[index];
}

size_t TextMenu::MenuStart() const {
  return menu_start_;
}

size_t TextMenu::MenuEnd() const {
  return std::min(ItemsCount(), menu_start_ + max_display_items_);
}

size_t TextMenu::ItemsCount() const {
  return text_items_.size();
}

bool TextMenu::ItemsOverflow(std::string* cur_selection_str) const {
  if (ItemsCount() <= max_display_items_) {
    return false;
  }

  *cur_selection_str =
      android::base::StringPrintf("Current item: %d/%zu", selection_ + 1, ItemsCount());
  return true;
}

// TODO(xunchang) modify the function parameters to button up & down.
int TextMenu::Select(int sel) {
  CHECK_LE(ItemsCount(), static_cast<size_t>(std::numeric_limits<int>::max()));
  int count = ItemsCount();

  int min = IsMain() ? 0 : -1; // -1 is back arrow

  if (sel < min) {
    selection_ = wrappable() ? count - 1 : min;
  } else if (sel >= count) {
    selection_ = wrappable() ? min : count - 1;
  } else {
    selection_ = sel;
  }

  if (selection_ >= 0) {
    if (selection_ < menu_start_) {
      menu_start_ = selection_;
    } else if (static_cast<size_t>(selection_) >= MenuEnd()) {
      menu_start_ = selection_ - max_display_items_ + 1;
    }
  }

  return selection_;
}

int TextMenu::SelectVisible(int relative_sel) {
  int sel = relative_sel;
  if (menu_start_ > 0) {
    sel += menu_start_;
  }

  return Select(sel);
}

int TextMenu::Scroll(int updown) {
  if ((updown > 0 && menu_start_ + max_display_items_ < ItemsCount()) ||
      (updown < 0 && menu_start_ > 0)) {
    menu_start_ += updown;

    /* We can receive a kInvokeItem event from a different source than touch,
       like from Power button. For this reason, selection should not get out of
       the screen. Constrain it to the first or last visible item of the list */
    if (selection_ < menu_start_) {
      selection_ = menu_start_;
    } else if (selection_ >= menu_start_ + max_display_items_) {
      selection_ = menu_start_ + max_display_items_ - 1;
    }
  }
  return selection_;
}

int TextMenu::DrawHeader(int x, int y) const {
  int offset = 0;

  draw_funcs_.SetColor(UIElement::HEADER);
  offset += draw_funcs_.DrawWrappedTextLines(x, y + offset, text_headers());

  return offset;
}

int TextMenu::DrawItems(int /*x*/, int y, int screen_width, bool long_press) const {
  int horizontal_rule_height = 8;
  int offset = 0;
  int padding = draw_funcs_.MenuItemPadding();
  int spacing = draw_funcs_.MenuItemSpacing();

  draw_funcs_.SetColor(UIElement::MENU);
  offset += horizontal_rule_height + 4;

  int item_container_offset = offset; // store it for drawing scrollbar on most top

  for (size_t i = MenuStart(); i < MenuEnd(); ++i) {
    const auto selected = i == selection();

    // Draw the rounded action card.
    draw_funcs_.SetColor(long_press ? UIElement::MENU_SEL_BG_ACTIVE
                         : selected ? UIElement::MENU_SEL_BG
                                    : UIElement::MENU_BG);

    int bar_height = padding + char_height_ + padding;
    draw_funcs_.DrawHighlightBar(padding, y + offset, screen_width - (padding * 2), bar_height);

    draw_funcs_.SetColor(selected ? UIElement::MENU_SEL_FG : UIElement::MENU);
    offset += draw_funcs_.DrawTextLine(padding * 2, y + offset, TextItem(i), selected);
    offset += spacing;
  }
  offset += horizontal_rule_height;

  std::string unused;
  if (ItemsOverflow(&unused)) {
    int container_height = max_display_items_ * (2 * padding + char_height_ + spacing);
    int bar_height = container_height / (text_items_.size() - max_display_items_ + 1);
    int start_y = y + item_container_offset + bar_height * menu_start_;
    draw_funcs_.SetColor(UIElement::SCROLLBAR);
    draw_funcs_.DrawScrollBar(start_y, bar_height);
  }

  return offset;
}

GraphicMenu::GraphicMenu(const GRSurface* graphic_headers,
                         const std::vector<const GRSurface*>& graphic_items,
                         size_t initial_selection, const DrawInterface& draw_funcs)
    : Menu(initial_selection, draw_funcs) {
  graphic_headers_ = graphic_headers->Clone();
  graphic_items_.reserve(graphic_items.size());
  for (const auto& item : graphic_items) {
    graphic_items_.emplace_back(item->Clone());
  }
}

// Define destructor out-of-line so that GRSurface is defined for unique_ptr.
GraphicMenu::~GraphicMenu() = default;

int GraphicMenu::Select(int sel) {
  CHECK_LE(graphic_items_.size(), static_cast<size_t>(std::numeric_limits<int>::max()));
  int count = graphic_items_.size();

  // Wraps the selection at boundary if the menu is not scrollable.
  if (sel < 0) {
    selection_ = count - 1;
  } else if (sel >= count) {
    selection_ = 0;
  } else {
    selection_ = sel;
  }

  return selection_;
}

int GraphicMenu::DrawHeader(int x, int y) const {
  draw_funcs_.SetColor(UIElement::HEADER);
  draw_funcs_.DrawTextIcon(x, y, graphic_headers_.get());
  return graphic_headers_->height;
}

int GraphicMenu::DrawItems(int x, int y, int screen_width, bool long_press) const {
  int offset = 0;

  draw_funcs_.SetColor(UIElement::MENU);
  offset += draw_funcs_.DrawHorizontalRule(y + offset) + 4;

  for (size_t i = 0; i < graphic_items_.size(); i++) {
    auto& item = graphic_items_[i];
    if (i == selection_) {
      draw_funcs_.SetColor(long_press ? UIElement::MENU_SEL_BG_ACTIVE : UIElement::MENU_SEL_BG);

      int bar_height = item->height + 4;
      draw_funcs_.DrawHighlightBar(0, y + offset - 2, screen_width, bar_height);

      // Bold white text for the selected item.
      draw_funcs_.SetColor(UIElement::MENU_SEL_FG);
    }
    draw_funcs_.DrawTextIcon(x, y + offset, item.get());
    offset += item->height;

    draw_funcs_.SetColor(UIElement::MENU);
  }
  offset += draw_funcs_.DrawHorizontalRule(y + offset);

  return offset;
}

size_t GraphicMenu::ItemsCount() const {
  return graphic_items_.size();
}

bool GraphicMenu::Validate(size_t max_width, size_t max_height, const GRSurface* graphic_headers,
                           const std::vector<const GRSurface*>& graphic_items) {
  int offset = 0;
  if (!ValidateGraphicSurface(max_width, max_height, offset, graphic_headers)) {
    return false;
  }
  offset += graphic_headers->height;

  for (const auto& item : graphic_items) {
    if (!ValidateGraphicSurface(max_width, max_height, offset, item)) {
      return false;
    }
    offset += item->height;
  }

  return true;
}

bool GraphicMenu::ValidateGraphicSurface(size_t max_width, size_t max_height, int y,
                                         const GRSurface* surface) {
  if (!surface) {
    fprintf(stderr, "Graphic surface can not be null\n");
    return false;
  }

  if (surface->pixel_bytes != 1 || surface->width != surface->row_bytes) {
    fprintf(stderr, "Invalid graphic surface, pixel bytes: %zu, width: %zu row_bytes: %zu\n",
            surface->pixel_bytes, surface->width, surface->row_bytes);
    return false;
  }

  if (surface->width > max_width || surface->height > max_height - y) {
    fprintf(stderr,
            "Graphic surface doesn't fit into the screen. width: %zu, height: %zu, max_width: %zu,"
            " max_height: %zu, vertical offset: %d\n",
            surface->width, surface->height, max_width, max_height, y);
    return false;
  }

  return true;
}

MenuDrawFunctions::MenuDrawFunctions(const DrawInterface& wrappee)
    : wrappee_(wrappee) {
}

int MenuDrawFunctions::DrawTextLine(int x, int y, const std::string& line, bool bold) const {
  gr_text(gr_menu_font(), x, y + MenuItemPadding(), line.c_str(), bold);
  return 2 * MenuItemPadding() + MenuCharHeight();
}

int MenuDrawFunctions::DrawTextLines(int x, int y, const std::vector<std::string>& lines) const {
  int offset = 0;
  for (const auto& line : lines) {
    offset += DrawTextLine(x, y + offset, line, false);
  }
  return offset;
}

int MenuDrawFunctions::DrawWrappedTextLines(int x, int y, const std::vector<std::string>& lines) const {
  const int padding = MenuItemPadding() / 2;

  // Keep symmetrical margins based on the given offset (i.e. x).
  size_t text_cols = (gr_fb_width() - x * 2) / MenuCharWidth();
  int offset = 0;
  for (const auto& line : lines) {
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, text_cols + 1);
      if (sub.size() <= text_cols) {
        next_start += sub.size();
      } else {
        // Line too long and must be wrapped to text_cols columns.
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          // No space found, just draw as much as we can.
          sub.resize(text_cols);
          next_start += text_cols;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      offset += DrawTextLine(x, y + offset, sub, false) - (2 * MenuItemPadding() - padding);
    }
  }
  if (!lines.empty()) {
    offset += 2 * MenuItemPadding() - padding;
  }
  return offset;
}

constexpr int kDefaultMarginHeight = 0;
constexpr int kDefaultMarginWidth = 0;
constexpr int kDefaultAnimationFps = 30;

ScreenRecoveryUI::ScreenRecoveryUI()
    : margin_width_(
          android::base::GetIntProperty("ro.recovery.ui.margin_width", kDefaultMarginWidth)),
      margin_height_(
          android::base::GetIntProperty("ro.recovery.ui.margin_height", kDefaultMarginHeight)),
      animation_fps_(
          android::base::GetIntProperty("ro.recovery.ui.animation_fps", kDefaultAnimationFps)),
      density_(static_cast<float>(android::base::GetIntProperty("ro.sf.lcd_density", 160)) / 160.f),
      blank_unblank_on_init_(
          android::base::GetBoolProperty("ro.recovery.ui.blank_unblank_on_init", false)),
      current_icon_(NONE),
      current_frame_(0),
      intro_done_(false),
      progressBarType(EMPTY),
      progressScopeStart(0),
      progressScopeSize(0),
      progress(0),
      pagesIdentical(false),
      text_cols_(0),
      text_rows_(0),
      text_(nullptr),
      text_col_(0),
      text_row_(0),
      show_text(false),
      show_text_ever(false),
      file_viewer_text_(nullptr),
      stage(-1),
      max_stage(-1),
      locale_(""),
      rtl_locale_(false),
      batt_capacity_(0),
      charging_(false),
      is_graphics_available(false) {}

ScreenRecoveryUI::~ScreenRecoveryUI() {
  batt_monitor_thread_stopped_ = true;
  if (batt_monitor_thread_.joinable()) {
    batt_monitor_thread_.join();
  }

  progress_thread_stopped_ = true;
  if (progress_thread_.joinable()) {
    progress_thread_.join();
  }
  // No-op if gr_init() (via Init()) was not called or had failed.
  gr_exit();
}

const GRSurface* ScreenRecoveryUI::GetCurrentFrame() const {
  if (current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING) {
    return intro_done_ ? loop_frames_[current_frame_].get() : intro_frames_[current_frame_].get();
  }
  return error_icon_.get();
}

const GRSurface* ScreenRecoveryUI::GetCurrentText() const {
  switch (current_icon_) {
    case ERASING:
      return erasing_text_.get();
    case ERROR:
      return error_text_.get();
    case INSTALLING_UPDATE:
      return installing_text_.get();
    case NO_COMMAND:
      return no_command_text_.get();
    case NONE:
      abort();
  }
}

int ScreenRecoveryUI::PixelsFromDp(int dp) const {
  return dp * density_;
}

// Here's the intended layout:

//          | portrait    large        landscape      large
// ---------+-------------------------------------------------
//      gap |
// icon     |                   (200dp)
//      gap |    68dp      68dp             56dp      112dp
// text     |                    (14sp)
//      gap |    32dp      32dp             26dp       52dp
// progress |                     (2dp)
//      gap |

// Note that "baseline" is actually the *top* of each icon (because that's how our drawing routines
// work), so that's the more useful measurement for calling code. We use even top and bottom gaps.

enum Layout { PORTRAIT = 0, PORTRAIT_LARGE = 1, LANDSCAPE = 2, LANDSCAPE_LARGE = 3, LAYOUT_MAX };
enum Dimension { TEXT = 0, ICON = 1, DIMENSION_MAX };
static constexpr int kLayouts[LAYOUT_MAX][DIMENSION_MAX] = {
  { 32, 68 },   // PORTRAIT
  { 32, 68 },   // PORTRAIT_LARGE
  { 26, 56 },   // LANDSCAPE
  { 52, 112 },  // LANDSCAPE_LARGE
};

int ScreenRecoveryUI::GetAnimationBaseline() const {
  return GetTextBaseline() - PixelsFromDp(kLayouts[layout_][ICON]) -
         gr_get_height(loop_frames_[0].get());
}

int ScreenRecoveryUI::GetTextBaseline() const {
  return GetProgressBaseline() - PixelsFromDp(kLayouts[layout_][TEXT]) -
         gr_get_height(installing_text_.get());
}

int ScreenRecoveryUI::GetProgressBaseline() const {
  int elements_sum = gr_get_height(loop_frames_[0].get()) + PixelsFromDp(kLayouts[layout_][ICON]) +
                     gr_get_height(installing_text_.get()) + PixelsFromDp(kLayouts[layout_][TEXT]) +
                     gr_get_height(progress_bar_fill_.get());
  int bottom_gap = (ScreenHeight() - elements_sum) / 2;
  return ScreenHeight() - bottom_gap - gr_get_height(progress_bar_fill_.get());
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_background_locked() {
  pagesIdentical = false;
  gr_color(0x08, 0x09, 0x0C, 255);
  gr_clear();
  if (current_icon_ != NONE) {
    if (max_stage != -1) {
      int stage_height = gr_get_height(stage_marker_empty_.get());
      int stage_width = gr_get_width(stage_marker_empty_.get());
      int x = (ScreenWidth() - max_stage * gr_get_width(stage_marker_empty_.get())) / 2;
      int y = ScreenHeight() - stage_height - margin_height_;
      for (int i = 0; i < max_stage; ++i) {
        const auto& stage_surface = (i < stage) ? stage_marker_fill_ : stage_marker_empty_;
        DrawSurface(stage_surface.get(), 0, 0, stage_width, stage_height, x, y);
        x += stage_width;
      }
    }

    const auto& text_surface = GetCurrentText();
    int text_x = (ScreenWidth() - gr_get_width(text_surface)) / 2;
    int text_y = GetTextBaseline();
    gr_color(0xE8, 0xEC, 0xF4, 255);
    DrawTextIcon(text_x, text_y, text_surface);
  }
}

void ScreenRecoveryUI::draw_modern_progress_bar_locked(int x, int y, int width, int height) const {
  if (width <= 0 || height <= 0) return;
  const int radius = height / 2;

  // Track background
  gr_color(0x22, 0x26, 0x30, 255);
  gr_fill_rounded(x, y, x + width, y + height, radius);

  if (progressBarType == DETERMINATE) {
    float p = std::clamp(progressScopeStart + progress * progressScopeSize, 0.0f, 1.0f);
    int fill_w = static_cast<int>(p * width);
    if (fill_w > 0) {
      int clamped_w = std::max(fill_w, std::min(width, height));
      int fx1 = rtl_locale_ ? (x + width - clamped_w) : x;
      int fx2 = fx1 + clamped_w;
      if (fastbootd_logo_enabled_) {
        gr_color(0xD8, 0x4B, 0x00, 255);
      } else {
        gr_color(0xB3, 0x13, 0x29, 255);
      }
      gr_fill_rounded(fx1, y, fx2, y + height, radius);

      // Animated subtle leading highlight shimmer
      int shimmer_w = std::min(clamped_w, PixelsFromDp(20));
      if (shimmer_w > 4) {
        float pulse = 0.5f - 0.5f * std::cos(install_anim_phase_ * 6.2831853f);
        uint8_t a = static_cast<uint8_t>(70 + pulse * 110);
        if (fastbootd_logo_enabled_) {
          gr_color(0xFF, 0xB7, 0x4D, a);
        } else {
          gr_color(0xFF, 0x6B, 0x7D, a);
        }
        int sx1 = rtl_locale_ ? fx1 : (fx2 - shimmer_w);
        gr_fill_rounded(sx1, y, sx1 + shimmer_w, y + height, radius);
      }
    }
  } else if (progressBarType == INDETERMINATE ||
             (progressBarType == EMPTY &&
              (current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING))) {
    int pill_w = std::max(height * 2, width * 28 / 100);
    float travel = static_cast<float>(width - pill_w);
    // Smooth back-and-forth sine sweep at 30 FPS
    float eased = 0.5f - 0.5f * std::cos(install_anim_phase_ * 6.2831853f);
    int px = x + static_cast<int>(eased * travel);
    if (fastbootd_logo_enabled_) {
      gr_color(0xFF, 0x70, 0x20, 255);
    } else {
      gr_color(0xE8, 0x38, 0x4F, 255);
    }
    gr_fill_rounded(px, y, px + pill_w, y + height, radius);
  }
}

// Draws the animation and progress bar (if any) on the screen. Does not flip pages. Should only be
// called with updateMutex locked.
void ScreenRecoveryUI::draw_foreground_locked() {
  if (current_icon_ != NONE) {
    const auto& frame = GetCurrentFrame();
    int frame_width = gr_get_width(frame);
    int frame_height = gr_get_height(frame);
    int frame_x = (ScreenWidth() - frame_width) / 2;
    int frame_y = GetAnimationBaseline();
    if (frame_x >= 0 && frame_y >= 0 && (frame_x + frame_width) < ScreenWidth() &&
        (frame_y + frame_height) < ScreenHeight())
      DrawSurface(frame, 0, 0, frame_width, frame_height, frame_x, frame_y);
  }

  if (progressBarType != EMPTY) {
    int width = gr_get_width(progress_bar_empty_.get());
    int height = std::max(PixelsFromDp(10), static_cast<int>(gr_get_height(progress_bar_empty_.get())));

    int progress_x = (ScreenWidth() - width) / 2;
    int progress_y = GetProgressBaseline();

    // Erase behind the progress bar (in case this was a progress-only update)
    gr_color(0x08, 0x09, 0x0C, 255);
    gr_fill(std::max(0, progress_x - 4), std::max(0, progress_y - 4),
            std::min(ScreenWidth(), progress_x + width + 4),
            std::min(ScreenHeight(), progress_y + height + 4));
    draw_modern_progress_bar_locked(progress_x, progress_y, width, height);
  }
}

/* recovery dark:  #7C4DFF
   recovery dxpe:  #9c091d
   recovery light: #F890FF
   recovery lxpe:  #752229
   fastbootd dark: #E65100
   fastboot light: #FDD835 */
void ScreenRecoveryUI::SetColor(UIElement e) const {
  current_element_ = e;
  switch (e) {
    case UIElement::BATTERY_LOW:
      if (fastbootd_logo_enabled_)
        gr_color(0xfd, 0x35, 0x35, 255);
      else
        gr_color(0xf8, 0x71, 0x71, 255);
      break;
    case UIElement::INFO:
      if (fastbootd_logo_enabled_)
        gr_color(0xfd, 0xd8, 0x35, 255);
      else
        gr_color(0x9b, 0xa1, 0xae, 255);
      break;
    case UIElement::HEADER:
      if (fastbootd_logo_enabled_)
        gr_color(0xfd, 0xd8, 0x35, 255);
      else
        gr_color(0xe8, 0xec, 0xf4, 255);
      break;
    case UIElement::MENU:
      gr_color(0xe8, 0xec, 0xf4, 255);
      break;
    case UIElement::MENU_BG:
      gr_color(0x16, 0x18, 0x1e, 255);
      break;
    case UIElement::MENU_SEL_BG:
      if (fastbootd_logo_enabled_)
        gr_color(0xd8, 0x4b, 0x00, 255);
      else
        gr_color(0xb3, 0x13, 0x29, 255);
      break;
    case UIElement::SCROLLBAR:
      if (fastbootd_logo_enabled_)
        gr_color(0xff, 0x8f, 0x00, 255);
      else
        gr_color(0xe8, 0x38, 0x4f, 255);
      break;
    case UIElement::MENU_SEL_BG_ACTIVE:
      gr_color(0x00, 0xa8, 0x6b, 255);
      break;
    case UIElement::MENU_SEL_FG:
      gr_color(0xff, 0xff, 0xff, 255);
      break;
    case UIElement::LOG:
      gr_color(0xa8, 0xb0, 0xbe, 255);
      break;
    case UIElement::TEXT_FILL:
      gr_color(0x08, 0x09, 0x0c, 200);
      break;
    default:
      gr_color(255, 255, 255, 255);
      break;
  }
}

void ScreenRecoveryUI::SelectAndShowBackgroundText(const std::vector<std::string>& locales_entries,
                                                   size_t sel) {
  SetLocale(locales_entries[sel]);
  std::vector<std::string> text_name = { "erasing_text", "error_text", "installing_text",
                                         "installing_security_text", "no_command_text" };
  std::unordered_map<std::string, std::unique_ptr<GRSurface>> surfaces;
  for (const auto& name : text_name) {
    auto text_image = LoadLocalizedBitmap(name);
    if (!text_image) {
      Print("Failed to load %s\n", name.c_str());
      return;
    }
    surfaces.emplace(name, std::move(text_image));
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  gr_color(0, 0, 0, 255);
  gr_clear();

  int text_y = margin_height_;
  int text_x = margin_width_;
  int line_spacing = gr_sys_font()->char_height;  // Put some extra space between images.
  // Write the header and descriptive texts.
  SetColor(UIElement::INFO);
  std::string header = "Show background text image";
  text_y += DrawTextLine(text_x, text_y, header, true);
  std::string locale_selection = android::base::StringPrintf(
      "Current locale: %s, %zu/%zu", locales_entries[sel].c_str(), sel + 1, locales_entries.size());
  // clang-format off
  std::vector<std::string> instruction = {
    locale_selection,
    "Use volume up/down to switch locales and power to exit."
  };
  // clang-format on
  text_y += DrawWrappedTextLines(text_x, text_y, instruction);

  // Iterate through the text images and display them in order for the current locale.
  for (const auto& p : surfaces) {
    text_y += line_spacing;
    SetColor(UIElement::LOG);
    text_y += DrawTextLine(text_x, text_y, p.first, false);
    gr_color(255, 255, 255, 255);
    gr_texticon(text_x, text_y, p.second.get());
    text_y += gr_get_height(p.second.get());
  }
  // Update the whole screen.
  gr_flip();
}

void ScreenRecoveryUI::CheckBackgroundTextImages() {
  // Load a list of locales embedded in one of the resource files.
  std::vector<std::string> locales_entries = get_locales_in_png("installing_text");
  if (locales_entries.empty()) {
    Print("Failed to load locales from the resource files\n");
    return;
  }
  std::string saved_locale = locale_;
  size_t selected = 0;
  SelectAndShowBackgroundText(locales_entries, selected);

  FlushKeys();
  while (true) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) break;
    }
    if (evt.type() == EventType::KEY) {
      if (evt.key() == KEY_POWER || evt.key() == KEY_ENTER) {
        break;
      } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP) {
        selected = (selected == 0) ? locales_entries.size() - 1 : selected - 1;
        SelectAndShowBackgroundText(locales_entries, selected);
      } else if (evt.key() == KEY_DOWN || evt.key() == KEY_VOLUMEDOWN) {
        selected = (selected == locales_entries.size() - 1) ? 0 : selected + 1;
        SelectAndShowBackgroundText(locales_entries, selected);
      }
    }
  }

  SetLocale(saved_locale);
}

int ScreenRecoveryUI::ScreenWidth() const {
  return gr_fb_width();
}

int ScreenRecoveryUI::ScreenHeight() const {
  return gr_fb_height();
}

void ScreenRecoveryUI::DrawSurface(const GRSurface* surface, int sx, int sy, int w, int h, int dx,
                                   int dy) const {
  gr_blit(surface, sx, sy, w, h, dx, dy);
}

int ScreenRecoveryUI::DrawHorizontalRule(int y) const {
  const int pad = std::max(margin_width_ + PixelsFromDp(14), MenuItemPadding());
  gr_color(0x26, 0x2a, 0x34, 255);
  gr_fill(pad, y + 4, std::max(pad + 1, ScreenWidth() - pad), y + 6);
  return 8;
}

void ScreenRecoveryUI::DrawHighlightBar(int x, int y, int width, int height) const {
  if (y >= ScreenHeight() || width <= 0) return;
  if (y + height > ScreenHeight())
    height = ScreenHeight() - y;
  if (height <= 0) return;

  const int radius = std::min(PixelsFromDp(20), height / 2);
  const int border = std::max(1, PixelsFromDp(1) / 2);

  // Fill rounded card using the color previously set by SetColor()
  gr_fill_rounded(x, y, x + width, y + height, radius);

  // Subtle card border + left accent indicator & right chevron '>'
  if (current_element_ == UIElement::MENU_SEL_BG) {
    if (fastbootd_logo_enabled_)
      gr_color(0xff, 0x8f, 0x00, 255);
    else
      gr_color(0xe8, 0x38, 0x4f, 255);
    gr_stroke_rounded(x, y, x + width, y + height, radius, border);
  } else if (current_element_ == UIElement::MENU_SEL_BG_ACTIVE) {
    gr_color(0x34, 0xd3, 0x99, 255);
    gr_stroke_rounded(x, y, x + width, y + height, radius, border);
  } else {
    gr_color(0x26, 0x2a, 0x34, 255);
    gr_stroke_rounded(x, y, x + width, y + height, radius, border);
  }

  // Right-aligned chevron '>' inside card
  if (gr_menu_font() != nullptr && width > MenuCharWidth() * 4) {
    if (current_element_ == UIElement::MENU_SEL_BG ||
        current_element_ == UIElement::MENU_SEL_BG_ACTIVE) {
      gr_color(0xff, 0xff, 0xff, 230);
    } else {
      gr_color(0x6e, 0x76, 0x85, 255);
    }
    const int chev_x = x + width - MenuItemPadding() - MenuCharWidth();
    const int chev_y = y + (height - MenuCharHeight()) / 2;
    gr_text(gr_menu_font(), chev_x, chev_y, ">", false);
  }
}

void ScreenRecoveryUI::DrawScrollBar(int y, int height) const {
  int x = ScreenWidth() - margin_width_ - PixelsFromDp(4);
  int width = std::max(4, PixelsFromDp(3));
  gr_fill_rounded(x - width, y, x, y + height, width / 2);
}

void ScreenRecoveryUI::DrawFill(int x, int y, int w, int h) const {
  gr_fill(x, y, w, h);
}

void ScreenRecoveryUI::DrawTextIcon(int x, int y, const GRSurface* surface) const {
  gr_texticon(x, y, surface);
}

int ScreenRecoveryUI::DrawTextLine(int x, int y, const std::string& line, bool bold) const {
  gr_text(gr_sys_font(), x, y, line.c_str(), bold);
  return char_height_ + 4;
}

int ScreenRecoveryUI::DrawTextLines(int x, int y, const std::vector<std::string>& lines) const {
  int offset = 0;
  for (const auto& line : lines) {
    offset += DrawTextLine(x, y + offset, line, false);
  }
  return offset;
}

int ScreenRecoveryUI::DrawWrappedTextLines(int x, int y,
                                           const std::vector<std::string>& lines) const {
  // Keep symmetrical margins based on the given offset (i.e. x).
  size_t text_cols = (ScreenWidth() - x * 2) / char_width_;
  int offset = 0;
  for (const auto& line : lines) {
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, text_cols + 1);
      if (sub.size() <= text_cols) {
        next_start += sub.size();
      } else {
        // Line too long and must be wrapped to text_cols columns.
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          // No space found, just draw as much as we can.
          sub.resize(text_cols);
          next_start += text_cols;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      offset += DrawTextLine(x, y + offset, sub, false);
    }
  }
  return offset;
}

void ScreenRecoveryUI::SetTitle(const std::vector<std::string>& lines) {
  title_lines_ = lines;
}

std::vector<std::string> ScreenRecoveryUI::GetMenuHelpMessage() const {
  // clang-format off
  static std::vector<std::string> REGULAR_HELP{
    "Use the volume up/down keys to navigate.",
    "Use the power key to select.",
  };
  static std::vector<std::string> LONG_PRESS_HELP{
    "Any button cycles highlight.",
    "Long-press activates.",
  };
  static const std::vector<std::string> NO_HELP = {};
  // clang-format on
  return HasTouchScreen() ? NO_HELP : HasThreeButtons() ? REGULAR_HELP : LONG_PRESS_HELP;
}

// Redraws everything on the screen. Does not flip pages. Should only be called with updateMutex
// locked.
void ScreenRecoveryUI::draw_screen_locked() {
  if (!show_text) {
    draw_background_locked();
    draw_foreground_locked();
    return;
  }

  gr_color(0x08, 0x09, 0x0C, 255);
  gr_clear();

  draw_menu_and_text_buffer_locked(GetMenuHelpMessage());
  draw_battery_capacity_locked();
}

void ScreenRecoveryUI::draw_top_status_bar_locked(int outer_pad, int top_y) const {
  const int pill_pad_x = PixelsFromDp(12);
  const int pill_pad_y = PixelsFromDp(6);
  const int pill_h = std::max(char_height_ + pill_pad_y * 2, PixelsFromDp(28));
  const int radius = pill_h / 2;
  const int border = std::max(1, PixelsFromDp(1) / 2);

  batt_pill_y_ = top_y;
  batt_pill_h_ = pill_h;
  back_btn_x_ = 0;
  back_btn_y_ = 0;
  back_btn_w_ = 0;
  back_btn_h_ = 0;

  if (menu_ && !menu_->IsMain()) {
    const std::string back_label = "< Back";
    const int label_w = gr_measure(gr_sys_font(), back_label.c_str());
    const int pill_w = std::max(PixelsFromDp(72), label_w + pill_pad_x * 2);
    back_btn_x_ = outer_pad;
    back_btn_y_ = top_y;
    back_btn_w_ = pill_w;
    back_btn_h_ = pill_h;

    const bool sel_back = (menu_->selection() == -1);
    if (sel_back) {
      gr_color(0xb3, 0x13, 0x29, 255);
    } else {
      gr_color(0x16, 0x18, 0x1e, 255);
    }
    gr_fill_rounded(back_btn_x_, back_btn_y_, back_btn_x_ + back_btn_w_,
                    back_btn_y_ + back_btn_h_, radius);
    if (sel_back) {
      gr_color(0xe8, 0x38, 0x4f, 255);
    } else {
      gr_color(0x26, 0x2a, 0x34, 255);
    }
    gr_stroke_rounded(back_btn_x_, back_btn_y_, back_btn_x_ + back_btn_w_,
                      back_btn_y_ + back_btn_h_, radius, border);
    gr_color(0xe8, 0xec, 0xf4, 255);
    gr_text(gr_sys_font(), back_btn_x_ + (pill_w - label_w) / 2,
            back_btn_y_ + (pill_h - char_height_) / 2, back_label.c_str(), false);
    return;
  }

  // Show Active Slot badge on the left when available
  std::string slot_text;
  for (const auto& line : title_lines_) {
    if (line.find("Active slot:") != std::string::npos ||
        line.find("Active Slot:") != std::string::npos) {
      slot_text = line;
      break;
    }
  }
  if (!slot_text.empty()) {
    const int text_w = gr_measure(gr_sys_font(), slot_text.c_str());
    const int pill_w = text_w + pill_pad_x * 2;
    gr_color(0x16, 0x18, 0x1e, 255);
    gr_fill_rounded(outer_pad, top_y, outer_pad + pill_w, top_y + pill_h, radius);
    gr_color(0x26, 0x2a, 0x34, 255);
    gr_stroke_rounded(outer_pad, top_y, outer_pad + pill_w, top_y + pill_h, radius, border);
    gr_color(0xd0, 0xd6, 0xe2, 255);
    gr_text(gr_sys_font(), outer_pad + pill_pad_x,
            top_y + (pill_h - char_height_) / 2, slot_text.c_str(), false);
  }
}

void ScreenRecoveryUI::draw_install_overlay_locked(
    int outer_pad, int y_start, int* out_next_y) const {
  const int card_w = std::max(100, ScreenWidth() - outer_pad * 2);
  const int card_radius = PixelsFromDp(22);
  const int border = std::max(1, PixelsFromDp(1) / 2);

  int y = y_start;
  auto& logo = fastbootd_logo_enabled_ ? fastbootd_logo_ : lineage_logo_;
  const int logo_w = logo ? static_cast<int>(gr_get_width(logo.get())) : 0;
  const int logo_h = logo ? static_cast<int>(gr_get_height(logo.get())) : 0;
  const int hero_pad_y = PixelsFromDp(10);
  const int hero_h = logo_h + hero_pad_y * 2;

  if (logo && hero_h > 0) {
    gr_color(0x00, 0x00, 0x00, 255);
    gr_fill_rounded(outer_pad, y, outer_pad + card_w, y + hero_h, card_radius);
    gr_color(0x26, 0x2a, 0x34, 255);
    gr_stroke_rounded(outer_pad, y, outer_pad + card_w, y + hero_h, card_radius, border);
    const int lx = ScreenWidth() / 2 - logo_w / 2;
    DrawSurface(logo.get(), 0, 0, logo_w, logo_h, lx, y + hero_pad_y);
    y += hero_h + PixelsFromDp(12);
  }

  // Animated progress / status card
  std::string status_label = "Processing...";
  switch (current_icon_) {
    case INSTALLING_UPDATE:
      status_label = "Installing update";
      break;
    case ERASING:
      status_label = "Erasing data";
      break;
    case ERROR:
      status_label = "Operation failed";
      break;
    case NO_COMMAND:
      status_label = "Ready";
      break;
    default:
      break;
  }

  std::string right_label;
  if (progressBarType == DETERMINATE) {
    int pct = static_cast<int>(
        std::clamp(progressScopeStart + progress * progressScopeSize, 0.0f, 1.0f) * 100.0f);
    right_label = std::to_string(pct) + "%";
  } else if (progressBarType == INDETERMINATE ||
             current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING) {
    int dots = static_cast<int>(install_anim_phase_ * 4.0f) % 4;
    right_label = std::string(dots, '.');
    if (right_label.empty()) right_label = ".";
  }

  const int inner_pad = PixelsFromDp(14);
  const int bar_h = PixelsFromDp(12);
  const int prog_card_h =
      inner_pad * 2 + menu_char_height_ + PixelsFromDp(10) + bar_h;
  gr_color(0x16, 0x18, 0x1e, 255);
  gr_fill_rounded(outer_pad, y, outer_pad + card_w, y + prog_card_h, PixelsFromDp(18));
  gr_color(0x26, 0x2a, 0x34, 255);
  gr_stroke_rounded(outer_pad, y, outer_pad + card_w, y + prog_card_h,
                    PixelsFromDp(18), border);

  const GRFont* label_font = gr_menu_font() ? gr_menu_font() : gr_sys_font();
  if (label_font) {
    gr_color(0xe8, 0xec, 0xf4, 255);
    gr_text(label_font, outer_pad + inner_pad, y + inner_pad, status_label.c_str(), true);
    if (!right_label.empty()) {
      const int rw = gr_measure(label_font, right_label.c_str());
      if (fastbootd_logo_enabled_)
        gr_color(0xff, 0x8f, 0x00, 255);
      else
        gr_color(0xe8, 0x38, 0x4f, 255);
      gr_text(label_font, outer_pad + card_w - inner_pad - rw, y + inner_pad,
              right_label.c_str(), true);
    }
  }

  const int bar_y = y + inner_pad + menu_char_height_ + PixelsFromDp(10);
  draw_modern_progress_bar_locked(outer_pad + inner_pad, bar_y,
                                  card_w - inner_pad * 2, bar_h);
  y += prog_card_h + PixelsFromDp(12);
  *out_next_y = y;
}

// Draws the menu and text buffer on the screen. Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_menu_and_text_buffer_locked(
    const std::vector<std::string>& help_message) {
  const int outer_pad = std::max(margin_width_ + PixelsFromDp(14), MenuItemPadding());
  const int card_w = std::max(100, ScreenWidth() - outer_pad * 2);
  const int border = std::max(1, PixelsFromDp(1) / 2);

  int y = margin_height_ + PixelsFromDp(10);
  draw_top_status_bar_locked(outer_pad, y);
  y += batt_pill_h_ + PixelsFromDp(10);

  if (menu_) {
    auto& logo = fastbootd_logo_enabled_ ? fastbootd_logo_ : lineage_logo_;
    const int logo_width = logo ? static_cast<int>(gr_get_width(logo.get())) : 0;
    const int logo_height = logo ? static_cast<int>(gr_get_height(logo.get())) : 0;

    // Filter out slot line (already shown in top pill) for clean hero metadata
    std::vector<std::string> hero_lines;
    for (const auto& line : title_lines_) {
      if (line.find("Active slot:") == std::string::npos &&
          line.find("Active Slot:") == std::string::npos) {
        hero_lines.push_back(line);
      }
    }

    const int hero_pad_y = PixelsFromDp(10);
    const int info_gap = (!hero_lines.empty() && logo_height > 0) ? PixelsFromDp(6) : 0;
    const int line_step = char_height_ + 6;
    const int hero_h = hero_pad_y * 2 + logo_height + info_gap +
                       static_cast<int>(hero_lines.size()) * line_step;
    const int hero_radius = PixelsFromDp(22);

    // Pure #000000 interior so existing RGB logo_image/fastbootd PNG blends seamlessly
    gr_color(0x00, 0x00, 0x00, 255);
    gr_fill_rounded(outer_pad, y, outer_pad + card_w, y + hero_h, hero_radius);
    gr_color(0x26, 0x2a, 0x34, 255);
    gr_stroke_rounded(outer_pad, y, outer_pad + card_w, y + hero_h, hero_radius, border);

    int inner_y = y + hero_pad_y;
    if (logo && logo_height > 0) {
      int centered_x = ScreenWidth() / 2 - logo_width / 2;
      DrawSurface(logo.get(), 0, 0, logo_width, logo_height, centered_x, inner_y);
      inner_y += logo_height + info_gap;
    }

    for (size_t idx = 0; idx < hero_lines.size(); ++idx) {
      const std::string& line = hero_lines[idx];
      if (idx == 0) {
        SetColor(UIElement::HEADER);
      } else {
        SetColor(UIElement::INFO);
      }
      int lw = gr_measure(gr_sys_font(), line.c_str());
      int lx = outer_pad + std::max(PixelsFromDp(12), (card_w - lw) / 2);
      gr_text(gr_sys_font(), lx, inner_y, line.c_str(), false);
      inner_y += line_step;
    }

    y += hero_h + PixelsFromDp(8);

    const int header_x = outer_pad + PixelsFromDp(6);
    y += menu_->DrawHeader(header_x, y);
    menu_start_y_ = y + 12;  // Keep aligned with TextMenu::DrawItems top spacing
    menu_->SetMenuHeight(std::max(0, ScreenHeight() - menu_start_y_ - PixelsFromDp(36)));
    y += menu_->DrawItems(header_x, y, ScreenWidth(), IsLongPress());
    if (!help_message.empty()) {
      y -= MenuItemSpacing();
      y += PixelsFromDp(6);
      SetColor(UIElement::INFO);
      y += DrawTextLines(header_x, y, help_message);
    }
  } else {
    draw_install_overlay_locked(outer_pad, y, &y);
  }

  // Bottom System Log Console Card when there is enough vertical room
  const int bottom_pad = margin_height_ + PixelsFromDp(12);
  const int avail_h = (ScreenHeight() - bottom_pad) - y;
  const int console_pad = PixelsFromDp(10);
  if (avail_h >= char_height_ * 2 + console_pad * 2) {
    const int console_y1 = y + PixelsFromDp(4);
    const int console_y2 = ScreenHeight() - bottom_pad;
    const int console_radius = PixelsFromDp(18);
    gr_color(0x0e, 0x10, 0x14, 255);
    gr_fill_rounded(outer_pad, console_y1, outer_pad + card_w, console_y2, console_radius);
    gr_color(0x1e, 0x22, 0x2b, 255);
    gr_stroke_rounded(outer_pad, console_y1, outer_pad + card_w, console_y2,
                      console_radius, border);

    const int max_chars = std::max(1, (card_w - console_pad * 2) / std::max(1, char_width_));
    SetColor(UIElement::LOG);
    int row = text_row_;
    size_t count = 0;
    const int min_ty = console_y1 + console_pad;
    for (int ty = console_y2 - console_pad - char_height_; ty >= min_ty && count < text_rows_;
         ty -= (char_height_ + 2), ++count) {
      std::string line(text_[row]);
      if (static_cast<int>(line.size()) > max_chars) {
        line.resize(max_chars);
      }
      if (!line.empty()) {
        gr_text(gr_sys_font(), outer_pad + console_pad, ty, line.c_str(), false);
      }
      --row;
      if (row < 0) row = text_rows_ - 1;
    }
  }
}

// Draws the battery capacity pill on the top-right corner of the screen.
void ScreenRecoveryUI::draw_battery_capacity_locked() {
  if (is_battery_less) return;

  const int outer_pad = std::max(margin_width_ + PixelsFromDp(14), MenuItemPadding());
  const int pill_pad_x = PixelsFromDp(10);
  const int pill_h = batt_pill_h_ > 0 ? batt_pill_h_ : (char_height_ + PixelsFromDp(12));
  const int pill_y = batt_pill_y_ > 0 ? batt_pill_y_ : (margin_height_ + PixelsFromDp(10));
  const int radius = pill_h / 2;
  const int border = std::max(1, PixelsFromDp(1) / 2);

  std::string batt_capacity = std::to_string(batt_capacity_) + '%';
  if (charging_) {
    batt_capacity.push_back('+');
  }

  const int text_w = gr_measure(gr_sys_font(), batt_capacity.c_str());
  const int icon_w = std::max(char_width_ * 4 / 5, PixelsFromDp(10));
  const int icon_h = std::max(char_height_ * 4 / 5, PixelsFromDp(14));
  const int gap = PixelsFromDp(6);
  const int pill_w = pill_pad_x * 2 + icon_w + gap + text_w;
  const int pill_x1 = ScreenWidth() - outer_pad - pill_w;
  const int pill_x2 = ScreenWidth() - outer_pad;

  gr_color(0x16, 0x18, 0x1e, 255);
  gr_fill_rounded(pill_x1, pill_y, pill_x2, pill_y + pill_h, radius);
  gr_color(0x26, 0x2a, 0x34, 255);
  gr_stroke_rounded(pill_x1, pill_y, pill_x2, pill_y + pill_h, radius, border);

  // Mini battery icon inside pill
  const int bx = pill_x1 + pill_pad_x;
  const int by = pill_y + (pill_h - icon_h) / 2;
  const int nub_h = std::max(2, icon_h / 8);
  const int nub_w = std::max(2, icon_w / 3);
  if (batt_capacity_ <= 15) {
    gr_color(0xf8, 0x71, 0x71, 255);
  } else if (batt_capacity_ <= 30) {
    gr_color(0xfb, 0xbf, 0x24, 255);
  } else {
    gr_color(0x4a, 0xde, 0x80, 255);
  }
  gr_fill(bx + (icon_w - nub_w) / 2, by, bx + (icon_w + nub_w) / 2, by + nub_h);
  gr_fill_rounded(bx, by + nub_h, bx + icon_w, by + icon_h, std::max(2, icon_w / 4));

  // Unfilled top portion inside battery body
  const int body_h = icon_h - nub_h;
  const int inner_pad = std::max(1, icon_w / 6);
  const int inner_h = std::max(1, body_h - inner_pad * 2);
  const int cap_h = inner_h * std::clamp(batt_capacity_, 0, 100) / 100;
  if (cap_h < inner_h) {
    gr_color(0x16, 0x18, 0x1e, 255);
    gr_fill(bx + inner_pad, by + nub_h + inner_pad, bx + icon_w - inner_pad,
            by + nub_h + inner_pad + (inner_h - cap_h));
  }

  gr_color(0xe8, 0xec, 0xf4, 255);
  gr_text(gr_sys_font(), bx + icon_w + gap, pill_y + (pill_h - char_height_) / 2,
          batt_capacity.c_str(), false);
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::update_screen_locked() {
  draw_screen_locked();
  gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::update_progress_locked() {
  if (show_text || !pagesIdentical) {
    draw_screen_locked();  // Must redraw the whole screen
    pagesIdentical = true;
  } else {
    draw_foreground_locked();  // Draw only the progress bar and overlays
  }
  gr_flip();
}

void ScreenRecoveryUI::BattMonitorThreadLoop() {
  using aidl::android::hardware::health::BatteryStatus;
  using android::hardware::health::InitHealthdConfig;

  auto config = std::make_unique<healthd_config>();
  InitHealthdConfig(config.get());

  auto batt_monitor = std::make_unique<android::BatteryMonitor>();
  batt_monitor->init(config.get());

  bool is_first_call = true;

  while (!batt_monitor_thread_stopped_) {
    bool redraw = false;
    {
      std::lock_guard<std::mutex> lg(updateMutex);

      auto charge_status = static_cast<BatteryStatus>(batt_monitor->getChargeStatus());
      // Treat unknown status as on charger.
      bool charging = (charge_status != BatteryStatus::DISCHARGING &&
                       charge_status != BatteryStatus::NOT_CHARGING &&
                       charge_status != BatteryStatus::FULL);
      if (charging_ != charging) {
        charging_ = charging;
        redraw = true;
      }

      android::BatteryProperty prop;
      android::base::Timer t;
      android::status_t status;
      while (t.duration() < 5s) {
        status = batt_monitor->getProperty(android::BATTERY_PROP_CAPACITY, &prop);
        if (status == android::OK || !is_first_call) {
          break;
        }

        LOG(WARNING) << "Trying again for reinitializing battery info";
        if (redraw) update_screen_locked();
        batt_monitor->init(config.get());
        std::this_thread::sleep_for(100ms);
      }
      is_first_call = false;

      // If we can't read battery percentage, it may be a device without battery. In this
      // situation, use 100 as a fake battery percentage.
      if (status != android::OK) {
        prop.valueInt64 = 100;
      }

      int32_t batt_capacity = static_cast<int32_t>(prop.valueInt64);
      if (batt_capacity_ != batt_capacity) {
        batt_capacity_ = batt_capacity;
        redraw = true;
      }

      if (redraw) update_screen_locked();
    }
    std::this_thread::sleep_for(5s);
  }
}

void ScreenRecoveryUI::ProgressThreadLoop() {
  double interval = 1.0 / animation_fps_;
  while (!progress_thread_stopped_) {
    double start = now();
    bool redraw = false;
    {
      std::lock_guard<std::mutex> lg(updateMutex);

      // Advance smooth 30 FPS progress card sweep/pulse phase whenever a task or bar is active.
      if (progressBarType != EMPTY || current_icon_ == INSTALLING_UPDATE ||
          current_icon_ == ERASING) {
        install_anim_phase_ += 0.03f;
        if (install_anim_phase_ >= 1.0f) {
          install_anim_phase_ -= 1.0f;
        }
        if (menu_ == nullptr || progressBarType == INDETERMINATE) {
          redraw = true;
        }
      }

      // update the installation animation, if active
      // skip this if we have a text overlay (too expensive to update)
      if ((current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING) && !show_text) {
        if (!intro_done_) {
          if (current_frame_ == intro_frames_.size() - 1) {
            intro_done_ = true;
            current_frame_ = 0;
          } else {
            ++current_frame_;
          }
        } else {
          current_frame_ = (current_frame_ + 1) % loop_frames_.size();
        }

        redraw = true;
      }

      // move the progress bar forward on timed intervals, if configured
      int duration = progressScopeDuration;
      if (progressBarType == DETERMINATE && duration > 0) {
        double elapsed = now() - progressScopeTime;
        float p = 1.0 * elapsed / duration;
        if (p > 1.0) p = 1.0;
        if (p > progress) {
          progress = p;
          redraw = true;
        }
      }

      if (redraw) update_progress_locked();
    }

    double end = now();
    // minimum of 20ms delay between frames
    double delay = interval - (end - start);
    if (delay < 0.02) delay = 0.02;
    usleep(static_cast<useconds_t>(delay * 1000000));
  }
}

std::unique_ptr<GRSurface> ScreenRecoveryUI::LoadBitmap(const std::string& filename) {
  GRSurface* surface;
  if (auto result = res_create_display_surface(filename.c_str(), &surface); result < 0) {
    LOG(ERROR) << "Failed to load bitmap " << filename << " (error " << result << ")";
    return nullptr;
  }
  return std::unique_ptr<GRSurface>(surface);
}

std::unique_ptr<GRSurface> ScreenRecoveryUI::LoadLocalizedBitmap(const std::string& filename) {
  GRSurface* surface;
  auto result = res_create_localized_alpha_surface(filename.c_str(), locale_.c_str(), &surface);
  if (result == 0) {
    return std::unique_ptr<GRSurface>(surface);
  }

  result = res_create_localized_alpha_surface(filename.c_str(), DEFAULT_LOCALE, &surface);
  if (result == 0) {
    return std::unique_ptr<GRSurface>(surface);
  }

  return nullptr;
}

static char** Alloc2d(size_t rows, size_t cols) {
  char** result = new char*[rows];
  for (size_t i = 0; i < rows; ++i) {
    result[i] = new char[cols];
    memset(result[i], 0, cols);
  }
  return result;
}

// Choose the right background string to display during update.
void ScreenRecoveryUI::SetSystemUpdateText(bool security_update) {
  if (security_update) {
    installing_text_ = LoadLocalizedBitmap("installing_security_text");
  } else {
    installing_text_ = LoadLocalizedBitmap("installing_text");
  }
  Redraw();
}

bool ScreenRecoveryUI::InitTextParams() {
  // gr_init() would return successfully on font initialization failure.
  if (gr_sys_font() == nullptr) {
    return false;
  }
  gr_font_size(gr_sys_font(), &char_width_, &char_height_);
  gr_font_size(gr_menu_font(), &menu_char_width_, &menu_char_height_);
  text_rows_ = (ScreenHeight() - margin_height_ * 2) / char_height_;
  text_cols_ = (ScreenWidth() - margin_width_ * 2) / char_width_;
  return true;
}

bool ScreenRecoveryUI::LoadWipeDataMenuText() {
  // Ignores the errors since the member variables will stay as nullptr.
  cancel_wipe_data_text_ = LoadLocalizedBitmap("cancel_wipe_data_text");
  factory_data_reset_text_ = LoadLocalizedBitmap("factory_data_reset_text");
  try_again_text_ = LoadLocalizedBitmap("try_again_text");
  wipe_data_confirmation_text_ = LoadLocalizedBitmap("wipe_data_confirmation_text");
  wipe_data_menu_header_text_ = LoadLocalizedBitmap("wipe_data_menu_header_text");
  return true;
}

static bool InitGraphics() {
  // Default timeout is 5 seconds, same as init wait for file
  // 10ms increments
  const unsigned timeout =
      android::base::GetIntProperty("ro.recovery.ui.graphics_timeout_ms", 5000) / 10;
  for (auto retry = timeout; retry > 0; --retry) {
    if (gr_init() == 0) {
      if (retry < timeout) {
        // Log message like init wait for file completion log for consistency.
        LOG(WARNING) << "wait for 'graphics' took " << ((timeout - retry) * 10) << "ms";
      }
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  // Log message like init wait for file timeout log for consistency.
  LOG(ERROR) << "timeout wait for 'graphics' took " << (timeout * 10) << "ms";
  return false;
}

bool ScreenRecoveryUI::Init(const std::string& locale) {
  RecoveryUI::Init(locale);

  if (!InitGraphics()) {
    return false;
  }
  is_graphics_available = true;

  if (!InitTextParams()) {
    return false;
  }
  menu_draw_funcs_ = std::make_unique<MenuDrawFunctions>(*this);

  if (blank_unblank_on_init_) {
    gr_fb_blank(true);
    gr_fb_blank(false);
  }

  // Are we portrait or landscape?
  layout_ = (gr_fb_width() > gr_fb_height()) ? LANDSCAPE : PORTRAIT;
  // Are we the large variant of our base layout?
  if (gr_fb_height() > PixelsFromDp(800)) ++layout_;

  text_ = Alloc2d(text_rows_, text_cols_ + 1);
  file_viewer_text_ = Alloc2d(text_rows_, text_cols_ + 1);

  text_col_ = text_row_ = 0;

  // Set up the locale info.
  SetLocale(locale);

  error_icon_ = LoadBitmap("icon_error");

  progress_bar_empty_ = LoadBitmap("progress_empty");
  progress_bar_fill_ = LoadBitmap("progress_fill");
  stage_marker_empty_ = LoadBitmap("stage_empty");
  stage_marker_fill_ = LoadBitmap("stage_fill");

  erasing_text_ = LoadLocalizedBitmap("erasing_text");
  no_command_text_ = LoadLocalizedBitmap("no_command_text");
  error_text_ = LoadLocalizedBitmap("error_text");

  back_icon_ = LoadBitmap("ic_back");
  back_icon_sel_ = LoadBitmap("ic_back_sel");
  if (android::base::GetBoolProperty("ro.boot.dynamic_partitions", false) ||
      android::base::GetBoolProperty("ro.fastbootd.available", true)) {
    lineage_logo_ = LoadBitmap("logo_image_switch");
    fastbootd_logo_ = LoadBitmap("fastbootd");
  } else {
    lineage_logo_ = LoadBitmap("logo_image");
  }

  // Background text for "installing_update" could be "installing update" or
  // "installing security update". It will be set after Init() according to the commands in BCB.
  installing_text_.reset();

  LoadWipeDataMenuText();

  LoadAnimation();

  is_battery_less = android::base::GetBoolProperty("ro.recovery.batteryless", false);
  if (!is_battery_less)
    // Keep the battery capacity updated.
    batt_monitor_thread_ = std::thread(&ScreenRecoveryUI::BattMonitorThreadLoop, this);

  // Keep the progress bar updated, even when the process is otherwise busy.
  progress_thread_ = std::thread(&ScreenRecoveryUI::ProgressThreadLoop, this);

  // set the callback for hall sensor event
  (void)ev_sync_sw_state([this](auto&& a, auto&& b) { return this->SetSwCallback(a, b);});

  return true;
}

std::string ScreenRecoveryUI::GetLocale() const {
  return locale_;
}

void ScreenRecoveryUI::LoadAnimation() {
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(Paths::Get().resource_dir().c_str()),
                                                closedir);
  dirent* de;
  std::vector<std::string> intro_frame_names;
  std::vector<std::string> loop_frame_names;

  while ((de = readdir(dir.get())) != nullptr) {
    int value, num_chars;
    if (sscanf(de->d_name, "intro%d%n.png", &value, &num_chars) == 1) {
      intro_frame_names.emplace_back(de->d_name, num_chars);
    } else if (sscanf(de->d_name, "loop%d%n.png", &value, &num_chars) == 1) {
      loop_frame_names.emplace_back(de->d_name, num_chars);
    }
  }

  size_t intro_frames = intro_frame_names.size();
  size_t loop_frames = loop_frame_names.size();

  // It's okay to not have an intro.
  if (intro_frames == 0) intro_done_ = true;
  // But you must have an animation.
  if (loop_frames == 0) abort();

  std::sort(intro_frame_names.begin(), intro_frame_names.end());
  std::sort(loop_frame_names.begin(), loop_frame_names.end());

  intro_frames_.clear();
  intro_frames_.reserve(intro_frames);
  for (const auto& frame_name : intro_frame_names) {
    intro_frames_.emplace_back(LoadBitmap(frame_name));
  }

  loop_frames_.clear();
  loop_frames_.reserve(loop_frames);
  for (const auto& frame_name : loop_frame_names) {
    loop_frames_.emplace_back(LoadBitmap(frame_name));
  }
}

void ScreenRecoveryUI::SetBackground(Icon icon) {
  std::lock_guard<std::mutex> lg(updateMutex);

  current_icon_ = icon;
  update_screen_locked();
}

void ScreenRecoveryUI::SetProgressType(ProgressType type) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (progressBarType != type) {
    progressBarType = type;
  }
  progressScopeStart = 0;
  progressScopeSize = 0;
  progress = 0;
  update_progress_locked();
}

void ScreenRecoveryUI::ShowProgress(float portion, float seconds) {
  std::lock_guard<std::mutex> lg(updateMutex);
  progressBarType = DETERMINATE;
  progressScopeStart += progressScopeSize;
  progressScopeSize = portion;
  progressScopeTime = now();
  progressScopeDuration = seconds;
  progress = 0;
  update_progress_locked();
}

void ScreenRecoveryUI::SetProgress(float fraction) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  if (progressBarType == DETERMINATE && fraction > progress) {
    // Skip updates that aren't visibly different.
    int width = gr_get_width(progress_bar_empty_.get());
    float scale = width * progressScopeSize;
    if ((int)(progress * scale) != (int)(fraction * scale)) {
      progress = fraction;
      update_progress_locked();
    }
  }
}

void ScreenRecoveryUI::SetStage(int current, int max) {
  std::lock_guard<std::mutex> lg(updateMutex);
  stage = current;
  max_stage = max;
}

void ScreenRecoveryUI::PrintV(const char* fmt, bool copy_to_stdout, va_list ap) {
  std::string str;
  android::base::StringAppendV(&str, fmt, ap);

  if (copy_to_stdout) {
    fputs(str.c_str(), stdout);
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  if (text_rows_ > 0 && text_cols_ > 0) {
    for (const char* ptr = str.c_str(); *ptr != '\0'; ++ptr) {
      if (*ptr == '\n' || text_col_ >= text_cols_) {
        text_[text_row_][text_col_] = '\0';
        text_col_ = 0;
        text_row_ = (text_row_ + 1) % text_rows_;
      }
      if (*ptr != '\n') text_[text_row_][text_col_++] = *ptr;
    }
    text_[text_row_][text_col_] = '\0';
    update_screen_locked();
  }
}

void ScreenRecoveryUI::Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, true, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PrintOnScreenOnly(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, false, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PutChar(char ch) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (ch != '\n') text_[text_row_][text_col_++] = ch;
  if (ch == '\n' || text_col_ >= text_cols_) {
    text_col_ = 0;
    ++text_row_;
  }
}

void ScreenRecoveryUI::ClearText() {
  std::lock_guard<std::mutex> lg(updateMutex);
  text_col_ = 0;
  text_row_ = 0;
  for (size_t i = 0; i < text_rows_; ++i) {
    memset(text_[i], 0, text_cols_ + 1);
  }
}

void ScreenRecoveryUI::ShowFile(FILE* fp) {
  std::vector<off_t> offsets;
  offsets.push_back(ftello(fp));
  ClearText();

  struct stat sb;
  fstat(fileno(fp), &sb);

  bool show_prompt = false;
  while (true) {
    if (show_prompt) {
      PrintOnScreenOnly("--(%d%% of %d bytes)--",
                        static_cast<int>(100 * (double(ftello(fp)) / double(sb.st_size))),
                        static_cast<int>(sb.st_size));
      Redraw();
      while (show_prompt) {
        show_prompt = false;
        InputEvent evt = WaitInputEvent();
        if (evt.type() == EventType::EXTRA) {
          if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) {
            return;
          }
        }
        if (evt.type() != EventType::KEY) {
          show_prompt = true;
          continue;
        }
        if (evt.key() == KEY_POWER || evt.key() == KEY_ENTER || evt.key() == KEY_BACKSPACE ||
            evt.key() == KEY_BACK || evt.key() == KEY_HOMEPAGE ||
            evt.key() == KEY_ESC || evt.key() == KEY_LEFTMETA || evt.key() == KEY_RIGHTMETA) {
          return;
        } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP || evt.key() == KEY_SCROLLUP ||
                   evt.key() == KEY_PAGEUP) {
          if (offsets.size() <= 1) {
            show_prompt = true;
          } else {
            offsets.pop_back();
            fseek(fp, offsets.back(), SEEK_SET);
          }
        } else {
          if (feof(fp)) {
            return;
          }
          offsets.push_back(ftello(fp));
        }
      }
      ClearText();
    }

    int ch = getc(fp);
    if (ch == EOF) {
      while (text_row_ < text_rows_ - 1) PutChar('\n');
      show_prompt = true;
    } else {
      PutChar(ch);
      if (text_col_ == 0 && text_row_ >= text_rows_ - 1) {
        show_prompt = true;
      }
    }
  }
}

void ScreenRecoveryUI::ShowFile(const std::string& filename) {
  std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(filename.c_str(), "re"), fclose);
  if (!fp) {
    Print("  Unable to open %s: %s\n", filename.c_str(), strerror(errno));
    return;
  }

  char** old_text = text_;
  size_t old_text_col = text_col_;
  size_t old_text_row = text_row_;

  // Swap in the alternate screen and clear it.
  text_ = file_viewer_text_;
  ClearText();

  ShowFile(fp.get());

  text_ = old_text;
  text_col_ = old_text_col;
  text_row_ = old_text_row;
}

std::unique_ptr<Menu> ScreenRecoveryUI::CreateMenu(
    const GRSurface* graphic_header, const std::vector<const GRSurface*>& graphic_items,
    const std::vector<std::string>& text_headers, const std::vector<std::string>& text_items,
    size_t initial_selection) const {
  // horizontal unusable area: margin width + menu indent
  size_t max_width = ScreenWidth() - margin_width_ - kMenuIndent;
  // vertical unusable area: margin height + title lines + helper message + high light bar.
  // It is safe to reserve more space.
  size_t max_height = ScreenHeight() - margin_height_ - char_height_ * (title_lines_.size() + 3);
  if (GraphicMenu::Validate(max_width, max_height, graphic_header, graphic_items)) {
    return std::make_unique<GraphicMenu>(graphic_header, graphic_items, initial_selection, *this);
  }

  fprintf(stderr, "Failed to initialize graphic menu, falling back to use the text menu.\n");

  return CreateMenu(text_headers, text_items, initial_selection);
}

std::unique_ptr<Menu> ScreenRecoveryUI::CreateMenu(const std::vector<std::string>& text_headers,
                                                   const std::vector<std::string>& text_items,
                                                   size_t initial_selection) const {
  int menu_char_width = MenuCharWidth();
  int menu_char_height = MenuCharHeight();
  int avail_text_w =
      std::max(menu_char_width,
               ScreenWidth() - margin_width_ * 2 - MenuItemPadding() * 4 - menu_char_width * 2);
  int menu_cols = avail_text_w / menu_char_width;
  bool wrap_selection = !HasThreeButtons() && !HasTouchScreen();
  return std::make_unique<TextMenu>(wrap_selection, menu_cols, text_headers, text_items,
                                    initial_selection, menu_char_height, *menu_draw_funcs_);
}

int ScreenRecoveryUI::SelectMenu(int sel) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (menu_) {
    int old_sel = menu_->selection();
    sel = menu_->Select(sel);

    if (sel != old_sel) {
      update_screen_locked();
    }
  }
  return sel;
}

int ScreenRecoveryUI::SelectMenu(const Point& p) {
  Point point;

  const auto scaleX = static_cast<double>(p.x()) / ScreenWidth();
  const auto scaleY = static_cast<double>(p.y()) / ScreenHeight();

  // Correct position for touch rotation
  switch (gr_touch_rotation()) {
    case GRRotation::NONE:
      point.x(ScreenWidth() * scaleX);
      point.y(ScreenHeight() * scaleY);
      break;
    case GRRotation::RIGHT:
      point.x(ScreenWidth() * scaleY);
      point.y(ScreenHeight() - (ScreenHeight() * scaleX));
      break;
    case GRRotation::DOWN:
      point.x(ScreenWidth() - (ScreenWidth() * scaleX));
      point.y(ScreenHeight() - (ScreenHeight() * scaleY));
      break;
    case GRRotation::LEFT:
      point.x(ScreenWidth() - (ScreenWidth() * scaleY));
      point.y(ScreenHeight() * scaleX);
      break;
  }

  // Correct position for overscan
  point.x(point.x() - gr_overscan_offset_x());
  point.y(point.y() - gr_overscan_offset_y());

  int new_sel = Device::kNoAction;
  std::lock_guard<std::mutex> lg(updateMutex);
  if (menu_) {
    if (!menu_->IsMain()) {
      // Back pill hitbox computed by draw_top_status_bar_locked().
      if (back_btn_w_ > 0 && back_btn_h_ > 0 && point.x() >= back_btn_x_ &&
          point.x() <= back_btn_x_ + back_btn_w_ && point.y() >= back_btn_y_ &&
          point.y() <= back_btn_y_ + back_btn_h_) {
        return Device::kGoBack;
      }
    }

    const int menu_item_height = MenuItemHeight();
    const int menu_item_height_with_spacing = menu_item_height + MenuItemSpacing();

    if (point.y() >= menu_start_y_ &&
        point.y() < menu_start_y_ + menu_->ItemsCount() * menu_item_height_with_spacing) {
      int old_sel = menu_->selection();
      int relative_sel = (point.y() - menu_start_y_) / menu_item_height_with_spacing;
      int menu_item_start_y = menu_start_y_ + (relative_sel * menu_item_height_with_spacing);
      if (point.y() > menu_item_start_y + menu_item_height) {
        // The touch is in the spacing area between two menu items.
        return Device::kNoAction;
      }
      new_sel = menu_->SelectVisible(relative_sel);
      if (new_sel != -1 && new_sel != old_sel) {
        update_screen_locked();
      }
    }
  }
  return new_sel;
}

int ScreenRecoveryUI::ScrollMenu(int updown) {
  std::lock_guard<std::mutex> lg(updateMutex);
  int sel = Device::kNoAction;
  if (menu_) {
    sel = menu_->Scroll(updown);
    update_screen_locked();
  }
  return sel;
}

size_t ScreenRecoveryUI::ShowMenu(std::unique_ptr<Menu>&& menu, bool menu_only,
                                  const std::function<int(int, bool)>& key_handler,
                                  bool refreshable) {
  // Throw away keys pressed previously, so user doesn't accidentally trigger menu items.
  FlushKeys();

  // If there is a key interrupt in progress, return KeyError::INTERRUPTED without starting the
  // menu.
  if (IsKeyInterrupted()) return static_cast<size_t>(KeyError::INTERRUPTED);

  CHECK(menu != nullptr);

  // Starts and displays the menu
  menu_ = std::move(menu);
  Redraw();

  int selected = menu_->selection();
  int chosen_item = -1;
  while (chosen_item < 0) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) {
        // WaitKey() was interrupted.
        return static_cast<size_t>(KeyError::INTERRUPTED);
      }
      if (evt.key() == static_cast<int>(KeyError::TIMED_OUT)) {  // WaitKey() timed out.
        if (WasTextEverVisible()) {
          continue;
        } else {
          LOG(INFO) << "Timed out waiting for key input; rebooting.";
          menu_.reset();
          Redraw();
          return static_cast<size_t>(KeyError::TIMED_OUT);
        }
      }
    }

    int action = Device::kNoAction;
    if (evt.type() == EventType::TOUCH) {
      int touch_sel = SelectMenu(evt.pos());
      if (touch_sel < 0) {
        action = touch_sel;
      } else {
        action = Device::kInvokeItem;
        selected = touch_sel;
      }
    } else {
      bool visible = IsTextVisible();
      action = key_handler(evt.key(), visible);
    }

    if (action < 0) {
      switch (action) {
        case Device::kHighlightUp:
          selected = SelectMenu(--selected);
          break;
        case Device::kHighlightDown:
          selected = SelectMenu(++selected);
          break;
        case Device::kHighlightFirst:
          selected = SelectMenu(0);
          break;
        case Device::kHighlightLast:
          selected = SelectMenu(menu_->ItemsCount() - 1);
          break;
        case Device::kScrollUp:
          selected = ScrollMenu(-1);
          break;
        case Device::kScrollDown:
          selected = ScrollMenu(1);
          break;
        case Device::kInvokeItem:
          if (selected < 0) {
            chosen_item = Device::kGoBack;
          } else {
            chosen_item = selected;
          }
          break;
        case Device::kNoAction:
          break;
        case Device::kGoBack:
          chosen_item = Device::kGoBack;
          break;
        case Device::kGoHome:
          chosen_item = Device::kGoHome;
          break;
        case Device::kDoSideload:
          chosen_item = Device::kDoSideload;
          break;
        case Device::kRefresh:
          if (refreshable) {
            chosen_item = Device::kRefresh;
          }
          break;
      }
    } else if (!menu_only) {
      chosen_item = action;
    }

    if (chosen_item == Device::kGoBack || chosen_item == Device::kGoHome ||
        chosen_item == Device::kDoSideload || chosen_item == Device::kRefresh) {
      break;
    }
  }

  menu_.reset();

  return chosen_item;
}

size_t ScreenRecoveryUI::ShowMenu(const std::vector<std::string>& headers,
                                  const std::vector<std::string>& items, size_t initial_selection,
                                  bool menu_only,
                                  const std::function<int(int, bool)>& key_handler,
                                  bool refreshable) {
  auto menu = CreateMenu(headers, items, initial_selection);
  if (menu == nullptr) {
    return initial_selection;
  }

  return ShowMenu(std::move(menu), menu_only, key_handler, refreshable);
}

size_t ScreenRecoveryUI::ShowPromptWipeDataMenu(const std::vector<std::string>& backup_headers,
                                                const std::vector<std::string>& backup_items,
                                                const std::function<int(int, bool)>& key_handler) {
  auto wipe_data_menu = CreateMenu(backup_headers, backup_items, 0);
  if (wipe_data_menu == nullptr) {
    return 0;
  }

  return ShowMenu(std::move(wipe_data_menu), true, key_handler);
}

size_t ScreenRecoveryUI::ShowPromptWipeDataConfirmationMenu(
    const std::vector<std::string>& backup_headers, const std::vector<std::string>& backup_items,
    const std::function<int(int, bool)>& key_handler) {
  auto confirmation_menu =
      CreateMenu(wipe_data_confirmation_text_.get(),
                 { cancel_wipe_data_text_.get(), factory_data_reset_text_.get() }, backup_headers,
                 backup_items, 0);
  if (confirmation_menu == nullptr) {
    return 0;
  }

  return ShowMenu(std::move(confirmation_menu), true, key_handler);
}

bool ScreenRecoveryUI::IsTextVisible() {
  std::lock_guard<std::mutex> lg(updateMutex);
  int visible = show_text;
  return visible;
}

bool ScreenRecoveryUI::WasTextEverVisible() {
  std::lock_guard<std::mutex> lg(updateMutex);
  int ever_visible = show_text_ever;
  return ever_visible;
}

void ScreenRecoveryUI::ShowText(bool visible) {
  std::lock_guard<std::mutex> lg(updateMutex);
  show_text = visible;
  if (show_text) show_text_ever = true;
  update_screen_locked();
}

void ScreenRecoveryUI::Redraw() {
  std::lock_guard<std::mutex> lg(updateMutex);
  update_screen_locked();
}

void ScreenRecoveryUI::KeyLongPress(int) {
  // Redraw so that if we're in the menu, the highlight
  // will change color to indicate a successful long press.
  Redraw();
}

void ScreenRecoveryUI::SetLocale(const std::string& new_locale) {
  locale_ = new_locale;
  rtl_locale_ = false;

  if (!new_locale.empty()) {
    size_t separator = new_locale.find('-');
    // lang has the language prefix prior to the separator, or full string if none exists.
    std::string lang = new_locale.substr(0, separator);

    // A bit cheesy: keep an explicit list of supported RTL languages.
    if (lang == "ar" ||  // Arabic
        lang == "fa" ||  // Persian (Farsi)
        lang == "he" ||  // Hebrew (new language code)
        lang == "iw" ||  // Hebrew (old language code)
        lang == "ur") {  // Urdu
      rtl_locale_ = true;
    }
  }
}

int ScreenRecoveryUI::SetSwCallback(int code, int value) {
  if (!is_graphics_available) { return -1; }
  if (code > SW_MAX) { return -1; }
  if (code != SW_LID) { return 0; }

  /* detect dual display */
  if (!gr_has_multiple_connectors()) { return -1; }

  /* turn off all screen */
  gr_fb_blank(true, DirectRenderManager::DRM_INNER);
  gr_fb_blank(true, DirectRenderManager::DRM_OUTER);
  gr_color(0, 0, 0, 255);
  gr_clear();

  /* turn on the screen */
  gr_fb_blank(false, value);
  gr_flip();

  /* set the retation */
  std::string rotation_str;
  if (value == DirectRenderManager::DRM_OUTER) {
    rotation_str =
      android::base::GetProperty("ro.minui.second_rotation", "ROTATION_NONE");
  } else {
    rotation_str =
      android::base::GetProperty("ro.minui.default_rotation", "ROTATION_NONE");
  }

  if (rotation_str == "ROTATION_RIGHT") {
    gr_rotate(GRRotation::RIGHT);
  } else if (rotation_str == "ROTATION_DOWN") {
    gr_rotate(GRRotation::DOWN);
  } else if (rotation_str == "ROTATION_LEFT") {
    gr_rotate(GRRotation::LEFT);
  } else {  // "ROTATION_NONE" or unknown string
    gr_rotate(GRRotation::NONE);
  }
  Redraw();

  return 0;
}
