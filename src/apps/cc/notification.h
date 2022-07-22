#ifndef SCDETECT_APPS_CC_NOTIFICATION_H_
#define SCDETECT_APPS_CC_NOTIFICATION_H_

#include <seiscomp/core/baseobject.h>
#include <seiscomp/core/defs.h>

#include <thread>

namespace Seiscomp {
namespace detect {

DEFINE_SMARTPOINTER(WorkerNotification);
// Base worker notification
class WorkerNotification : public Core::BaseObject {
 public:
  // Custom internal application worker notification types (negative values)
  enum class Type {
    kInitializing = -1000,
    kInitialized,
    kRunning,
    kTerminating,
    kFinished,
    kDetection = -1,
  };

  std::thread::id threadId{std::this_thread::get_id()};
};

}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_NOTIFICATION_H_
