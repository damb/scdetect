#ifndef SCDETECT_APPS_CC_PROCESSING_UTIL_H_
#define SCDETECT_APPS_CC_PROCESSING_UTIL_H_

#include <string>

#include "../def.h"
#include "exception.h"

namespace Seiscomp {
namespace detect {
namespace processing {

template <typename T>
std::unique_ptr<Filter<T>> createFilter(const std::string &filter) {
  std::string err;
  std::unique_ptr<Filter<T>> ret{Filter<T>::Create(filter, &err)};
  if (!ret) {
    throw BaseException{"failed to compile filter (" + filter + "): " + err};
  }
  return ret;
}

}  // namespace processing
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_PROCESSING_UTIL_H_
