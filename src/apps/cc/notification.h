#ifndef SCDETECT_APPS_CC_NOTIFICATION_H_
#define SCDETECT_APPS_CC_NOTIFICATION_H_

namespace Seiscomp {
namespace detect {

// Custom worker notifications (negative values)
enum class WorkerNotification {
  kInitializing = -1000,
  kInitialized,
  kRunning,
  kTerminating,
  kFinished,
  kDetection
};

}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_NOTIFICATION_H_
