#include "detector_worker.h"

#include <seiscomp/client/application.h>

#include <boost/variant2/variant.hpp>
#include <cstddef>
#include <string>
#include <thread>

#include "../notification.h"
#include "../notification/detection.h"
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

const DetectorWorker::Id& DetectorWorker::id() const { return _id; }

void DetectorWorker::setId(Id workerId) { _id = std::move(workerId); }

DetectorWorker::ThreadId DetectorWorker::threadId() {
  return std::this_thread::get_id();
}

DetectorWorker::Status DetectorWorker::status() const { return _status; }

void DetectorWorker::pause(bool enable) { _paused = enable; }

bool DetectorWorker::paused() const { return _paused; }

void DetectorWorker::flush() {
  for (auto& detector : _detectors) {
    detector->flush();
  }
}

void DetectorWorker::close() {
  _closed = true;
  closeDetectors();
}

bool DetectorWorker::closed() const { return _closed; }

void DetectorWorker::terminate() {
  _recordStream.terminate();

  for (auto& detector : _detectors) {
    detector->terminate();
  }

  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::Type::kTerminated),
      new WorkerNotification{id()}});

  _exitRequested = true;

  _status = Status::kTerminated;
}

void DetectorWorker::shutdown() {
  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::Type::kShuttingDown),
      new WorkerNotification{id()}});

  for (auto& detector : _detectors) {
    detector->flush();
  }

  _exitRequested = true;

  _status = Status::kFinished;

  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::Type::kShutdown),
      new WorkerNotification{id()}});
}

void DetectorWorker::postEvent(Event&& ev) { _eventQueue.put(std::move(ev)); }

void DetectorWorker::setEmitApplicationNotificationCallback(
    EmitApplicationNotificationCallback callback) {
  _emitApplicationNotificationCallback = std::move(callback);
}

bool DetectorWorker::init() {
  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::Type::kInitializing),
      new WorkerNotification{id()}});

  for (std::size_t i{0}; i < _detectors.size(); ++i) {
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
    return storeRecord(std::move(record));
  });

  _recordStream.setOnAquisitionFinished([this]() { onAquisitionFinished(); });

  _status = Status::kInitialized;

  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::Type::kInitialized),
      new WorkerNotification{id()}});

  return true;
}

void DetectorWorker::run() {
  startAcquisition();

  _status = Status::kRunning;

  emitApplicationNotification(
      Client::Notification{static_cast<int>(WorkerNotification::Type::kRunning),
                           new WorkerNotification{id()}});
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
  // machines which defines the threshold when the *congestion window* needs
  // to be adjusted.
  // - it still needs to be clarified which strategy should be applied (e.g.
  // AIMD)
  // - the initial congestion window value must depend on the template
  // processor buffer size.

  // -> control the record stream by means of the congestion window

  boost::variant2::visit(DetectorEventHandler{worker}, std::move(ev));
}

DetectorWorker::DetectorEventHandler::DetectorEventHandler(
    DetectorWorker* worker)
    : worker{worker} {}

void DetectorWorker::DetectorEventHandler::operator()(
    detector::event::Link&& ev) {
  // XXX(damb): no bounds checking
  worker->_detectors[worker->_detectorIdIdx[ev.detectorId]]->dispatch(
      std::move(ev));
}

void DetectorWorker::DetectorEventHandler::operator()(
    detector::event::Status&& ev) {
  worker->handleStatus(ev);
}

DetectorWorker::InternalEventHandler::InternalEventHandler(
    DetectorWorker* worker)
    : worker{worker} {}

void DetectorWorker::InternalEventHandler::operator()(
    detector::event::Record&& ev) {
  worker->dispatch(std::move(ev));
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

void DetectorWorker::closeDetectors() {
  for (auto& detector : _detectors) {
    detector->close();
  }
}

void DetectorWorker::handleCommand(const event::Command& ev) {
  using CommandType = event::Command::Type;
  switch (ev.type()) {
    case CommandType::kClose: {
      close();
      break;
    }
    case CommandType::kShutdown: {
      flush();
      shutdown();
      break;
    }
    case CommandType::kTerminate: {
      terminate();
      break;
    }
    default:
      SCDETECT_LOG_WARNING("Unknown command type: %d",
                           static_cast<int>(ev.type()));
  }
}

void DetectorWorker::handleStatus(const detector::event::Status& ev) {
  using DetectorStatusType = detector::event::Status::Type;
  switch (ev.type) {
    case DetectorStatusType::kFinishedProcessing: {
      auto allDetectorsFinishedProcessing{
          std::all_of(std::begin(_detectors), std::end(_detectors),
                      [](const Detectors::value_type& detector) {
                        return detector->finished();
                      })};

      if (allDetectorsFinishedProcessing) {
        emitApplicationNotification(Client::Notification{
            static_cast<int>(WorkerNotification::Type::kFinishedProcessing),
            new WorkerNotification{id()}});

        _status = Status::kFinished;
      }
      break;
    }
    default:
      SCDETECT_LOG_WARNING("unhandled status type: %d",
                           static_cast<int>(ev.type));
      break;
  }
}

void DetectorWorker::dispatch(detector::event::Record&& ev) {
  auto range{_detectorWaveformStreamIdIdx.equal_range(ev.record->streamID())};
  for (auto it{range.first}; it != range.second; ++it) {
    auto& detector{_detectors[it->second]};

    // XXX(damb): clone. The record might be used by multiple detectors.
    detector->dispatch(Detector::Event{detector::event::Record{ev}});
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
  auto* payload{new notification::Detection{id()}};
  payload->detectorId = detector->id();
  payload->detection = std::move(detection);
  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::Type::kDetection), payload});
}

bool DetectorWorker::storeRecord(std::unique_ptr<Record> record) {
  // TODO(damb): monitor the queue size and modify the congestion window
  // in order to avoid feeding all the records at once
  _eventQueue.put(detector::event::Record{record.release()},
                  _recordCongestionWindow);
  return true;
}

void DetectorWorker::startAcquisition() {
  _recordStream.open();
  _recordStream.start();
}

void DetectorWorker::onAquisitionFinished() {
  emitApplicationNotification(Client::Notification{
      static_cast<int>(WorkerNotification::Type::kFinishedRecordStreaming),
      new WorkerNotification{id()}});
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
    return;
  }
  // XXX(damb): this should never happen
  delete notification.object;
}

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp
