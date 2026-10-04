#pragma once

#include "core/platform/async.h"

namespace app::async {

using core::async::CancelToken;
using core::async::Result;
using core::async::Status;
using core::async::cancel;
using core::async::failure;
using core::async::restart;
using core::async::runOnce;
using core::async::running;
using core::async::status;
using core::async::success;

} // namespace app::async
