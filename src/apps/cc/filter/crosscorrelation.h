#ifndef SCDETECT_APPS_CC_FILTER_CROSSCORRELATION_H_
#define SCDETECT_APPS_CC_FILTER_CROSSCORRELATION_H_

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/genericrecord.h>
#include <seiscomp/core/typedarray.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/optional/optional.hpp>
#include <cfenv>
#include <cmath>
#include <string>
#include <vector>

#include "../filter.h"
#include "../log.h"
#include "../template_waveform.h"
#include "../util/math.h"

namespace Seiscomp {
namespace detect {
namespace filter {

// Cross-correlation filter implementation
//
// - the filter delay corresponds to the length of the template waveform
// - automatically adopts to different sampling frequencies (i.e. implements
// template waveform resampling facilities)
template <typename TData>
class CrossCorrelation {
 public:
  // Creates a `CrossCorrelation` filter from `waveform`. The filter is
  // configured to the sampling frequency provided by `waveform`.
  //
  // - It is a bug if `waveform` is not a valid pointer.
  explicit CrossCorrelation(const GenericRecordCPtr &waveform);
  // Creates an `CrossCorrelation` filter from `templateWaveform`
  explicit CrossCorrelation(TemplateWaveform templateWaveform);

  virtual ~CrossCorrelation() = default;

  // Apply the cross-correlation in place to the (previously filtered) data.
  // Before using the filter make sure the sam
  void apply(size_t nData, TData *data);

  void apply(std::vector<TData> &data);

  void apply(TypedArray<TData> &data);
  // Reset the cross-correlation filter
  virtual void reset();

  // Set the sampling frequency in Hz. Allows delayed initialization when the
  // data arrive
  void setSamplingFrequency(double sampling_frequency);
  // Returns the configured sampling frequency
  double samplingFrequency() const;

  const TemplateWaveform &templateWaveform() const;

 protected:
  // Compute the actual cross-correlation
  virtual void correlate(size_t nData, TData *data);

  virtual void setupFilter(double samplingFrequency);

 private:
  // The template waveform
  TemplateWaveform _templateWaveform;
  // Buffer for data to be cross-correlated
  boost::circular_buffer<TData> _buffer;

  // Template waveform samples squared summed
  double _sumSquaredTemplateWaveform{0};
  // Template waveform samples summed
  double _sumTemplateWaveform{0};

  double _denominatorTemplateWaveform{0};

  // The data samples squared summed
  double _sumSquaredData{0};
  // The data samples summed
  double _sumData{0};

  bool _initialized{false};
};

template <typename TData>
CrossCorrelation<TData>::CrossCorrelation(const GenericRecordCPtr &waveform)
    : _templateWaveform{TemplateWaveform{waveform}} {
  setupFilter(waveform->samplingFrequency());
}

template <typename TData>
CrossCorrelation<TData>::CrossCorrelation(TemplateWaveform templateWaveform)
    : _templateWaveform{std::move(templateWaveform)} {
  if (_templateWaveform.processingConfig().samplingFrequency) {
    setupFilter(*_templateWaveform.processingConfig().samplingFrequency);
  }
}

template <typename TData>
void CrossCorrelation<TData>::apply(size_t nData, TData *data) {
  correlate(nData, data);
}

template <typename TData>
void CrossCorrelation<TData>::apply(std::vector<TData> &data) {
  apply(data.size(), data.data());
}

template <typename TData>
void CrossCorrelation<TData>::apply(TypedArray<TData> &data) {
  apply(data.size(), data.typedData());
}

template <typename TData>
void CrossCorrelation<TData>::reset() {
  _buffer.clear();

  _sumSquaredData = 0;
  _sumData = 0;

  const double *samples_template_wf{
      TypedArray<TData>::ConstCast(_templateWaveform.waveform().data())
          ->typedData()};
  const int n{_templateWaveform.waveform().data()->size()};
  _sumTemplateWaveform = 0;
  _sumSquaredTemplateWaveform = 0;
  for (int i = 0; i < n; ++i) {
    _sumTemplateWaveform += samples_template_wf[i];
    _sumSquaredTemplateWaveform += util::square(samples_template_wf[i]);
  }

  _denominatorTemplateWaveform =
      std::sqrt(n * _sumSquaredTemplateWaveform -
                _sumTemplateWaveform * _sumTemplateWaveform);

  _buffer.set_capacity(n);
  while (!_buffer.full()) {
    _buffer.push_back(0);
  }
}

template <typename TData>
void CrossCorrelation<TData>::setSamplingFrequency(double sampling_frequency) {
  setupFilter(sampling_frequency);
}

template <typename TData>
const TemplateWaveform &CrossCorrelation<TData>::templateWaveform() const {
  return _templateWaveform;
}

template <typename TData>
double CrossCorrelation<TData>::samplingFrequency() const {
  return _templateWaveform.samplingFrequency();
}

template <typename TData>
void CrossCorrelation<TData>::correlate(size_t nData, TData *data) {
  /*
   * Pearson correlation coefficient for time series X and Y of length n
   *
   *              sum((Xi-meanX) * (Yi-meanY))
   * cc = --------------------------------------------------
   *      sqrt(sum((Xi-meanX)^2)) * sqrt(sum((Yi-meanY)^2))
   *
   * Where sum(X) is the sum of Xi for i=1 until i=n.
   *
   * This can be rearranged in a form suitable for a single-pass algorithm
   * (where the mean of X and Y are not needed):
   *
   *                 n * sum(Xi*Yi) - sum(Xi) * sum(Yi)
   * cc = -----------------------------------------------------------
   *      sqrt(n*sum(Xi^2)-sum(Xi)^2) * sqrt(n*sum(Yi^2)-sum(Yi)^2))
   *
   * Given a cross-correlation with a template waveform trace `tr1` which is
   * correlated against a data trace `tr2` at subsequent offset, pre-compute the
   * parts that involve `tr1` and re-use them at each step of the
   * cross-correlation:
   *
   *   _sumTemplateWaveform = sum(Xi)
   *   _sumSquaredTemplateWaveform= sum(Xi^2)
   *   _denominatorTemplateWaveform = \
   *     sqrt(n*_sumSquaredTemplateWaveform-(_sumTemplateWaveform)^2)
   *
   * For the parts that involve the data trace (extracted from the circular
   * buffer) exclusively, compute the components in a rolling fashion (removing
   * first sample of previous iteration and adding the last sample of the
   * current iteration):
   *
   *   _sumData = sum(Yi)
   *   _sumSquaredData = sum(Yi^2)
   *   denominator_data = sqrt(n*_sumSquaredData-(_sumData)^2)
   *
   * Finally, this is the equation at each step (lag) of cross-correlation in
   * order to compute the Pearson correlation coefficient:
   *
   *       n * sum(Xi*Yi) - _sumTemplateWaveform * _sumData
   * cc = --------------------------------------------------
   *       _denominatorTemplateWaveform * denominator_data
   *
   * Unfortunately, further optimization of sum(Xi*Yi) is not possible which
   * requires to be computed within an inner loop inside the main
   * cross-correlation loop.
   */

  if (!_initialized) {
    throw BaseException{
        "failed to apply cross-correlation filter: not initialized"};
  }

  std::feclearexcept(FE_ALL_EXCEPT);

  const auto n{_buffer.capacity()};
  const TData *samplesTemplateWf{
      TypedArray<TData>::ConstCast(_templateWaveform.waveform().data())
          ->typedData()};
  // cross-correlation loop
  for (size_t i = 0; i < nData; ++i) {
    const TData newSample{data[i]};
    const TData lastSample{_buffer.front()};
    _sumData += newSample - lastSample;
    _sumSquaredData += util::square(newSample) - util::square(lastSample);
    const double denominatorData{
        std::sqrt(n * _sumSquaredData - _sumData * _sumData)};

    _buffer.push_back(newSample);

    double sumTemplateData{0};
    for (size_t k = 0; k < n; ++k) {
      sumTemplateData += samplesTemplateWf[k] * _buffer[k];
    }

    const double pearsonCoeff{
        (n * sumTemplateData - _sumTemplateWaveform * _sumData) /
        (_denominatorTemplateWaveform * denominatorData)};

    int fe{std::fetestexcept(FE_ALL_EXCEPT)};
    if ((fe & ~FE_INEXACT) != 0)  // we don't care about FE_INEXACT
    {
      std::vector<std::string> exceptions;
      if (fe & FE_DIVBYZERO) exceptions.push_back("FE_DIVBYZERO");
      if (fe & FE_INVALID) exceptions.push_back("FE_INVALID");
      if (fe & FE_OVERFLOW) exceptions.push_back("FE_OVERFLOW");
      if (fe & FE_UNDERFLOW) exceptions.push_back("FE_UNDERFLOW");

      std::string msg{
          "Floating point exception during cross-correlation (sample_idx=" +
          std::to_string(i) + ", sample=" + std::to_string(newSample) + "): "};
      msg += boost::algorithm::join(exceptions, ", ");
      SCDETECT_LOG_WARNING("%s", msg.c_str());

      std::feclearexcept(FE_ALL_EXCEPT);
    }

    data[i] =
        static_cast<TData>(std::isfinite(pearsonCoeff) ? pearsonCoeff : 0);
  }
}

template <typename TData>
void CrossCorrelation<TData>::setupFilter(double samplingFrequency) {
  assert((samplingFrequency > 0));

  _initialized = false;
  _templateWaveform.setSamplingFrequency(samplingFrequency);
  reset();
  _initialized = true;
}

}  // namespace filter
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_FILTER_CROSSCORRELATION_H_
