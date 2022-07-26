#include "notification.h"

namespace Seiscomp {
namespace detect {

WorkerNotification::WorkerNotification(std::string workerId)
    : workerId{std::move(workerId)} {}

}  // namespace detect
}  // namespace Seiscomp
