#ifndef SCDETECT_APPS_CC_WORKER_EVENT_COMMAND_H_
#define SCDETECT_APPS_CC_WORKER_EVENT_COMMAND_H_

namespace Seiscomp {
namespace detect {
namespace worker {
namespace event {

class Command {
 public:
  enum class Type { kClose, kShutdown, kTerminate };

  explicit Command(Type type = Type::kClose);

  Type type() const;

 private:
  Type _type{Type::kClose};
};

}  // namespace event
}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_EVENT_COMMAND_H_
