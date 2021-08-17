#ifndef SCDETECT_APPS_SCDETECT_UTILS_H_
#define SCDETECT_APPS_SCDETECT_UTILS_H_

#include <seiscomp/core/defs.h>
#include <seiscomp/datamodel/waveformstreamid.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Seiscomp {
namespace detect {
namespace utils {

const std::string createUUID();
void replaceEscapedXMLFilterIdChars(std::string &filter_id);

template <typename T>
bool isGeZero(const T num) {
  return 0 <= num;
}

template <typename TMap>
auto map_keys(const TMap &map) -> std::vector<typename TMap::key_type> {
  std::vector<typename TMap::key_type> retval;
  for (const auto &pair : map) retval.push_back(pair.first);

  return retval;
}

template <typename TMap>
auto map_values(const TMap &map) -> std::vector<typename TMap::mapped_type> {
  std::vector<typename TMap::mapped_type> retval;
  for (const auto &pair : map) retval.push_back(pair.second);

  return retval;
}

template <typename TEnum>
auto asInteger(const TEnum value) ->
    typename std::underlying_type<TEnum>::type {
  return static_cast<typename std::underlying_type<TEnum>::type>(value);
}

// Provide C++11 make_unique<T>()
template <typename T, typename... Ts>
std::unique_ptr<T> make_unique(Ts &&...params) {
  return std::unique_ptr<T>(new T(std::forward<Ts>(params)...));
}

template <typename T, typename... Ts>
typename Core::SmartPointer<T>::Impl make_smart(Ts &&...params) {
  return
      typename Core::SmartPointer<T>::Impl(new T(std::forward<Ts>(params)...));
}

template <typename TMap, typename Predicate>
std::vector<typename TMap::key_type> filterKeys(const TMap &m, Predicate &p) {
  std::vector<typename TMap::key_type> retval;
  for (const auto &pair : m) {
    if (p(pair)) {
      retval.push_back(pair.first);
    }
  }
  return retval;
}

// Compute the mean value of `samples` using a cumulative moving average
// algorithm.
template <typename T>
double cma(T *samples, size_t n) {
  double cma{0};
  // cummulative moving average for samples a_0, ..., a_n:
  //
  // mean_n = mean_{n-1} + (a_n - mean_{n-1}) / n
  //
  for (size_t i = 0; i < n; ++i) {
    cma += (samples[i] - cma) / (i + 1);
  }
  return cma;
}

// Returns `true` if the difference between two floating point numbers is
// smaller than epsilon, else `false`
template <typename TFloatingPoint>
bool almostEqual(TFloatingPoint lhs, TFloatingPoint rhs,
                 TFloatingPoint epsilon) {
  // The IEEE standard says that any comparison operation involving
  // a NAN must return false.
  if (std::isnan(lhs) || std::isnan(rhs)) {
    return false;
  }

  // From Knuth - The Art of Computer Programming
  return std::abs(rhs - lhs) <=
         std::max(std::abs(lhs), std::abs(rhs)) * epsilon;
}

// Returns `true` if `lhs` is greater than `rhs` under consideration of an
// accuracy of `epsilon`. If `lhs` is smaller than `rhs`, `false` is returned.
template <typename TFloatingPoint>
bool greaterThan(TFloatingPoint lhs, TFloatingPoint rhs,
                 TFloatingPoint epsilon) {
  // The IEEE standard says that any comparison operation involving
  // a NAN must return false.
  if (std::isnan(lhs) || std::isnan(rhs)) {
    return false;
  }

  // From Knuth - The Art of Computer Programming
  return (lhs - rhs) > std::max(std::abs(lhs), std::abs(rhs)) * epsilon;
}

// Returns `true` if `lhs` is smaller than `rhs` under consideration of an
// accuracy of `epsilon`. If `lhs` is smaller than `rhs`, `false` is returned.
template <typename TFloatingPoint>
bool lessThan(TFloatingPoint lhs, TFloatingPoint rhs, TFloatingPoint epsilon) {
  // The IEEE standard says that any comparison operation involving
  // a NAN must return false.
  if (std::isnan(lhs) || std::isnan(rhs)) {
    return false;
  }

  // From Knuth - The Art of Computer Programming
  return (rhs - lhs) > std::max(std::abs(lhs), std::abs(rhs)) * epsilon;
}

/* ------------------------------------------------------------------------- */
class WaveformStreamID {
 public:
  explicit WaveformStreamID(const std::string &netStaLocCha);
  explicit WaveformStreamID(const DataModel::WaveformStreamID &id);
  WaveformStreamID(const std::string &netCode, const std::string &staCode,
                   const std::string &locCode, const std::string &chaCode);

  // Returns the network code
  const std::string &netCode() const;
  // Returns the station code
  const std::string &staCode() const;
  // Returns the location code
  const std::string &locCode() const;
  // Returns the channel code
  const std::string &chaCode() const;

  std::string sensorLocationId() const;

  // Returns `true` if the waveform stream identifier is valid, `false`
  // otherwise.
  bool isValid() const;

  friend std::ostream &operator<<(std::ostream &os, const WaveformStreamID &id);

 protected:
  static const std::string _delimiter;

 private:
  std::string _netCode;
  std::string _staCode;
  std::string _locCode;
  std::string _chaCode;
};

}  // namespace utils
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_SCDETECT_UTILS_H_
