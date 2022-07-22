#ifndef SCDETECT_APPS_CC_WORKER_DETECTORWORKER_H_
#define SCDETECT_APPS_CC_WORKER_DETECTORWORKER_H_

#include <atomic>
#include <boost/variant2/variant.hpp>
#include <memory>
#include <vector>

#include "../detector/detector.h"
#include "../util/sync_queue.h"
#include "../worker.h"
#include "event/command.h"
#include "recordstream.h"
#include "seiscomp/client/application.h"

namespace Seiscomp {
namespace Client {

class Notification;

}

namespace detect {
namespace worker {

// An event-driven detector worker implementation which implements an
// event-loop.
class DetectorWorker : public Worker {
 public:
  using Detector = detector::Detector;
  using Event = boost::variant2::variant<event::Command, Detector::Event>;
  using EmitApplicationNotificationCallback =
      std::function<void(const Client::Notification&)>;
  using Id = std::thread::id;

  DetectorWorker(RecordStream&& recordStream,
                 std::vector<std::unique_ptr<Detector>>&& detectors);

  // Returns the worker's id
  static Id id();

  // Pauses the worker
  void pause(bool enable);
  // Returns whether the worker is currently paused
  bool paused() const;

  void shutdown();

  // Posts `ev` to the worker
  void postEvent(Event&& ev);

  void setEmitApplicationNotificationCallback(
      EmitApplicationNotificationCallback callback);

 protected:
  bool init() override;
  void run() override;
  void done() override;

 private:
  using Detectors = std::vector<std::unique_ptr<Detector>>;
  using EventQueue = util::SyncQueue<Event>;
  using WaveformStreamId = std::string;
  using DetectorWaveformStreamIdIdx =
      std::unordered_multimap<WaveformStreamId, std::size_t>;
  using DetectorId = std::string;
  using DetectorIdIdx = std::unordered_map<DetectorId, std::size_t>;

  struct EventHandler {
    explicit EventHandler(DetectorWorker* worker);

    void operator()(event::Command&& ev);
    void operator()(Detector::Event&& ev);

    DetectorWorker* worker;
  };

  friend EventHandler;

  struct InternalEventHandler {
    explicit InternalEventHandler(DetectorWorker* worker);
    void operator()(detector::event::Record&& ev);

    template <typename TInternalEvent>
    void operator()(TInternalEvent&& ev) {
      // XXX(damb): no bounds checking
      worker->_detectors[worker->_detectorIdIdx[ev.detectorId]]->dispatch(
          std::forward(ev));
    }

    DetectorWorker* worker;
  };

  friend InternalEventHandler;

  void initDetectorIdx(
      const Detector& detector, std::size_t idx,
      const std::set<WaveformStreamId>& associatedWaveformStreamIds);
  void initDetector(Detector& detector);

  void handle(Event&& ev);

  void handleCommand(const event::Command& ev);

  void dispatchRecord(const detector::event::Record& ev);

  void storeDetectorEvent(Detector::Event&& ev);
  void storeDetection(const Detector* detector,
                      std::unique_ptr<Detector::Detection> detection);
  bool storeRecord(std::unique_ptr<Record> record);
  void onAquisitionFinished();

  void sleep_or_yield();

  void emitApplicationNotification(const Client::Notification& notification);

  Detectors _detectors;
  DetectorWaveformStreamIdIdx _detectorWaveformStreamIdIdx;
  DetectorIdIdx _detectorIdIdx;

  EventQueue _eventQueue;

  RecordStream _recordStream;

  EmitApplicationNotificationCallback _emitApplicationNotificationCallback;

  // Sleep duration in microseconds
  boost::optional<int> _sleep_duration{1000};

  // Atomic variable defining the maximum allowed number of records to be
  // stored in the input event queue
  std::atomic<std::size_t> _recordCongestionWindow{10};

  // An atomic variable indicating whether the worker should pause. When set to
  // `true`, the worker temporarily stops retrieving new tasks out of its queue,
  // although any tasks already executed will keep running until they are
  // finished. Set to `false` again to resume retrieving tasks.
  std::atomic<bool> _paused{false};
  // An atomic variable indicating to the worker to keep running. When set to
  // `true`, the worker permanently stops working.
  std::atomic<bool> _exitRequested{false};
};

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_DETECTORWORKER_H_
