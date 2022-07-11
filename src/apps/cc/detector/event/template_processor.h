#ifndef SCDETECT_APPS_CC_DETECTOR_EVENT_TEMPLATEPROCESSOR_H_
#define SCDETECT_APPS_CC_DETECTOR_EVENT_TEMPLATEPROCESSOR_H_

#include "../../def.h"
#include "../detail.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace event {

struct TemplateProcessorEvent {
  detail::ProcessorIdType detectorId;
  detail::ProcessorIdType templateProcessorId;
};

struct CheckSaturation : public TemplateProcessorEvent {
  double threshold{0};
};

struct Resample : public TemplateProcessorEvent {
  boost::optional<double> targetSamplingFrequency;
};

struct Filter : public TemplateProcessorEvent {
  DoubleFilter* filter{nullptr};
};

struct CrossCorrelate : public TemplateProcessorEvent {
  detail::CrossCorrelation* filter{nullptr};
};

struct Process : public TemplateProcessorEvent {};

struct Reset : public TemplateProcessorEvent {};

// Emitted when a final state is entered
struct Finished : public TemplateProcessorEvent {
  bool initialized{false};
};

}  // namespace event
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_EVENT_TEMPLATEPROCESSOR_H_
