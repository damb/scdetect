#ifndef SCDETECT_APPS_CC_PROCESSING_PROCESSOR_H_
#define SCDETECT_APPS_CC_PROCESSING_PROCESSOR_H_

#include <string>

#include "../log.h"
#include "exception.h"

namespace Seiscomp {
namespace detect {
namespace processing {

#define SCDETECT_LOG_DEBUG_PROCESSOR(processor_ptr, ...) \
  SCDETECT_LOG_DEBUG_TAGGED(processor_ptr->id(), __VA_ARGS__)
#define SCDETECT_LOG_INFO_PROCESSOR(processor_ptr, ...) \
  SCDETECT_LOG_INFO_TAGGED(processor_ptr->id(), __VA_ARGS__)
#define SCDETECT_LOG_WARNING_PROCESSOR(processor_ptr, ...) \
  SCDETECT_LOG_WARNING_TAGGED(processor_ptr->id(), __VA_ARGS__)
#define SCDETECT_LOG_ERROR_PROCESSOR(processor_ptr, ...) \
  SCDETECT_LOG_ERROR_TAGGED(processor_ptr->id(), __VA_ARGS__)
#define SCDETECT_LOG_NOTICE_PROCESSOR(processor_ptr, ...) \
  SCDETECT_LOG_NOTICE_TAGGED(processor_ptr->id(), __VA_ARGS__)
#define SCDETECT_LOG_PROCESSOR(channel, processor_ptr, ...) \
  SCDETECT_LOG_TAGGED(channel, processor_ptr->id(), __VA_ARGS__)

// Abstract interface for processor implementations
class Processor {
 public:
  virtual ~Processor() = default;

  // Base class for all processor related exceptions
  class BaseException : public processing::BaseException {
   public:
    using processing::BaseException::BaseException;
    BaseException();
  };

  // Sets the processor's identifier
  void setId(const std::string &id);
  // Returns the processor's identifier
  const std::string &id() const;

 private:
  std::string _id;
};

}  // namespace processing
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_PROCESSING_PROCESSOR_H_
