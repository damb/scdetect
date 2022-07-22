#include "command.h"

namespace Seiscomp {
namespace detect {
namespace worker {
namespace event {

Command::Command(Type type) : _type{type} {}

Command::Type Command::type() const { return _type; }

}  // namespace event
}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp
