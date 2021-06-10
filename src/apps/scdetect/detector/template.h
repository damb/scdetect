#ifndef SCDETECT_APPS_SCDETECT_DETECTOR_TEMPLATE_H_
#define SCDETECT_APPS_SCDETECT_DETECTOR_TEMPLATE_H_

#include <cstdlib>
#include <ostream>
#include <string>

#include <boost/optional.hpp>

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/timewindow.h>
#include <seiscomp/datamodel/eventparameters.h>
#include <seiscomp/datamodel/pick.h>
#include <seiscomp/datamodel/stream.h>

#include "../builder.h"
#include "../filter/crosscorrelation.h"
#include "../waveformprocessor.h"

namespace Seiscomp {
namespace detect {
namespace detector {

// Template waveform processor implementation
// - implements resampling and filtering
// - applies the cross-correlation algorithm
class Template : public WaveformProcessor {

public:
  // Creates a `Template` waveform processor. Waveform related parameters are
  // forwarded to the underlying cross-correlation instance.
  Template(const GenericRecordCPtr &waveform, const std::string filter_id,
           const Core::Time &template_starttime,
           const Core::Time &template_endtime, const std::string &processor_id,
           const Processor *p = nullptr);

  DEFINE_SMARTPOINTER(MatchResult);
  struct MatchResult : public Result {
    double coefficient{std::nan("")};
    double lag{}; // seconds

    // Time window for w.r.t. the match result
    Core::TimeWindow time_window;
  };

  void set_filter(Filter *filter,
                  const Core::TimeSpan &init_time = 0.0) override;

  const Core::TimeWindow &processed() const override;

  void Reset() override;

  void set_target_sampling_frequency(double f);
  boost::optional<double> target_sampling_frequency() const;

  // Returns the template waveform starttime
  boost::optional<const Core::Time> template_starttime() const;
  // Returns the template waveform endtime
  boost::optional<const Core::Time> template_endtime() const;

protected:
  WaveformProcessor::StreamState &stream_state(const Record *record) override;

  void Process(StreamState &stream_state, const Record *record,
               const DoubleArray &filtered_data) override;

  void Fill(StreamState &stream_state, const Record *record,
            DoubleArrayPtr &data) override;

  void SetupStream(StreamState &stream_state, const Record *record) override;

private:
  StreamState stream_state_;

  // The optional target sampling frequency (used for on-the-fly resampling)
  boost::optional<double> target_sampling_frequency_;
  // The in-place cross-correlation filter
  filter::AdaptiveCrossCorrelation<double> cross_correlation_;
};

} // namespace detector
} // namespace detect
} // namespace Seiscomp

#endif // SCDETECT_APPS_SCDETECT_DETECTOR_TEMPLATE_H_
