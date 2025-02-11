#include "timewindow_processor.h"

namespace Seiscomp {
namespace detect {
namespace processing {

void TimeWindowProcessor::setTimeWindow(const Core::TimeWindow &tw) {
  if (!tw) {
    _timeWindow = Core::TimeWindow{};
    _safetyTimeWindow = Core::TimeWindow{};
    reset();
    return;
  }
  _timeWindow = tw;
  _safetyTimeWindow.setStartTime(_timeWindow.startTime() - _initTime);
  _safetyTimeWindow.setEndTime(_timeWindow.endTime());
  reset();
}

const Core::TimeWindow &TimeWindowProcessor::timeWindow() const {
  return _timeWindow;
}

const Core::TimeWindow &TimeWindowProcessor::safetyTimeWindow() const {
  return _safetyTimeWindow;
}

void TimeWindowProcessor::computeTimeWindow() {
  throw Processor::BaseException{"failed to compute time window"};
}

bool TimeWindowProcessor::store(const Record *record) {
  if (!record->timeWindow().overlaps(_safetyTimeWindow)) {
    if (status() > Status::kInProgress) {
      return false;
    }

    // Terminate the processor if a record arrives with a starttime later than
    // the requested time window
    if (record->startTime() > _safetyTimeWindow.endTime()) {
      setStatus(Status::kTerminated, 0.0);
    }

    return false;
  }

  return WaveformProcessor::store(record);
}

bool TimeWindowProcessor::enoughDataReceived(
    const StreamState &streamState) const {
  return streamState.dataTimeWindow.startTime() <=
             _safetyTimeWindow.startTime() &&
         streamState.dataTimeWindow.endTime() >= _safetyTimeWindow.endTime();
}

}  // namespace processing
}  // namespace detect
}  // namespace Seiscomp
