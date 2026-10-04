#pragma once

#include <string>
#include <vector>

// 用户选择已识别的文件类型：登记 Windows 可选打开方式，查询默认应用，或清理登记。
// 脚本类型也可由用户选择登记；初始选择仍仅包含 TXT/MD。
//
// 注册全程走用户级注册表（HKCU），不需要管理员权限。Windows 8 之后只有用户
// 能在系统设置里更改默认应用；IApplicationAssociationRegistration 仅支持查询。
// 应用能力声明（Capabilities + RegisteredApplications）让它出现在默认应用列表里。

namespace neo::fileassoc {

struct RegisterOutcome {
    bool ok = false;              // 注册本身（ProgId/能力声明/打开方式列表）是否成功
    bool defaultsApplied = false; // 查询确认 .txt 和 .md 当前都默认用 EUI-Edits
    std::string error;            // ok == false 时的说明
};

// 当前是否已注册为可选打开方式。只在设置页打开/操作后调用，别每帧扫注册表。
bool isRegistered();
// Partial/stale registrations are still removable and offered for repair.
bool hasRegistrationEntries();

enum class DefaultApp { Unknown, EUIEdits, Other, None };
struct TypeStatus {
    std::string extension;
    bool registered = false;
    DefaultApp defaultApp = DefaultApp::Unknown;
};
std::vector<std::string> registeredExtensions();
// 查询全量已识别类型，包括不可新登记的脚本，以保护默认应用并清理旧版本遗留项。
std::vector<TypeStatus> queryTypes();
RegisterOutcome applySelection(const std::vector<std::string>& extensions);
struct DefaultStatus {
    DefaultApp txt = DefaultApp::Unknown;
    DefaultApp md = DefaultApp::Unknown;
};

// 向 Windows 查询实际生效的默认应用，按扩展名分别返回。Unknown 表示查询失败，
// 不能当成"不是默认"。非 Windows 平台返回两个 Unknown。
DefaultStatus queryDefaultStatus();

// 兼容入口：加入 TXT/MD 并保留既有选择；不更改 Windows 默认应用。
RegisterOutcome registerAsDefault();

// 打开 Windows 默认应用设置。新系统优先定位到 EUI-Edits，旧系统回退到通用页。
bool openDefaultAppsSettings();

// 取消注册：删掉能力声明、ProgId 与"打开方式"条目。仍是默认应用时要求用户
// 先在系统设置里改选其他程序，避免删除当前默认关联后留下悬空的 ProgId。
bool unregister(std::string& error);

} // namespace neo::fileassoc
