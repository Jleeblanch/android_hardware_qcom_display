/*
* Copyright (c) 2014 - 2016, 2018, 2020 The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without modification, are permitted
* provided that the following conditions are met:
*    * Redistributions of source code must retain the above copyright notice, this list of
*      conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above copyright notice, this list of
*      conditions and the following disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its contributors may be used to
*      endorse or promote products derived from this software without specific prior written
*      permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <dlfcn.h>
#include <signal.h>
#include <utils/constants.h>
#include <utils/debug.h>
#include <utils/locker.h>
#include <utils/utils.h>

#include "color_manager.h"
#include "core_impl.h"
#include "display_builtin.h"
#include "display_pluggable.h"
#include "display_virtual.h"
#include "hw_info_interface.h"

#define __CLASS__ "CoreImpl"

namespace sdm {

CoreImpl::CoreImpl(BufferAllocator *buffer_allocator,
                   SocketHandler *socket_handler)
  : buffer_allocator_(buffer_allocator), socket_handler_(socket_handler) {
}

DisplayError CoreImpl::Init() {
  SCOPE_LOCK(locker_);
  DisplayError error = kErrorNone;

  // Try to load extension library & get handle to its interface.
  if (extension_lib_.Open(EXTENSION_LIBRARY_NAME)) {
    if (!extension_lib_.Sym(CREATE_EXTENSION_INTERFACE_NAME,
                            reinterpret_cast<void **>(&create_extension_intf_)) ||
        !extension_lib_.Sym(DESTROY_EXTENSION_INTERFACE_NAME,
                            reinterpret_cast<void **>(&destroy_extension_intf_))) {
      DLOGE("Unable to load symbols, error = %s", extension_lib_.Error());
      return kErrorUndefined;
    }

    error = create_extension_intf_(EXTENSION_VERSION_TAG, &extension_intf_);
    if (error != kErrorNone) {
      DLOGE("Unable to create interface");
      return error;
    }
  } else {
    DLOGW("Unable to load = %s, error = %s", EXTENSION_LIBRARY_NAME, extension_lib_.Error());
  }

  error = HWInfoInterface::Create(&hw_info_intf_);
  if (error != kErrorNone) {
    goto CleanupOnError;
  }

  error = hw_info_intf_->GetHWResourceInfo(&hw_resource_);
  if (error != kErrorNone) {
    goto CleanupOnError;
  }

  error = comp_mgr_.Init(hw_resource_, extension_intf_, buffer_allocator_, socket_handler_);

  if (error != kErrorNone) {
    goto CleanupOnError;
  }

  error = ColorManagerProxy::Init(hw_resource_);
  // if failed, doesn't affect display core functionalities.
  if (error != kErrorNone) {
    DLOGW("Unable creating color manager and continue without it.");
  }

  // Populate hw_displays_info_ once.
  error = hw_info_intf_->GetDisplaysStatus(&hw_displays_info_);
  if (error != kErrorNone) {
    DLOGW("Failed getting displays status. Error = %d", error);
  }

  signal(SIGPIPE, SIG_IGN);
  return kErrorNone;

CleanupOnError:
  if (hw_info_intf_) {
    HWInfoInterface::Destroy(hw_info_intf_);
  }

  return error;
}

DisplayError CoreImpl::Deinit() {
  SCOPE_LOCK(locker_);

  ColorManagerProxy::Deinit();

  comp_mgr_.Deinit();
  HWInfoInterface::Destroy(hw_info_intf_);

  return kErrorNone;
}

DisplayError CoreImpl::CreateDisplay(DisplayType type, DisplayEventHandler *event_handler,
                                     DisplayInterface **intf) {
  SCOPE_LOCK(locker_);

  if (!event_handler || !intf) {
    return kErrorParameters;
  }

  DisplayBase *display_base = NULL;

  switch (type) {
    case kBuiltIn:
      display_base = new DisplayBuiltIn(event_handler, hw_info_intf_, buffer_allocator_,
                                        &comp_mgr_);
      break;
    case kPluggable:
      display_base = new DisplayPluggable(event_handler, hw_info_intf_, buffer_allocator_,
                                          &comp_mgr_);
      break;
    case kVirtual:
      display_base = new DisplayVirtual(event_handler, hw_info_intf_, buffer_allocator_,
                                        &comp_mgr_);
      break;
    default:
      DLOGE("Spurious display type %d", type);
      return kErrorParameters;
  }

  if (!display_base) {
    return kErrorMemory;
  }

  DisplayError error = display_base->Init();
  if (error != kErrorNone) {
    delete display_base;
    return error;
  }

  *intf = display_base;
  return kErrorNone;
}

DisplayError CoreImpl::CreateDisplay(int32_t display_id, DisplayEventHandler *event_handler,
                                     DisplayInterface **intf) {
  SCOPE_LOCK(locker_);

  if (!event_handler || !intf) {
    return kErrorParameters;
  }

  auto iter = hw_displays_info_.find(display_id);

  if (iter == hw_displays_info_.end()) {
    DLOGE("Spurious display id %d", display_id);
    return kErrorParameters;
  }

  DisplayBase *display_base = NULL;
  DisplayType display_type = iter->second.display_type;

  switch (display_type) {
    case kBuiltIn:
      display_base = new DisplayBuiltIn(display_id, event_handler, hw_info_intf_,
                                        buffer_allocator_, &comp_mgr_);
      break;
    case kPluggable:
      display_base = new DisplayPluggable(display_id, event_handler, hw_info_intf_,
                                          buffer_allocator_, &comp_mgr_);
      break;
    case kVirtual:
      display_base = new DisplayVirtual(display_id, event_handler, hw_info_intf_,
                                        buffer_allocator_, &comp_mgr_);
      break;
    default:
      DLOGE("Spurious display type %d", display_type);
      return kErrorParameters;
  }

  if (!display_base) {
    return kErrorMemory;
  }

  DisplayError error = display_base->Init();
  if (error != kErrorNone) {
    delete display_base;
    return error;
  }

  *intf = display_base;

  return kErrorNone;
}

DisplayError CoreImpl::DestroyDisplay(DisplayInterface *intf) {
  SCOPE_LOCK(locker_);

  if (!intf) {
    return kErrorParameters;
  }

  DisplayBase *display_base = static_cast<DisplayBase *>(intf);
  display_base->Deinit();
  delete display_base;

  return kErrorNone;
}

DisplayError CoreImpl::SetMaxBandwidthMode(HWBwModes mode) {
  SCOPE_LOCK(locker_);

  return comp_mgr_.SetMaxBandwidthMode(mode);
}

DisplayError CoreImpl::GetFirstDisplayInterfaceType(HWDisplayInterfaceInfo *hw_disp_info) {
  SCOPE_LOCK(locker_);
  return hw_info_intf_->GetFirstDisplayInterfaceType(hw_disp_info);
}

DisplayError CoreImpl::GetDisplaysStatus(HWDisplaysInfo *hw_displays_info) {
  SCOPE_LOCK(locker_);
  DisplayError error = hw_info_intf_->GetDisplaysStatus(hw_displays_info);
  if (kErrorNone == error) {
    // Needed for error-checking in CreateDisplay(int32_t display_id, ...) and getting display-type.
    hw_displays_info_ = *hw_displays_info;
  }
  return error;
}

DisplayError CoreImpl::GetMaxDisplaysSupported(DisplayType type, int32_t *max_displays) {
  SCOPE_LOCK(locker_);
  return hw_info_intf_->GetMaxDisplaysSupported(type, max_displays);
}

bool CoreImpl::IsRotatorSupportedFormat(LayerBufferFormat format) {
  SCOPE_LOCK(locker_);
  return comp_mgr_.IsRotatorSupportedFormat(format);
}

}  // namespace sdm

