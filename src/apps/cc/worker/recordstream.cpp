#include "recordstream.h"

#include <seiscomp/io/recordinput.h>

#include <memory>

#include "../log.h"

namespace Seiscomp {
namespace detect {
namespace worker {

RecordStream::RecordStream(std::string recordStreamURI)
    : _recordStreamURI{std::move(recordStreamURI)} {}

RecordStream::~RecordStream() { terminate(); }

bool RecordStream::open() {
  // if there is already an active recordstream return `false`
  if (_recordStream) {
    return false;
  }

  _recordStream.reset(IO::RecordStream::Open(_recordStreamURI.c_str()));
  return static_cast<bool>(_recordStream);
}

void RecordStream::start() {
  _recordThread = std::thread{[this]() { read(); }};
}

void RecordStream::terminate() {
  if (_recordStream) {
    _recordStream->close();
  }

  if (_recordThread.joinable()) {
    _recordThread.join();
  }

  _recordStream.reset();
}

bool RecordStream::addStream(const std::string& netCode,
                             const std::string& staCode,
                             const std::string& locCode,
                             const std::string& chaCode) {
  if (!_recordStream) {
    return false;
  }

  return _recordStream->addStream(netCode, staCode, locCode, chaCode);
}

bool RecordStream::setStartTime(const Core::Time& time) {
  if (!_recordStream) {
    return false;
  }
  return _recordStream->setStartTime(time);
}

bool RecordStream::setEndTime(const Core::Time& time) {
  if (!_recordStream) {
    return false;
  }
  return _recordStream->setEndTime(time);
}

bool RecordStream::setTimeWindow(const Core::TimeWindow& tw) {
  if (!_recordStream) {
    return false;
  }

  return _recordStream->setTimeWindow(tw);
}

void RecordStream::setStoreCallback(StoreCallback f) {
  _storeCallback = std::move(f);
}

void RecordStream::setOnAquisitionFinished(OnAquisitionFinishedCallback f) {
  _onAquisitionFinishedCallback = std::move(f);
}

void RecordStream::read() {
  if (!_storeCallback) {
    return;
  }

  SCDETECT_LOG_INFO("Starting record acquisition");

  IO::RecordInput recInput{_recordStream.get()};
  try {
    for (IO::RecordIterator it = recInput.begin(); it != recInput.end(); ++it) {
      std::unique_ptr<Record> rec{*it};
      if (rec) {
        try {
          rec->endTime();
          if (!_storeCallback(std::move(rec))) {
            return;
          }
        } catch (...) {
          SCDETECT_LOG_ERROR(
              "Skipping invalid record for %s.%s.%s.%s (fsamp: %0.2f, nsamp: "
              "%d)",
              rec->networkCode().c_str(), rec->stationCode().c_str(),
              rec->locationCode().c_str(), rec->channelCode().c_str(),
              rec->samplingFrequency(), rec->sampleCount());
        }
      }
    }
  } catch (Core::OperationInterrupted& e) {
    SCDETECT_LOG_INFO("Interrupted acquisition: %s", e.what());
  } catch (std::exception& e) {
    SCDETECT_LOG_INFO("Exception in acquisition: %s", e.what());
  }

  if (_onAquisitionFinishedCallback) {
    _onAquisitionFinishedCallback();
  }
}

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp
