#include "worker.h"

namespace Seiscomp {
namespace detect {

void Worker::exec() {
  if (init()) {
    run();
  }

  done();
}

void Worker::operator()() { return exec(); }

bool Worker::init() { return true; }

void Worker::done() {}

}  // namespace detect
}  // namespace Seiscomp
