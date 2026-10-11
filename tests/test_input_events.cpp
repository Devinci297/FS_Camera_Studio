#include "test.hpp"
#include "input_registration.hpp"
#include "input_router.hpp"

#include <limits>
#include <optional>
#include <vector>

using namespace fscs::host;
namespace {
// Models the SDK contract: release callbacks do not exist unless UpEventID was
// supplied. Hat down callbacks carry the raw angle, not the optional DownValue.
struct RegisteredHat {
    std::string definition;
    std::uint32_t down = 0, up = std::numeric_limits<std::uint32_t>::max();
    bool map(const std::string& input, std::uint32_t downId, std::uint32_t,
             std::uint32_t upId = std::numeric_limits<std::uint32_t>::max(), std::uint32_t = 0) {
        definition = input; down = downId; up = upId; return true;
    }
    std::optional<std::pair<std::uint32_t, std::uint32_t>> release() const {
        if (up == std::numeric_limits<std::uint32_t>::max()) return {};
        return std::pair{up, 0u}; // SDK release payload zero must not mean hat-up.
    }
};
}

TEST_CASE("input SDK: runtime test and capture POV mappings request explicit release") {
    // IDs mirror the three disjoint adapter ranges; all use the same mapping
    // helper rather than assuming a neutral packet that the SDK never promised.
    for (const auto& ids : std::vector<std::pair<std::uint32_t,std::uint32_t>>{{1003,4003},{900,901},{10032,10033}}) {
        RegisteredHat sdk;
        CHECK(registerPovInput([&](const auto& definition, auto down, auto downValue, auto up, auto upValue) {
            return sdk.map(definition, down, downValue, up, upValue);
        }, "joystick:5:POV:0", ids.first, ids.second));
        CHECK_EQ(sdk.definition,std::string("joystick:5:POV:0"));
        CHECK_EQ(sdk.down,ids.first);CHECK_EQ(sdk.up,ids.second);CHECK(sdk.down!=sdk.up);
        const auto released=sdk.release();CHECK(released.has_value());if(!released)continue;
        CHECK_EQ(povInputValue(released->first==sdk.up,released->second),kPovCentered);
        for(const auto angle:{0u,4500u,9000u,13500u,18000u,22500u,27000u,31500u})
            CHECK_EQ(povInputValue(false,angle),angle);
    }
    RegisteredHat missingRelease;
    missingRelease.map("joystick:5:POV:0",10032,0);
    CHECK(!missingRelease.release().has_value());
}

TEST_CASE("input SDK: mapped hat release clears motion and allows the next press") {
    fscs::profiles::InputSpec spec;
    spec.kind=fscs::profiles::InputKind::Joystick;spec.definition="joystick:5:POV:0";
    spec.pov=fscs::profiles::PovDirection::Up;
    InputRouter router;router.setBindings({{fscs::profiles::Action::MoveUp,0,spec}});
    RegisteredHat sdk;
    registerPovInput([&](const auto& definition, auto down, auto downValue, auto up, auto upValue) {
        return sdk.map(definition,down,downValue,up,upValue);
    },spec.definition,1000,4000);
    const auto down=router.onInput(0,true,povInputValue(false,0));
    CHECK_EQ(down.size(),std::size_t{1});CHECK_NEAR(router.manualInput().moveUp,1,1e-9);
    const auto release=sdk.release();CHECK(release.has_value());if(!release)return;
    const auto up=router.onInput(0,true,povInputValue(release->first==sdk.up,release->second));
    CHECK_EQ(up.size(),std::size_t{1});CHECK_NEAR(router.manualInput().moveUp,0,1e-9);
    CHECK_EQ(router.onInput(0,true,povInputValue(false,0)).size(),std::size_t{1});
    CHECK_EQ(povInputValue(true,9000),kPovCentered); // Release event identity is authoritative.
}
