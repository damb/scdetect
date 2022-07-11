#ifndef SCDETECT_APPS_CC_WORKER_EVENT_COMMAND_H_
#define SCDETECT_APPS_CC_WORKER_EVENT_COMMAND_H_

namespace Seiscomp {
namespace detect {
namespace worker {
namespace event {

class Command {
 public:
  enum class Type { kShutdown };

  explicit Command(Type type = Type ::kShutdown);

  Type type() const;

 private:
  Type _type{Type::kShutdown};
};

}  // namespace event
}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_EVENT_COMMAND_H_
