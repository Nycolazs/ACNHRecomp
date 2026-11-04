#include <cstdarg>
#include <cstdio>

#include "save_log.h"

namespace nx {

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

}  // namespace nx
