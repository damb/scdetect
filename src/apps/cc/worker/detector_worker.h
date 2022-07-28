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
// event-loop
//
// - most of the member functions aren't thread-safe; post corresponding
// `event::Command`s in order to communicate with the worker
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
    // Indicating whether the worker is running
    kRunning,
    // Indicating whether the worker is paused (i.e. whether the worker
    // temporarily stops retrieving new tasks out of its queue).  Any tasks
    // already executed will keep running until they are finished.
    kPaused,
    // Indicating whether the worker is closed. If closed, it doesn't accept
    // new `event::Record`s, anymore.
    kClosed,
    // Indicating whether the worker was terminated
    kTerminated,
    // Indicating whether the worker finished gracefully
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
  //
  // - thead-safe
  Status status() const;

  // Pauses the worker
  //
  // - if unpausing the worker the next status will be `Status::kRunning`
  // - thead-safe
  void pause(bool enable);
  // Returns whether the worker is currently paused
  //
  // - thread-safe
  bool paused() const;

  // Flushes the worker
  void flush();
  // Closes the worker
  //
  // - after closing the worker won't accept new waveform data (i.e.
  // `event::Record`s) anymore
  // - thread-safe
  void close();
  // Returns whether the worker is closed
  //
  // - thead-safe
  bool closed() const;
  // Terminates the worker
  void terminate();
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

  // An atomic variable referring to the current status of the worker
  std::atomic<Status> _status{Status::kInitialized};

  // Indicates whether to stop running (`true`) or not (`false`)
  bool _exitRequested{false};
};

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_DETECTORWORKER_H_
