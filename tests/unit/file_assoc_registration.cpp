// Dedicated process redirects HKCU to a disposable key. No production association changes.
#include "../../apps/neo_editor/platform/file_assoc.cpp"
#include <iostream>

int main() {
#if defined(_WIN32)
    using namespace neo::fileassoc;
    const std::wstring sandboxPath = L"Software\\EuiEditsAssociationTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY sandbox = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sandboxPath.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &sandbox, nullptr) != ERROR_SUCCESS) return 2;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) != ERROR_SUCCESS) {
        RegCloseKey(sandbox); RegDeleteTreeW(HKEY_CURRENT_USER, sandboxPath.c_str()); return 2;
    }
    int fails = 0;
    auto check = [&](bool ok, const char* why) { if (!ok) { std::cerr << why << '\n'; ++fails; } };
    check(neo::filetypes::detect(".gitignore").category==neo::filetypes::Category::Unknown &&
          neo::filetypes::known(".gitignore") && neo::filetypes::known("C:/repo/README"),
          "known special plain-text names use generic category but remain recognized");
    check(!neo::filetypes::known(".unknown") && !neo::filetypes::associationEligible(".unknown"),
          "unknown extension is neither a known vault file nor an association candidate");
    for(const auto* ext:{"py","pyw","rb","lua","sh","bash","js","mjs","cjs","bat","cmd","ps1"})
        check(neo::filetypes::associationEligible(ext), "recognized script type is available as a user-selected Open With candidate");
    check(neo::filetypes::associationEligible("txt") && neo::filetypes::associationEligible("cpp") &&
          neo::filetypes::associationEligible("json"),
          "ordinary text/source/data file types remain association eligible");
    check(!neo::filetypes::associationEligible("madeup") &&
          std::find_if(neo::filetypes::types().begin(),neo::filetypes::types().end(),[](const auto& t){return std::string(t.extension)=="py";})!=neo::filetypes::types().end(),
          "unrecognized extension is rejected while known script types remain selectable");
    check(std::all_of(neo::filetypes::associationTypes().begin(),neo::filetypes::associationTypes().end(),[](const auto& t){return neo::filetypes::associationEligible(t.extension);}) &&
          std::find_if(neo::filetypes::associationTypes().begin(),neo::filetypes::associationTypes().end(),[](const auto& t){return std::string(t.extension)=="bat";})!=neo::filetypes::associationTypes().end(),
          "central association type list includes recognized scripts");
    check(!isRegistered() && !hasRegistrationEntries(), "empty hive must report unregistered");
    const auto exe = currentExePath();
    check(writeApplicationCapabilities(exe), "one operation must register commands, capabilities and icons");
    check(isRegistered() && hasRegistrationEntries(), "complete registration must be ready");
    const auto initialSelection=registeredExtensions();
    check(initialSelection.size()==2 && marked("txt") && marked("md"), "initial association contains only TXT and MD");
    check(!marked("bat") && !marked("py") && !marked("ps1"), "default registration does not add scripts");
    std::wstring value;
    check(readStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\EUIEdits.MarkdownDocument\\DefaultIcon", nullptr, value)
          && value == L"\"" + exe + L"\",-103", "MD icon resource must match current exe");
    check(writeApplicationCapabilities(exe) && isRegistered(), "register must be idempotent");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\EUIEdits.TextDocument\\shell\\open\\command", nullptr, L"stale.exe %1");
    check(!isRegistered() && hasRegistrationEntries(), "stale path must offer repair rather than unregistered");
    check(writeApplicationCapabilities(exe) && isRegistered(), "repair must restore executable path");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\EUIEdits.MarkdownDocument\\DefaultIcon", nullptr, L"stale.ico");
    check(!isRegistered(), "stale icons must require repair");
    check(writeApplicationCapabilities(exe) && isRegistered(), "repair must restore icons");
    for (auto txt : {DefaultApp::Unknown, DefaultApp::EUIEdits, DefaultApp::Other, DefaultApp::None})
        for (auto md : {DefaultApp::Unknown, DefaultApp::EUIEdits, DefaultApp::Other, DefaultApp::None}) {
            check(writeApplicationCapabilities(exe), "matrix fixture registration must succeed");
            const bool blocked = txt == DefaultApp::Unknown || md == DefaultApp::Unknown ||
                                 txt == DefaultApp::EUIEdits || md == DefaultApp::EUIEdits;
            std::string error;
            const bool removed = unregisterWithStatus({txt, md}, error);
            check(blocked ? !removed && !error.empty() && isRegistered()
                          : removed && !hasRegistrationEntries(),
                  "default/unknown must preserve entries; other/no default must permit cleanup");
        }
    check(writeApplicationCapabilities(exe), "restore registration after default-state matrix");
    // 更名残留：一次成功的注册必须顺手清掉旧 NeoEditor.* 标识，相邻键不动。
    writeStringValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"NeoEditor", L"Software\\NeoEditor\\Capabilities");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\NeoEditor.Document\\shell\\open\\command", nullptr, L"old.exe %1");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\NeoEditor\\Capabilities\\FileAssociations", L".txt", L"NeoEditor.Document");
    check(writeApplicationCapabilities(exe) && isRegistered(), "re-register over legacy fixture must succeed");
    check(!readStringValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"NeoEditor", value) &&
          !readStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\NeoEditor.Document\\shell\\open\\command", nullptr, value) &&
          !readStringValue(HKEY_CURRENT_USER, L"Software\\NeoEditor\\Capabilities\\FileAssociations", L".txt", value),
          "legacy NeoEditor naming must be removed by a fresh registration");
    // Shared/adjacent keys and unrelated applications must survive cleanup.
    writeStringValue(HKEY_CURRENT_USER, L"Software\\NeoEditor\\Preferences", L"keep", L"yes");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"OtherApp", L"OtherCapabilities");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\.txt\\OpenWithProgids", L"Other.Document", L"");
    std::string error;
    check(unregisterWithStatus({DefaultApp::Other, DefaultApp::Other}, error), "known other defaults permit one-click cleanup");
    check(!isRegistered() && !hasRegistrationEntries(), "cleanup must remove all owned registration entries");
    check(readStringValue(HKEY_CURRENT_USER, L"Software\\NeoEditor\\Preferences", L"keep", value) && value == L"yes", "cleanup must preserve adjacent keys outside the owned Capabilities tree");
    check(readStringValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"OtherApp", value), "cleanup must preserve other registered applications");
    check(readStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\.txt\\OpenWithProgids", L"Other.Document", value), "cleanup must preserve other open-with entries");
    check(unregisterWithStatus({DefaultApp::Other, DefaultApp::Other}, error), "cleanup must be idempotent");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\.md\\OpenWithProgids", L"EUIEdits.MarkdownDocument", L"");
    check(!isRegistered() && hasRegistrationEntries(), "orphan marker must still be detected");
    check(unregisterWithStatus({DefaultApp::Other, DefaultApp::Other}, error) && !hasRegistrationEntries(), "orphan marker must be removable");
    // Explicit per-type selection and guarded removal share the production path.
    auto allStatuses=[] {
        std::vector<TypeStatus> statuses;
        for(const auto& t:neo::filetypes::types()) statuses.push_back({t.extension,marked(t.extension),DefaultApp::Other});
        return statuses;
    };
    check(applyWithStatuses({"txt","md","json","py","bat","cmd","ps1","ini"},allStatuses()).ok,
          "explicit selection registers Python, BAT/CMD and PowerShell scripts");
    check(marked("py") && marked("bat") && marked("cmd") && marked("ps1") && registeredExtensions().size()==8,
          "script selections are recorded as EUI-Edits open-with options");
    writeStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\EUIEdits.CodeDocument\\shell\\open\\command", nullptr, L"stale.exe %1");
    check(!isRegistered(), "stale script registration must offer repair");
    auto scriptSelection=registeredExtensions();
    check(applyWithStatuses(scriptSelection,allStatuses()).ok && isRegistered() && marked("py") && marked("bat") && marked("cmd") && marked("ps1"),
          "repair retains every explicitly selected script");
    check(!applyWithStatuses({"txt","md","json","madeup"},allStatuses()).ok && registeredExtensions().size()==8,
          "invalid selection is rejected before registry mutation");
    check(applyWithStatuses({"txt","md","json","cpp","ini"},allStatuses()).ok,"selected types register together");
    check(registeredExtensions().size()==5 && isRegistered(),"selection owns only five extension markers");
    check(readStringValue(HKEY_CURRENT_USER,L"Software\\Classes\\EUIEdits.CodeDocument\\DefaultIcon",nullptr,value) && value==L"\""+exe+L"\",-104","code family binds resource 104");
    check(readStringValue(HKEY_CURRENT_USER,L"Software\\Classes\\EUIEdits.DataDocument\\DefaultIcon",nullptr,value) && value==L"\""+exe+L"\",-105","data family binds resource 105");
    auto active=allStatuses();for(auto& t:active) if(t.extension=="cpp") t.defaultApp=DefaultApp::EUIEdits;
    check(!applyWithStatuses({"txt","md","json","ini"},active).ok && registeredExtensions().size()==5,"selected removal must not orphan a source default");
    auto orphanDefault=allStatuses();for(auto& t:orphanDefault) if(t.extension=="cpp") {t.registered=false;t.defaultApp=DefaultApp::EUIEdits;}
    check(!applyWithStatuses({"txt","md","json","ini"},orphanDefault).ok && registeredExtensions().size()==5,"missing marker must not bypass effective-default protection");
    active=allStatuses();for(auto& t:active) if(t.extension=="json") t.defaultApp=DefaultApp::Unknown;
    check(!applyWithStatuses({"txt","md","cpp","ini"},active).ok && registeredExtensions().size()==5,"unknown effective default must block removed type");
    check(!applyWithStatuses({"../../bad"},allStatuses()).ok && registeredExtensions().size()==5,"invalid selection is rejected before mutation");
    writeStringValue(HKEY_CURRENT_USER,L"Software\\Classes\\.py\\OpenWithProgids",L"Python.File",L"");
    writeStringValue(HKEY_CURRENT_USER,L"Software\\Classes\\.py\\OpenWithProgids",L"EUIEdits.CodeDocument",L"");
    writeStringValue(HKEY_CURRENT_USER,L"Software\\EUI-Edits\\Capabilities\\FileAssociations",L".py",L"EUIEdits.CodeDocument");
    auto legacy=allStatuses();for(auto& t:legacy) if(t.extension=="py") t.registered=true;
    check(applyWithStatuses({"txt","md","json","ini"},legacy).ok,"known other default allows targeted removal of a previously selected script marker");
    check(!marked("py"),"apply clears a deselected EUI-Edits-owned script registration");
    check(readStringValue(HKEY_CURRENT_USER,L"Software\\Classes\\.py\\OpenWithProgids",L"Python.File",value),"script removal preserves Python's open-with entry");
    check(applyWithStatuses({},allStatuses()).ok && !hasRegistrationEntries(),"all families and selected markers clean up in sandbox");
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(sandbox);
    check(RegDeleteTreeW(HKEY_CURRENT_USER, sandboxPath.c_str()) == ERROR_SUCCESS, "disposable hive must be cleaned");
    std::cout << "association sandbox checks, failures=" << fails << '\n';
    return fails ? 1 : 0;
#else
    return 0;
#endif
}
