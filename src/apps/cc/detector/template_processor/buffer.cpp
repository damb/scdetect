#include "buffer.h"

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/timewindow.h>

namespace Seiscomp {
namespace detect {
namespace detector {
namespace template_processor {

Buffer::Buffer(const Core::TimeSpan& bufferSize)
    : _bufferSize{std::abs(bufferSize)} {}

bool Buffer::empty() const noexcept { return _buffer.empty(); }

bool Buffer::full() const {
  return Core::TimeSpan{_dataTimeWindowBuffered.length()} >= _bufferSize;
}

const Core::TimeSpan& Buffer::configuredBufferSize() const {
  return _bufferSize;
}

void Buffer::setConfiguredBufferSize(const Core::TimeSpan& duration) {
  _bufferSize = std::abs(duration);
}

bool Buffer::feed(const Core::TimeWindow& tw, DoubleArrayPtr& data) {
  auto pos{_buffer.end()};
  auto firstElementInserted{_buffer.insert(pos, data->begin(), data->end())};
  if (pos == firstElementInserted) {
    return false;
  }

  if (!_dataTimeWindowBuffered) {
    _dataTimeWindowBuffered = tw;
  } else {
    _dataTimeWindowBuffered.setEndTime(tw.endTime());
  }
  return true;
}

void Buffer::reset() {
  _buffer.clear();
  _dataTimeWindowBuffered = Core::TimeWindow{};
}

}  // namespace template_processor
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
