#include "acnh_data.h"

namespace rc {

// games/acnh/versions.txt and hooks*.txt (embedded at build time)
const std::map<std::string, std::string>& acnh_data() {
    static const std::map<std::string, std::string> m = {
#include "acnh_data.inc"
    };
    return m;
}

}  // namespace rc
