#include <cassert>
#include <iostream>

#include "scarlettx/loader/game_config.h"

int main() {
    const std::string scarlett_xml = R"(
<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="TestStudio.ScarlettGame" Publisher="CN=Test" Version="1.2.3.4" />
  <TitleId>74B7FFFC</TitleId>
  <ShellVisuals DefaultDisplayName="Test Scarlett Title" />
  <ExecutableList>
    <Executable Name="Game.exe" Id="Game" TargetDeviceFamilyForXboxOne="XboxGen9" />
  </ExecutableList>
</Game>
)";

    scarlettx::GameConfig cfg;
    auto st = scarlettx::ParseGameConfigXml(scarlett_xml, cfg);
    assert(st.ok());
    assert(cfg.IsNativeScarlett());
    assert(cfg.title_id == "74B7FFFC");
    assert(cfg.identity_name == "TestStudio.ScarlettGame");
    assert(cfg.PrimaryExecutable() != nullptr);
    assert(cfg.PrimaryExecutable()->name == "Game.exe");

    const std::string gen9_aware_xml = R"(
<Game configVersion="0">
  <Identity Name="TestStudio.OldGame" Publisher="CN=Test" Version="1.0.0.0" />
  <ExecutableList>
    <Executable Name="OldGame.exe" TargetDeviceFamilyForXboxOne="XboxOneGen9Aware" />
  </ExecutableList>
</Game>
)";
    scarlettx::GameConfig old_cfg;
    st = scarlettx::ParseGameConfigXml(gen9_aware_xml, old_cfg);
    assert(st.ok());
    assert(!old_cfg.IsNativeScarlett());
    assert(old_cfg.console_target == scarlettx::ConsoleTarget::XboxOneGen9Aware);

    scarlettx::GameConfig bad_cfg;
    st = scarlettx::ParseGameConfigXml("<NotAGame/>", bad_cfg);
    assert(!st.ok());

    std::cout << "[test_game_config] PASSED\n";
    return 0;
}
