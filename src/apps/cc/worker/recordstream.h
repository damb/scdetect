#ifndef SCDETECT_APPS_CC_WORKER_RECORDSTREAM_H_
#define SCDETECT_APPS_CC_WORKER_RECORDSTREAM_H_

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/timewindow.h>
#include <seiscomp/io/recordstream.h>

#include <memory>
#include <thread>

namespace Seiscomp {
namespace detect {
namespace worker {

class RecordStream {
 public:
  explicit RecordStream(std::string recordStreamURI);
  ~RecordStream();
  RecordStream(const RecordStream&) = delete;
  RecordStream& operator=(const RecordStream&) = delete;
  RecordStream(RecordStream&&) = default;
  RecordStream& operator=(RecordStream&&) = default;

  bool open();
  void start();
  void terminate();

  bool addStream(const std::string& netCode, const std::string& staCode,
                 const std::string& locCode, const std::string& chaCode);

  bool setStartTime(const Core::Time& time);
  bool setEndTime(const Core::Time& time);
  bool setTimeWindow(const Core::TimeWindow& tw);

  using StoreCallback = std::function<bool(std::unique_ptr<Record>)>;
  void setStoreCallback(StoreCallback f);
  using OnAquisitionFinishedCallback = std::function<void()>;
  void setOnAquisitionFinished(OnAquisitionFinishedCallback f);

 private:
  void read();

  StoreCallback _storeCallback{[](std::unique_ptr<Record>) { return false; }};
  OnAquisitionFinishedCallback _onAquisitionFinishedCallback;

  std::thread _recordThread;
  std::string _recordStreamURI;
  std::unique_ptr<IO::RecordStream> _recordStream;
};

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_RECORDSTREAM_H_
