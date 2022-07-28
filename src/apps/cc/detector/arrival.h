#ifndef SCDETECT_APPS_CC_DETECTOR_ARRIVAL_H_
#define SCDETECT_APPS_CC_DETECTOR_ARRIVAL_H_

#include <seiscomp/core/datetime.h>

#include <boost/functional/hash.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <string>

namespace Seiscomp {
namespace detect {
namespace detector {

// A detector pick
struct Pick {
  // The pick's time
  Core::Time time;
  // The pick's waveform stream identifier
  std::string waveformStreamId;

  // The tentative phase
  boost::optional<std::string> phaseHint;

  // The pick offset w.r.t. origin time
  Core::TimeSpan offset;

  // Lower uncertainty w.r.t. the pick time
  boost::optional<double> lowerUncertainty;
  // Upper uncertainty w.r.t. the pick time
  boost::optional<double> upperUncertainty;

  friend bool operator==(const Pick &lhs, const Pick &rhs);
  friend bool operator!=(const Pick &lhs, const Pick &rhs);
};

// A detector arrival
struct Arrival {
  Arrival(const Pick &pick, const std::string &phase,
          boost::optional<double> weight = boost::none);

  // The associated pick
  Pick pick;
  // The associated phase code
  std::string phase;
  // The arrival weight
  boost::optional<double> weight;

  friend bool operator==(const Arrival &lhs, const Arrival &rhs);
  friend bool operator!=(const Arrival &lhs, const Arrival &rhs);
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

namespace std {

template <>
struct hash<Seiscomp::detect::detector::Pick> {
  std::size_t operator()(
      const Seiscomp::detect::detector::Pick &p) const noexcept {
    std::size_t ret{0};
    boost::hash_combine(ret, std::hash<std::string>{}(p.time.iso()));
    boost::hash_combine(ret, std::hash<std::string>{}(p.waveformStreamId));
    if (p.phaseHint) {
      boost::hash_combine(ret, std::hash<std::string>{}(*p.phaseHint));
    }
    boost::hash_combine(ret, std::hash<double>{}(p.offset.length()));
    if (p.lowerUncertainty) {
      boost::hash_combine(ret, std::hash<double>{}(*p.lowerUncertainty));
    }
    if (p.upperUncertainty) {
      boost::hash_combine(ret, std::hash<double>{}(*p.upperUncertainty));
    }
    return ret;
  }
};

template <>
struct hash<Seiscomp::detect::detector::Arrival> {
  std::size_t operator()(
      const Seiscomp::detect::detector::Arrival &a) const noexcept {
    std::size_t ret{0};
    boost::hash_combine(ret,
                        std::hash<Seiscomp::detect::detector::Pick>{}(a.pick));
    boost::hash_combine(ret, std::hash<std::string>{}(a.phase));
    if (a.weight) {
      boost::hash_combine(ret, std::hash<double>{}(*a.weight));
    }
    return ret;
  }
};

}  // namespace std

#endif  // SCDETECT_APPS_CC_DETECTOR_ARRIVAL_H_
