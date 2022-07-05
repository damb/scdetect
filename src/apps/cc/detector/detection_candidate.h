#ifndef SCDETECT_APPS_CC_DETECTOR_DETECTIONCANDIDATE_H_
#define SCDETECT_APPS_CC_DETECTOR_DETECTIONCANDIDATE_H_

#include <vector>

#include "detail.h"
#include "linker/association.h"

namespace Seiscomp {
namespace detect {
namespace detector {

using DetectionCandidate = linker::Association;

struct TemplateResultInfo {
  DetectionCandidate::TemplateResult templateResult;
  detail::ProcessorIdType templateProcessorId;
};

std::vector<TemplateResultInfo> sortContributorsByArrivalTime(
    const DetectionCandidate &candidate);

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_DETECTIONCANDIDATE_H_
