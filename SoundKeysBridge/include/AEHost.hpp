#pragma once

#include "Protocol.hpp"

#include "AEConfig.h"
#include "AE_GeneralPlug.h"
#include "SPBasic.h"

#include <functional>

namespace skb {

struct RunResult {
    std::string inventoryJson;
    std::string keysJson;
    std::string route;
};

RunResult applySoundKeys(
    SPBasicSuite* basic,
    AEGP_PluginID id,
    const Job& job,
    const Profile& profile,
    const std::function<void(const std::string&)>& phase);

}  // namespace skb
