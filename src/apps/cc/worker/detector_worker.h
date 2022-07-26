#ifndef SCDETECT_APPS_CC_WORKER_DETECTORWORKER_H_
#define SCDETECT_APPS_CC_WORKER_DETECTORWORKER_H_

#include <atomic>
#include <boost/variant2/variant.hpp>
#include <memory>
#include <vector>

#include "../detector/detector.h"
#include "../detector/event/status.h"
#include "../util/sync_queue.h"
#include "../worker.h"
#include "event/command.h"
#include "recordstream.h"

namespace Seiscomp {
namespace Client {

struct Notification;

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
  using Id = std::string;
  using ThreadId = std::thread::id;

  enum class Status {
    kInitialized,
    kRunning,
    kTerminated,
    kFinished,
  };

  DetectorWorker(RecordStream&& recordStream,
                 std::vector<std::unique_ptr<Detector>>&& detectors);

  // Returns the worker's identifier which may differ from its `threadId()`
  const Id& id() const;
  // Sets the worker's identifier
  void setId(Id workerId);
  // Returns the worker's thread identifier
  static ThreadId threadId();

  // Returns the worker's status
  Status status() const;

  // Pauses the worker
  void pause(bool enable);
  // Returns whether the worker is currently paused
  bool paused() const;

  // Flushes the worker
  void flush();
  // Closes the worker
  //
  // - after closing the worker won't accept new waveform data (i.e.
  // `event::Record`s) anymore
  void close();
  // Returns whether the worker is closed
  bool closed() const;
  // Gracefully shuts the worker down
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

  struct DetectorEventHandler {
    explicit DetectorEventHandler(DetectorWorker* worker);
    void operator()(detector::event::Link&& ev);
    void operator()(detector::event::Status&& ev);

    template <typename TDetectorEvent>
    void operator()(TDetectorEvent&& ev) {
      boost::variant2::visit(InternalEventHandler{worker},
                             std::forward<TDetectorEvent>(ev));
    }

    DetectorWorker* worker;
  };

  friend DetectorEventHandler;

  struct InternalEventHandler {
    explicit InternalEventHandler(DetectorWorker* worker);
    void operator()(detector::event::Record&& ev);

    template <typename TInternalEvent>
    void operator()(TInternalEvent&& ev) {
      const auto& detectorId{boost::variant2::visit(DetectorIdExtractor{}, ev)};
      // XXX(damb): no bounds checking
      worker->_detectors[worker->_detectorIdIdx[detectorId]]->dispatch(
          std::forward<TInternalEvent>(ev));
    }

    DetectorWorker* worker;
  };

  friend InternalEventHandler;

  struct DetectorIdExtractor {
    DetectorId operator()(const detector::event::Record& ev);

    template <typename TInternalEvent>
    DetectorId operator()(const TInternalEvent& ev) {
      return ev.detectorId;
    }
  };

  void initDetectorIdx(
      const Detector& detector, std::size_t idx,
      const std::set<WaveformStreamId>& associatedWaveformStreamIds);
  void initDetector(Detector& detector);
  void closeDetectors();

  void handle(Event&& ev);

  void handleCommand(const event::Command& ev);
  void handleStatus(const detector::event::Status& ev);

  void dispatch(detector::event::Record&& ev);

  void storeDetectorEvent(Detector::Event&& ev);
  void storeDetection(const Detector* detector,
                      std::unique_ptr<Detector::Detection> detection);
  bool storeRecord(std::unique_ptr<Record> record);

  void startAcquisition();
  void onAquisitionFinished();

  void sleep_or_yield();

  void emitApplicationNotification(const Client::Notification& notification);

  Detectors _detectors;
  DetectorWaveformStreamIdIdx _detectorWaveformStreamIdIdx;
  DetectorIdIdx _detectorIdIdx;

  EventQueue _eventQueue;

  RecordStream _recordStream;

  Id _id;

  EmitApplicationNotificationCallback _emitApplicationNotificationCallback;

  // Sleep duration in microseconds
  boost::optional<int> _sleep_duration{1000};

  // Atomic variable defining the maximum allowed number of records to be
  // stored in the input event queue
  std::atomic<std::size_t> _recordCongestionWindow{10};

  Status _status{Status::kInitialized};

  // An atomic variable indicating whether the worker should pause. When set to
  // `true`, the worker temporarily stops retrieving new tasks out of its queue,
  // although any tasks already executed will keep running until they are
  // finished. Set to `false` again to resume retrieving tasks.
  std::atomic<bool> _paused{false};
  // An atomic variable indicating whether the worker is closed. If closed, it
  // doesn't accept new record events, anymore.
  std::atomic<bool> _closed{false};
  // An atomic variable indicating to the worker to keep running. When set to
  // `true`, the worker permanently stops working.
  std::atomic<bool> _exitRequested{false};
};

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_DETECTORWORKER_H_
