/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xam_app.h"

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XamApp::XamApp(KernelState* kernel_state) : App(kernel_state, 0xFE) {}

X_RESULT XamApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                     uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00020021: {
      struct message_data {
        char unk_00[64];
        xe::be<uint32_t> unk_40;  // KeGetCurrentProcessType() < 1 ? 1 : 0
        xe::be<uint32_t> unk_44;  // ? output_ptr ?
        xe::be<uint32_t> unk_48;  // ? overlapped_ptr ?
      }* data = reinterpret_cast<message_data*>(buffer);
      assert_true(buffer_length == sizeof(message_data));
      auto unk = memory_->TranslateVirtual<xe::be<uint32_t>*>(data->unk_44);
      *unk = 0;
      XELOGD("XamApp(0x00020021)('{}', {:08X}, {:08X}, {:08X})", data->unk_00,
             (uint32_t)data->unk_40, (uint32_t)data->unk_44,
             (uint32_t)data->unk_48);
      return X_ERROR_SUCCESS;
    }
    case 0x00021012: {
      XELOGD("XamApp(0x00021012)");
      return X_ERROR_SUCCESS;
    }
    case 0x00022005: {
      struct message_data {
        xe::be<uint32_t> unk_00;  // ? output_ptr ?
        xe::be<uint32_t> unk_04;  // ? value/jump to? ?
      }* data = reinterpret_cast<message_data*>(buffer);
      assert_true(buffer_length == sizeof(message_data));
      auto unk = memory_->TranslateVirtual<xe::be<uint32_t>*>(data->unk_00);
      auto adr = *unk;
      XELOGD("XamApp(0x00022005)(%.8X, %.8X)", (uint32_t)data->unk_00,
             (uint32_t)data->unk_04);
      return X_ERROR_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XAM message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
