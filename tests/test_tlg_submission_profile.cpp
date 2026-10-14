// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/windows/gow3-base/gpu/shim/tlg_submission_profile.h"
static int Set(const char* name,const char* value) {
#ifdef _WIN32
    return _putenv_s(name,value?value:"");
#else
    return value?setenv(name,value,1):unsetenv(name);
#endif
}
int main() {
    using namespace TlgSubmissionProfile;
    for(auto name:options) if(Set(name,nullptr)) return 1;
    if(Apply(nullptr) || Apply("OTHER")) return 1;
    for(auto name:options) if(std::getenv(name)) return 1;
    if(Apply("CUSA03627")) return 1;
    for(auto name:options) if(!std::getenv(name) || std::strcmp(std::getenv(name),"0")) return 1;
    if(Set(options[0],"1") || Set(options[1],"4") || Apply("CUSA03627")) return 1;
    if(std::strcmp(std::getenv(options[0]),"1") || std::strcmp(std::getenv(options[1]),"4")) return 1;
    if(Set(options[0],"") || Apply("CUSA03627") || std::strcmp(std::getenv(options[0]),"0")) return 1;
    return 0;
}
