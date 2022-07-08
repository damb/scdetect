#ifndef SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_BUFFER_H_
#define SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_BUFFER_H_

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/genericrecord.h>
#include <seiscomp/core/timewindow.h>
#include <seiscomp/core/typedarray.h>

#include <vector>

#include "../../util/memory.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace template_processor {

// A simple buffer implementation for raw samples
//
// - does neither implement gap interpolation nor does it handle changing
// sampling frequencies
class Buffer {
 public:
  explicit Buffer(const Core::TimeSpan& bufferSize = Core::TimeSpan{0.0});

  // Returns whether the buffer contains data
  bool empty() const noexcept;
  bool full() const;

  const Core::TimeSpan& configuredBufferSize() const;
  void setConfiguredBufferSize(const Core::TimeSpan& duration);

  // Feeds the raw sample `data` into the buffer
  bool feed(const Core::TimeWindow& tw, DoubleArrayPtr& data);

  // Resets the buffer
  void reset();

  template <typename T>
  std::unique_ptr<GenericRecord> contiguousRecord(
      const std::string& networkCode, const std::string& stationCode,
      const std::string& locationCode, const std::string& channelCode,
      double samplingFrequency) {
    auto ret{util::make_unique<GenericRecord>(
        networkCode, stationCode, locationCode, channelCode,
        _dataTimeWindowBuffered.startTime(), samplingFrequency)};
    auto buffered{
        util::make_unique<NumericArray<T>>(_buffer.size(), _buffer.data())};
    ret->setData(buffered.release());

    return ret;
  }

 protected:
 private:
  std::vector<double> _buffer;

  Core::TimeWindow _dataTimeWindowBuffered;
  Core::TimeSpan _bufferSize;
};

}  // namespace template_processor
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_BUFFER_H_
