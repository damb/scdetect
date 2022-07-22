#include "detector_worker.h"

#include <seiscomp/client/application.h>

#include <boost/variant2/variant.hpp>
#include <string>
#include <thread>

#include "../notification.h"
#include "../util/waveform_stream_id.h"
#include "event/command.h"

namespace Seiscomp {
namespace detect {
namespace worker {

DetectorWorker::DetectorWorker(
    RecordStream&& recordStream,
    std::vector<std::unique_ptr<Detector>>&& detectors)
    : _detectors{std::move(detectors)},
      _recordStream{std::move(recordStream)} {}

DetectorWorker::Id DetectorWorker::id() { return std::this_thread::get_id(); }

void DetectorWorker::pause(bool enable) { _paused = enable; }

bool DetectorWorker::paused() const { return _paused; }

void DetectorWorker::shutdown() { _exitRequested = true; }

void DetectorWorker::postEvent(Event&& ev) { _eventQueue.put(std::move(ev)); }

void DetectorWorker::setEmitApplicationNotificationCallback(
    EmitApplicationNotificationCallback callback) {
  _emitApplicationNotificationCallback = std::move(callback);
}

bool DetectorWorker::init() {
  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::kInitializing)});

  for (auto i{0}; i < _detectors.size(); ++i) {
    auto& detector{_detectors[i]};
    const auto& associatedWaveformStreamIds{
        detector->associatedWaveformStreamIds()};

    initDetectorIdx(*detector, i, associatedWaveformStreamIds);
    initDetector(*detector);

    // subscribe to recordStream
    for (const auto& waveformStreamIdStr : associatedWaveformStreamIds) {
      util::WaveformStreamID waveformStreamId{waveformStreamIdStr};
      _recordStream.addStream(
          waveformStreamId.netCode(), waveformStreamId.staCode(),
          waveformStreamId.locCode(), waveformStreamId.chaCode());
    }
  }

  _recordStream.setStoreCallback([this](std::unique_ptr<Record> record) {
    storeRecord(std::move(record));
    return true;
  });

  _recordStream.setOnAquisitionFinished([this]() { onAquisitionFinished(); });

  emitApplicationNotification(
      Client::Notification{static_cast<int>(WorkerNotification::kInitialized)});

  return true;
}

void DetectorWorker::run() {
  emitApplicationNotification(
      Client::Notification{static_cast<int>(WorkerNotification::kRunning)});

  // event loop
  while (!_exitRequested) {
    Event ev;
    if (!_paused &&
        _eventQueue.get(ev, std::chrono::microseconds(*_sleep_duration))) {
      handle(std::move(ev));
    } else {
      sleep_or_yield();
    }
  }
}

void DetectorWorker::done() {
  emitApplicationNotification(
      Client::Notification{static_cast<int>(WorkerNotification::kTerminating)});

  Worker::done();

  _recordStream.terminate();
}

DetectorWorker::EventHandler::EventHandler(DetectorWorker* worker)
    : worker{worker} {}

void DetectorWorker::EventHandler::operator()(event::Command&& ev) {
  worker->handleCommand(ev);
}

void DetectorWorker::EventHandler::operator()(Detector::Event&& ev) {
  // TODO(damb): here, some kind of congestion control is required in order to
  // not to feed all records at once -> this is particularly important when
  // processing data in offline mode.
  //
  // http://www.diva-portal.org/smash/get/diva2:1349831/FULLTEXT01.pdf
  //
  // - requires the user to configure the max concurrent processor state
  // machines which defines the threshold when the *congestion window* needs to
  // be adjusted.
  // - it still needs to be clarified which strategy should be applied (e.g.
  // AIMD)
  // - the initial congestion window value must depend on the template
  // processor buffer size.

  // -> control the record stream by means of the congestion window

  boost::variant2::visit(InternalEventHandler{worker}, std::move(ev));
}

DetectorWorker::InternalEventHandler::InternalEventHandler(
    DetectorWorker* worker)
    : worker{worker} {}

void DetectorWorker::InternalEventHandler::operator()(
    detector::event::Record&& ev) {
  worker->dispatchRecord(ev);
}

void DetectorWorker::handle(Event&& ev) {
  boost::variant2::visit(EventHandler{this}, std::move(ev));
}
void DetectorWorker::initDetectorIdx(
    const Detector& detector, std::size_t idx,
    const std::set<WaveformStreamId>& associatedWaveformStreamIds) {
  _detectorIdIdx.emplace(detector.id(), idx);
  for (const auto& waveformStreamId : associatedWaveformStreamIds) {
    _detectorWaveformStreamIdIdx.emplace(waveformStreamId, idx);
  }
}

void DetectorWorker::initDetector(Detector& detector) {
  // configure callbacks
  detector.setEmitDetectionCallback(
      [this](const Detector* detector,
             std::unique_ptr<Detector::Detection> detection) {
        storeDetection(detector, std::move(detection));
      });

  detector.setEmitEventCallback([this](detector::Detector::Event&& ev) {
    storeDetectorEvent(std::move(ev));
  });
}

void DetectorWorker::handleCommand(const event::Command& ev) {
  switch (ev.type()) {
    case event::Command::Type::kShutdown:
      shutdown();
      break;
    default:
      SCDETECT_LOG_WARNING("unhandled command type: %d",
                           static_cast<int>(ev.type()));
  }
}

void DetectorWorker::dispatchRecord(const detector::event::Record& ev) {
  auto range{_detectorWaveformStreamIdIdx.equal_range(ev.record->streamID())};
  for (auto it{range.first}; it != range.second; ++it) {
    auto& detector{_detectors[it->second]};

    detector::event::Record cloned{ev};
    detector->dispatch(std::move(cloned));
    if (detector->finished()) {
      detector->reset();
    }
  }
}

void DetectorWorker::storeDetectorEvent(Detector::Event&& ev) {
  _eventQueue.put(std::move(ev));
}

void DetectorWorker::storeDetection(
    const Detector* detector, std::unique_ptr<Detector::Detection> detection) {
  // TODO TODO TODO
  // - attach detection to Application Notification
  emitApplicationNotification(
      Client::Notification{static_cast<int>(WorkerNotification::kDetection)});
}

bool DetectorWorker::storeRecord(std::unique_ptr<Record> record) {
  Event ev{detector::event::Record{record.release()}};
  // TODO(damb): monitor the queue size and modify the congestion window
  // in order to avoid feeding all the records at once
  _eventQueue.put(std::move(ev), _recordCongestionWindow);
  return true;
}

void DetectorWorker::onAquisitionFinished() {
  emitApplicationNotification(
      Client::Notification{static_cast<int>(WorkerNotification::kFinished)});
}

void DetectorWorker::sleep_or_yield() {
  if (_sleep_duration) {
    std::this_thread::sleep_for(std::chrono::microseconds(*_sleep_duration));
  } else {
    std::this_thread::yield();
  }
}

void DetectorWorker::emitApplicationNotification(
    const Client::Notification& notification) {
  if (_emitApplicationNotificationCallback) {
    _emitApplicationNotificationCallback(notification);
  }
}

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp
