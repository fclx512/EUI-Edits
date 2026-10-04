#include "model/i18n.h"
#include "platform/file_assoc.h"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
// 顺序要紧：windows.h 必须最先（同 native_dialogs.cpp）。
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif
#endif // _WIN32

#include <string>
#include <vector>

#include "model/file_types.h"
#include "model/text_file.h"
#include <algorithm>
#include <map>

namespace neo::fileassoc {
namespace {
#if defined(_WIN32)
constexpr wchar_t kAppName[]=L"EUI-Edits";
// 更名（NeoEditor → EUI-Edits）前的旧标识：注册或清理时一并移除，只碰列出的这些名字。
constexpr wchar_t kLegacyAppName[]=L"NeoEditor";
constexpr wchar_t kLegacyCapabilitiesSubkey[]=L"Software\\NeoEditor\\Capabilities";
const wchar_t* const kLegacyProgIds[]={
    L"NeoEditor.Document",L"NeoEditor.TextDocument",L"NeoEditor.MarkdownDocument",
    L"NeoEditor.CodeDocument",L"NeoEditor.DataDocument"};
constexpr wchar_t kCapabilitiesSubkey[]=L"Software\\EUI-Edits\\Capabilities";
struct Family {const wchar_t* id;const char* labelId;int icon;};
const std::vector<Family>& families() {
    static const std::vector<Family> value={
        {L"EUIEdits.Document","assoc.document",106},
        {L"EUIEdits.TextDocument","assoc.text_document",102},
        {L"EUIEdits.MarkdownDocument","assoc.markdown_document",103},
        {L"EUIEdits.CodeDocument","assoc.code_document",104},
        {L"EUIEdits.DataDocument","assoc.data_document",105}};
    return value;
}
std::wstring wide(const std::string& value) {return {value.begin(),value.end()};}
std::wstring currentExePath() {
    std::vector<wchar_t> value(32768);DWORD n=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    return n && n<value.size()?std::wstring(value.data(),n):std::wstring{};
}
std::wstring openCommandFor(const std::wstring& path) {return L"\""+path+L"\" \"%1\"";}
bool ours(const std::wstring& id) {for(const auto& f:families()) if(CompareStringOrdinal(id.c_str(),-1,f.id,-1,TRUE)==CSTR_EQUAL) return true;return false;}
const Family& family(const std::string& ext) {
    const auto kind=filetypes::detect("file."+ext).category;
    return families()[kind==filetypes::Category::Markdown?2:kind==filetypes::Category::Code?3:kind==filetypes::Category::Data?4:1];
}
bool writeStringValue(HKEY root,const wchar_t* subkey,const wchar_t* name,const wchar_t* data) {
    HKEY key=nullptr;if(RegCreateKeyExW(root,subkey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return false;
    const auto result=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(data),static_cast<DWORD>((wcslen(data)+1)*sizeof(wchar_t)));
    RegCloseKey(key);return result==ERROR_SUCCESS;
}
bool readStringValue(HKEY root,const wchar_t* subkey,const wchar_t* name,std::wstring& out) {
    HKEY key=nullptr;if(RegOpenKeyExW(root,subkey,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS) return false;
    DWORD bytes=0,type=0;auto result=RegQueryValueExW(key,name,nullptr,&type,nullptr,&bytes);
    std::vector<wchar_t> data(bytes/sizeof(wchar_t)+1,0);
    if(result==ERROR_SUCCESS && type==REG_SZ) result=RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(data.data()),&bytes);
    RegCloseKey(key);if(result!=ERROR_SUCCESS || type!=REG_SZ) return false;out=data.data();return true;
}
bool deleteValue(HKEY root,const wchar_t* subkey,const wchar_t* name) {
    HKEY key=nullptr;auto result=RegOpenKeyExW(root,subkey,0,KEY_SET_VALUE,&key);
    if(result!=ERROR_SUCCESS) return result==ERROR_FILE_NOT_FOUND;
    result=RegDeleteValueW(key,name);RegCloseKey(key);return result==ERROR_SUCCESS || result==ERROR_FILE_NOT_FOUND;
}
std::wstring openWithKey(const std::string& ext) {return L"Software\\Classes\\."+wide(ext)+L"\\OpenWithProgids";}
std::wstring associationsKey() {return std::wstring(kCapabilitiesSubkey)+L"\\FileAssociations";}
bool marked(const std::string& ext) {
    std::wstring value;for(const auto& f:families()) if(readStringValue(HKEY_CURRENT_USER,openWithKey(ext).c_str(),f.id,value)) return true;
    return readStringValue(HKEY_CURRENT_USER,associationsKey().c_str(),(L"."+wide(ext)).c_str(),value) && ours(value);
}
DefaultApp queryDefault(IApplicationAssociationRegistration* registration,const std::string& ext) {
    if(!registration) return DefaultApp::Unknown;
    LPWSTR id=nullptr;auto result=registration->QueryCurrentDefault((L"."+wide(ext)).c_str(),AT_FILEEXTENSION,AL_EFFECTIVE,&id);
    if(FAILED(result)||!id) {CoTaskMemFree(id);return result==HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION)?DefaultApp::None:DefaultApp::Unknown;}
    const bool own=ours(id);CoTaskMemFree(id);return own?DefaultApp::EUIEdits:DefaultApp::Other;
}
std::vector<TypeStatus> queryAll() {
    std::vector<TypeStatus> result;const auto init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IApplicationAssociationRegistration* registration=nullptr;
    if(SUCCEEDED(init)||init==RPC_E_CHANGED_MODE) CoCreateInstance(CLSID_ApplicationAssociationRegistration,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&registration));
    for(const auto& t:filetypes::types()) result.push_back({t.extension,marked(t.extension),queryDefault(registration,t.extension)});
    if(registration) registration->Release();if(SUCCEEDED(init)) CoUninitialize();return result;
}
void notifyShellAssociationsChanged() {SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);}
bool clearExtension(const std::string& ext) {
    bool ok=true;for(const auto& f:families()) ok=deleteValue(HKEY_CURRENT_USER,openWithKey(ext).c_str(),f.id)&&ok;
    // Capabilities is our private key; never delete another application's associations.
    ok=deleteValue(HKEY_CURRENT_USER,associationsKey().c_str(),(L"."+wide(ext)).c_str())&&ok;return ok;
}
// 更名前的 NeoEditor.* 登记残留：旧 ProgID 类键、注册表应用名与能力声明键。
// 只删这里列出的自有名字；Software\NeoEditor 下的相邻键（如 Preferences）不动。
bool removeLegacyNamingEntries() {
    bool ok=deleteValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",kLegacyAppName);
    for(const auto* id:kLegacyProgIds) {
        const auto r=RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\")+id).c_str());
        ok=(r==ERROR_SUCCESS||r==ERROR_FILE_NOT_FOUND)&&ok;
    }
    const auto r=RegDeleteTreeW(HKEY_CURRENT_USER,kLegacyCapabilitiesSubkey);
    return (r==ERROR_SUCCESS||r==ERROR_FILE_NOT_FOUND)&&ok;
}
bool removeRegistrationEntries() {
    bool ok=deleteValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",kAppName);
    for(const auto& t:filetypes::types()) ok=clearExtension(t.extension)&&ok;
    for(const auto& f:families()) {const auto r=RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\")+f.id).c_str());ok=(r==ERROR_SUCCESS||r==ERROR_FILE_NOT_FOUND)&&ok;}
    const auto r=RegDeleteTreeW(HKEY_CURRENT_USER,kCapabilitiesSubkey);ok=(r==ERROR_SUCCESS||r==ERROR_FILE_NOT_FOUND)&&ok;
    return removeLegacyNamingEntries()&&ok;
}
bool writeApplicationCapabilities(const std::wstring& exe,const std::vector<std::string>& selected={"txt","md"}) {
    for(const auto& ext:selected) if(!filetypes::associationEligible(ext)) return false;
    const auto command=openCommandFor(exe);
    for(const auto& f:families()) {
        const auto key=std::wstring(L"Software\\Classes\\")+f.id;
        const auto icon=L"\""+exe+L"\",-"+std::to_wstring(f.icon);
        const std::wstring label=textfile::pathFromUtf8(i18n::tr(f.labelId)).wstring();
        if(!writeStringValue(HKEY_CURRENT_USER,key.c_str(),nullptr,label.c_str())||
           !writeStringValue(HKEY_CURRENT_USER,key.c_str(),L"FriendlyTypeName",label.c_str())||
           !writeStringValue(HKEY_CURRENT_USER,(key+L"\\shell\\open\\command").c_str(),nullptr,command.c_str())||
           !writeStringValue(HKEY_CURRENT_USER,(key+L"\\DefaultIcon").c_str(),nullptr,icon.c_str())) return false;
    }
    // Reconcile every recognized type with the explicit selection, preserving selected scripts
    // during repair and clearing only NeoEditor-owned markers for deselected types.
    for(const auto& t:filetypes::types()) {
        const std::string ext=t.extension;
        if(std::find(selected.begin(),selected.end(),ext)==selected.end()) {if(!clearExtension(ext)) return false;continue;}
        const auto& f=family(ext);
        if(!writeStringValue(HKEY_CURRENT_USER,openWithKey(ext).c_str(),f.id,L"")||
           !writeStringValue(HKEY_CURRENT_USER,associationsKey().c_str(),(L"."+wide(ext)).c_str(),f.id)) return false;
    }
    return writeStringValue(HKEY_CURRENT_USER,kCapabilitiesSubkey,L"ApplicationName",kAppName)&&
        writeStringValue(HKEY_CURRENT_USER,kCapabilitiesSubkey,L"ApplicationDescription",textfile::pathFromUtf8(i18n::tr("assoc.description")).wstring().c_str())&&
        writeStringValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",kAppName,kCapabilitiesSubkey)&&
        removeLegacyNamingEntries();
}
bool registeredCapabilitiesPresent() {
    std::wstring value;const auto exe=currentExePath();if(exe.empty()) return false;
    if(!readStringValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",kAppName,value)||value!=kCapabilitiesSubkey) return false;
    bool any=false;
    for(const auto& t:filetypes::associationTypes()) {
        if(!marked(t.extension)) continue;any=true;const auto& f=family(t.extension);
        const auto key=std::wstring(L"Software\\Classes\\")+f.id;
        if(!readStringValue(HKEY_CURRENT_USER,associationsKey().c_str(),(L"."+wide(t.extension)).c_str(),value)||value!=f.id||
           !readStringValue(HKEY_CURRENT_USER,openWithKey(t.extension).c_str(),f.id,value)||
           !readStringValue(HKEY_CURRENT_USER,(key+L"\\shell\\open\\command").c_str(),nullptr,value)||value!=openCommandFor(exe)||
           !readStringValue(HKEY_CURRENT_USER,(key+L"\\DefaultIcon").c_str(),nullptr,value)||value!=L"\""+exe+L"\",-"+std::to_wstring(f.icon)) return false;
    }
    return any;
}
bool unregisterWithStatus(const DefaultStatus& status,std::string& error) {
    if(status.txt==DefaultApp::EUIEdits || status.md==DefaultApp::EUIEdits || status.txt==DefaultApp::Unknown || status.md==DefaultApp::Unknown) {
        error=i18n::tr("assoc.default_protected");return false;
    }
    const bool ok=removeRegistrationEntries();notifyShellAssociationsChanged();if(!ok) error=i18n::tr("assoc.cleanup_partial");return ok;
}
RegisterOutcome applyWithStatuses(const std::vector<std::string>& requested,const std::vector<TypeStatus>& statuses) {
    RegisterOutcome result;std::vector<std::string> selected;
    for(const auto& ext:requested) {
        bool valid=false;for(const auto& t:filetypes::types()) if(ext==t.extension) valid=true;
        if(!valid) {result.error=i18n::tr("assoc.unknown_type");return result;}
        if(!filetypes::associationEligible(ext)) {result.error=i18n::tr("assoc.unknown_type");return result;}
        if(std::find(selected.begin(),selected.end(),ext)==selected.end()) selected.push_back(ext);
    }
    for(const auto& t:statuses) {
        if(std::find(selected.begin(),selected.end(),t.extension)!=selected.end()) continue;
        if(t.defaultApp==DefaultApp::EUIEdits || ((t.registered || (selected.empty() && hasRegistrationEntries())) && t.defaultApp==DefaultApp::Unknown)) {
            result.error=i18n::format("assoc.extension_protected", {{"extension", t.extension}});return result;
        }
    }
    const auto exe=currentExePath();if(exe.empty()) {result.error=i18n::tr("assoc.exe_path");return result;}
    result.ok=selected.empty()?removeRegistrationEntries():writeApplicationCapabilities(exe,selected);
    notifyShellAssociationsChanged();if(!result.ok) result.error=i18n::tr("assoc.write_partial");return result;
}
#endif
} // namespace
bool isRegistered() {
#if defined(_WIN32)
    return registeredCapabilitiesPresent();
#else
    return false;
#endif
}
bool hasRegistrationEntries() {
#if defined(_WIN32)
    std::wstring value;if(readStringValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",kAppName,value)) return true;
    HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,kCapabilitiesSubkey,0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {RegCloseKey(key);return true;}
    for(const auto& f:families()) if(RegOpenKeyExW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\")+f.id).c_str(),0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {RegCloseKey(key);return true;}
    for(const auto& t:filetypes::types()) if(marked(t.extension)) return true;
#endif
    return false;
}
std::vector<std::string> registeredExtensions() {
    std::vector<std::string> result;
#if defined(_WIN32)
    for(const auto& t:filetypes::associationTypes()) if(marked(t.extension)) result.emplace_back(t.extension);
#endif
    return result;
}
std::vector<TypeStatus> queryTypes() {
#if defined(_WIN32)
    return queryAll();
#else
    std::vector<TypeStatus> result;for(const auto& t:filetypes::types()) result.push_back({t.extension,false,DefaultApp::Unknown});return result;
#endif
}
DefaultStatus queryDefaultStatus() {
    DefaultStatus result;for(const auto& t:queryTypes()) {if(t.extension=="txt") result.txt=t.defaultApp;if(t.extension=="md") result.md=t.defaultApp;}return result;
}
RegisterOutcome applySelection(const std::vector<std::string>& selected) {
#if defined(_WIN32)
    return applyWithStatuses(selected,queryAll());
#else
    return {false,false,i18n::tr("assoc.platform")};
#endif
}
RegisterOutcome registerAsDefault() {auto selected=registeredExtensions();for(const auto* ext:{"txt","md"}) if(std::find(selected.begin(),selected.end(),ext)==selected.end()) selected.emplace_back(ext);return applySelection(selected);}
bool unregister(std::string& error) {auto r=applySelection({});error=r.error;return r.ok;}
bool openDefaultAppsSettings() {
#if defined(_WIN32)
    for(const auto* uri:{L"ms-settings:defaultapps?registeredAppUser=EUI-Edits",L"ms-settings:defaultapps"})
        if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",uri,nullptr,nullptr,SW_SHOWNORMAL))>32) return true;
#endif
    return false;
}
} // namespace neo::fileassoc
