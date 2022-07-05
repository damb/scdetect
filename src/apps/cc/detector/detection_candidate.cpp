#include "detection_candidate.h"

namespace Seiscomp {
namespace detect {
namespace detector {

std::vector<TemplateResultInfo> sortContributorsByArrivalTime(
    const DetectionCandidate &candidate) {
  std::vector<TemplateResultInfo> ret;
  for (const auto &resultPair : candidate.results) {
    ret.push_back(TemplateResultInfo{resultPair.second, resultPair.first});
  }
  std::sort(std::begin(ret), std::end(ret),
            [](const TemplateResultInfo &lhs, const TemplateResultInfo &rhs) {
              return lhs.templateResult.arrival.pick.time <
                     rhs.templateResult.arrival.pick.time;
            });
  return ret;
}

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
