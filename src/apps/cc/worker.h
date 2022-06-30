#ifndef SCDETECT_APPS_CC_WORKER_H_
#define SCDETECT_APPS_CC_WORKER_H_

namespace Seiscomp {
namespace detect {

class Worker {
 public:
  virtual ~Worker() = default;

  void exec();
  void operator()();

 protected:
  virtual bool init();
  virtual void run() = 0;
  virtual void done();
};

}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_H_
