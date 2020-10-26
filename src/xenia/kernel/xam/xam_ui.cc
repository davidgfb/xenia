/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/imgui/imgui.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/window.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

std::atomic<int> xam_dialogs_shown_ = {0};

dword_result_t XamIsUIActive() { return xam_dialogs_shown_ > 0 ? 1 : 0; }
DECLARE_XAM_EXPORT2(XamIsUIActive, kUI, kImplemented, kHighFrequency);

class MessageBoxDialog : public xe::ui::ImGuiDialog {
 public:
  MessageBoxDialog(xe::ui::Window* window, std::u16string title,
                   std::u16string description,
                   std::vector<std::u16string> buttons, uint32_t default_button,
                   uint32_t* out_chosen_button)
      : ImGuiDialog(window),
        title_(xe::to_utf8(title)),
        description_(xe::to_utf8(description)),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        out_chosen_button_(out_chosen_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
    if (out_chosen_button) {
      *out_chosen_button = default_button;
    }
  }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::Text("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      for (size_t i = 0; i < buttons_.size(); ++i) {
        auto button_name = xe::to_utf8(buttons_[i]);
        if (ImGui::Button(button_name.c_str())) {
          if (out_chosen_button_) {
            *out_chosen_button_ = static_cast<uint32_t>(i);
          }
          ImGui::CloseCurrentPopup();
          Close();
        }
        ImGui::SameLine();
      }
      ImGui::Spacing();
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::u16string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t* out_chosen_button_ = nullptr;
};

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
dword_result_t XamShowMessageBoxUI(dword_t user_index, lpu16string_t title_ptr,
                                   lpu16string_t text_ptr, dword_t button_count,
                                   lpdword_t button_ptrs, dword_t active_button,
                                   dword_t flags, lpdword_t result_ptr,
                                   pointer_t<XAM_OVERLAPPED> overlapped) {
  std::u16string title;
  if (title_ptr) {
    title = title_ptr.value();
  } else {
    title = u"";  // TODO(gibbed): default title based on flags?
  }
  auto text = text_ptr.value();

  std::vector<std::u16string> buttons;
  std::u16string all_buttons;
  for (uint32_t j = 0; j < button_count; ++j) {
    uint32_t button_ptr = button_ptrs[j];
    auto button = xe::load_and_swap<std::u16string>(
        kernel_state()->memory()->TranslateVirtual(button_ptr));
    all_buttons.append(button);
    if (j + 1 < button_count) {
      all_buttons.append(u" | ");
    }
    buttons.push_back(button);
  }

  // Broadcast XN_SYS_UI = true
  kernel_state()->BroadcastNotification(0x9, true);

  uint32_t chosen_button;
  if (cvars::headless) {
    // Auto-pick the focused button.
    chosen_button = active_button;
  } else {
    auto display_window = kernel_state()->emulator()->display_window();
    xe::threading::Fence fence;
    display_window->loop()->PostSynchronous([&]() {
      // TODO(benvanik): setup icon states.
      switch (flags & 0xF) {
        case 0:
          // config.pszMainIcon = nullptr;
          break;
        case 1:
          // config.pszMainIcon = TD_ERROR_ICON;
          break;
        case 2:
          // config.pszMainIcon = TD_WARNING_ICON;
          break;
        case 3:
          // config.pszMainIcon = TD_INFORMATION_ICON;
          break;
      }
      (new MessageBoxDialog(display_window, title, text, buttons, active_button,
                            &chosen_button))
          ->Then(&fence);
    });
    ++xam_dialogs_shown_;
    fence.Wait();
    --xam_dialogs_shown_;
  }
  *result_ptr = chosen_button;

  // Broadcast XN_SYS_UI = false
  kernel_state()->BroadcastNotification(0x9, false);

  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUI, kUI, kImplemented);

class KeyboardInputDialog : public xe::ui::ImGuiDialog {
 public:
  KeyboardInputDialog(xe::ui::Window* window, std::u16string title,
                      std::u16string description, std::u16string default_text,
                      std::u16string* out_text, size_t max_length)
      : ImGuiDialog(window),
        title_(xe::to_utf8(title)),
        description_(xe::to_utf8(description)),
        default_text_(xe::to_utf8(default_text)),
        out_text_(out_text),
        max_length_(max_length) {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    if (out_text_) {
      *out_text_ = default_text;
    }
    text_buffer_.resize(max_length);
    std::strncpy(text_buffer_.data(), default_text_.c_str(),
                 std::min(text_buffer_.size() - 1, default_text_.size()));
  }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::TextWrapped("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      if (ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (out_text_) {
          *out_text_ = xe::to_utf16(
              std::string_view(text_buffer_.data(), text_buffer_.size()));
        }
        ImGui::CloseCurrentPopup();
        Close();
      }
      if (ImGui::Button("OK")) {
        if (out_text_) {
          *out_text_ = xe::to_utf16(
              std::string_view(text_buffer_.data(), text_buffer_.size()));
        }
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  std::u16string* out_text_ = nullptr;
  std::vector<char> text_buffer_;
  size_t max_length_ = 0;
};

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
dword_result_t XamShowKeyboardUI(dword_t user_index, dword_t flags,
                                 lpu16string_t default_text,
                                 lpu16string_t title, lpu16string_t description,
                                 lpu16string_t buffer, dword_t buffer_length,
                                 pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Broadcast XN_SYS_UI = true
  kernel_state()->BroadcastNotification(0x9, true);

  if (cvars::headless) {
    // Redirect default_text back into the buffer.
    std::memset(buffer, 0, buffer_length * 2);
    if (default_text) {
      xe::store_and_swap<std::u16string>(buffer, default_text.value());
    }

    // Broadcast XN_SYS_UI = false
    kernel_state()->BroadcastNotification(0x9, false);

    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
      return X_ERROR_IO_PENDING;
    } else {
      return X_ERROR_SUCCESS;
    }
  }

  std::u16string out_text;

  auto display_window = kernel_state()->emulator()->display_window();
  xe::threading::Fence fence;
  display_window->loop()->PostSynchronous([&]() {
    (new KeyboardInputDialog(display_window, title ? title.value() : u"",
                             description ? description.value() : u"",
                             default_text ? default_text.value() : u"",
                             &out_text, buffer_length))
        ->Then(&fence);
  });
  ++xam_dialogs_shown_;
  fence.Wait();
  --xam_dialogs_shown_;

  // Zero the output buffer.
  std::memset(buffer, 0, buffer_length * 2);

  // Truncate the string.
  out_text = out_text.substr(0, buffer_length - 1);
  xe::store_and_swap<std::u16string>(buffer, out_text);

  // Broadcast XN_SYS_UI = false
  kernel_state()->BroadcastNotification(0x9, false);

  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT1(XamShowKeyboardUI, kUI, kImplemented);

dword_result_t XamShowDeviceSelectorUI(dword_t user_index, dword_t content_type,
                                       dword_t content_flags,
                                       qword_t total_requested,
                                       lpdword_t device_id_ptr,
                                       pointer_t<XAM_OVERLAPPED> overlapped) {
  // NOTE: 0x00000001 is our dummy device ID from xam_content.cc
  *device_id_ptr = 0x00000001;

  // Broadcast XN_SYS_UI = true followed by XN_SYS_UI = false
  kernel_state()->BroadcastNotification(0x9, true);
  kernel_state()->BroadcastNotification(0x9, false);

  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT1(XamShowDeviceSelectorUI, kUI, kImplemented);

void XamShowDirtyDiscErrorUI(dword_t user_index) {
  if (cvars::headless) {
    assert_always();
    exit(1);
    return;
  }

  auto display_window = kernel_state()->emulator()->display_window();
  xe::threading::Fence fence;
  display_window->loop()->PostSynchronous([&]() {
    xe::ui::ImGuiDialog::ShowMessageBox(
        display_window, "Disc Read Error",
        "There's been an issue reading content from the game disc.\nThis is "
        "likely caused by bad or unimplemented file IO calls.")
        ->Then(&fence);
  });
  ++xam_dialogs_shown_;
  fence.Wait();
  --xam_dialogs_shown_;

  // This is death, and should never return.
  // TODO(benvanik): cleaner exit.
  exit(1);
}
DECLARE_XAM_EXPORT1(XamShowDirtyDiscErrorUI, kUI, kImplemented);

void RegisterUIExports(xe::cpu::ExportResolver* export_resolver,
                       KernelState* kernel_state) {}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
