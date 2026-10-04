#include "state/app_actions.h"
#include "core/platform/platform.h"
#include "model/attachment.h"
#include "model/session_storage.h"
#include "model/atomic_write.h"
#include "model/png_encode.h"
#include "model/theme_loader.h"
#include "platform/file_assoc.h"
#include "platform/file_operations.h"
#include "platform/vault_rename.h"
#include "platform/clipboard_image.h"
#include "platform/native_dialogs.h"
#include "platform/font_safety.h"
#include "state/session_writer.h"
#include "state/tabs_trace.h"
#include "state/vault_cache.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo {

namespace {

// 新文档默认按平台习惯的换行，Windows 上用 CRLF。
textfile::Document makeEmptyDocument() {
    textfile::Document document;
#if defined(_WIN32)
    document.lineEnding = textfile::LineEnding::CrLf;
#else
    document.lineEnding = textfile::LineEnding::Lf;
#endif
    return document;
}

std::string vaultInitialDirectory(const AppState& state) {
    if (!state.vaultRoot.empty()) {
        return state.vaultRoot;
    }
    return state.path.empty() ? std::string{} : textfile::parentPath(state.path);
}

// 文档库跟随当前文档（2026-09-26 口径）：文档库不是"常驻仓库"，而是"当前文档所在的
// 目录"。打开/切换到某个文档时把侧栏根换到它所在目录；文档库里点开的文件本来就
// 落在根内，这里判为"已在根内"不动 —— 于是"手动选目录后浏览"不会被顺手覆盖。
// 返回 true = 根变了（调用方要重建扫描/展开/列表）。
bool followDocumentDirectory(AppState& state, const std::string& documentPath) {
    if (documentPath.empty()) {
        return false;
    }
    const std::string directory = textfile::parentPath(documentPath);
    if (directory.empty() || directory == state.vaultRoot) {
        return false;
    }
    if (documentWithinRoot(documentPath, state.vaultRoot)) return false;
    state.vaultRoot = directory;
    state.vaultAddress = directory;
    state.vaultScan.reset();
    state.vaultRowsGeneration = 0;
    state.expanded.clear();
    state.filter.clear();
    return true;
}

void updateRecentFiles(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::path normalized = textfile::pathFromUtf8(path);
    normalized = std::filesystem::absolute(normalized, error);
    if (error) {
        error.clear();
        normalized = textfile::pathFromUtf8(path);
    }
    normalized = normalized.lexically_normal();
    std::string utf8Path = textfile::pathToUtf8(normalized);
    auto& files = settings::current().recentFiles;
#if defined(_WIN32)
    std::string comparable = utf8Path;
    std::transform(comparable.begin(), comparable.end(), comparable.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    files.erase(std::remove_if(files.begin(), files.end(), [&](const std::string& existing) {
                    std::string folded = existing;
                    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    return folded == comparable;
                }),
                files.end());
#else
    files.erase(std::remove(files.begin(), files.end(), utf8Path), files.end());
#endif
    files.insert(files.begin(), std::move(utf8Path));
    if (files.size() > 8) {
        files.resize(8);
    }
}

} // namespace

void recordRecentFile(const std::string& path) {
    updateRecentFiles(path);
    settings::flush();
}

void showToast(AppState& state, std::string title, std::string message) {
    state.toastTitle = std::move(title);
    state.toastMessage = std::move(message);
    state.toastVisible = true;
    app::requestUpdate();
}

void newDocument(AppState& state) {
    if (!createDocumentTab(state)) return;
    state.newDocumentLanguage = state.pendingNewMarkdown ? "markdown" : "text";
    state.pendingNewMarkdown = false; state.languageOverride.clear(); state.wrapOverride = -1;
    state.doc = makeEmptyDocument();
    state.path.clear();
    state.diskFingerprint = {};
    state.suppressRecovery = false;
    ++state.revision;
    state.savedRevision = state.revision;
    state.recovered = false;
    state.recoveredFrom.clear();
    state.editorStateDirty = true;
    state.statsRevision = static_cast<unsigned long long>(-1);
    settings::current().lastFile.clear();
    settings::flush();
    persistDocumentSession(state);
}

bool loadDocument(AppState& state, const std::string& path) {
    clearTabCloseAnchor(state);
    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (document && sameDocumentFile(document->path, path)) return activateDocumentTab(state, id);
    }
    auto loaded = filesafety::load(path);
    textfile::LoadResult& result = loaded.result;
    if (!result.ok) {
        showToast(state, i18n::tr("safety.open_failed"), result.error);
        return false;
    }

    // Reuse only the initial pristine blank tab. Every edited or explicit new tab survives.
    if (!(state.tabOrder.size() == 1 && state.tabId == 1 && state.path.empty() && !state.dirty() && state.doc.text.empty()) &&
        !createDocumentTab(state)) return false;
    state.languageOverride.clear(); state.wrapOverride = -1;
    state.doc = std::move(result.document);
    state.path = path;
    state.diskFingerprint = loaded.fingerprint;
    state.suppressRecovery = false;
    ++state.revision;
    state.savedRevision = state.revision;
    state.recovered = false;
    state.recoveredFrom.clear();
    state.editorStateDirty = true;
    state.statsRevision = static_cast<unsigned long long>(-1);
    settings::current().lastFile = path;
    recordRecentFile(path);
    // 文档库跟随当前文档：换到它所在目录（已在根内则不动）。放在最后 —— 根变了
    // 随后必须重扫，否则上一份目录树还挂在旧根上（展开项、行都是旧根的）。
    if (followDocumentDirectory(state, path) | mergeRelatedVaultRoots(state)) {
        refreshVault(state, true);
    }
    const auto relative = vaultRelativePath(state, path);
    if (!relative.empty() && documentWithinRoot(path, state.vaultRoot)) {
        for (std::size_t i = 0; i < relative.size(); ++i)
            if (relative[i] == '/') state.expanded.insert(relative.substr(0, i));
        state.vaultSelectedPath = relative;
        state.vaultPendingReveal = relative;
        rebuildRows(state);
    }
    // 打开文件：该页的库根可能刚被跟随逻辑改写，重解算展示分组。
    refreshTabPresentation(state);
    persistDocumentSession(state);
    return true;
}

void retryUnavailableDocument(AppState& state) {    if (!state.unavailable || documentModalOpen(state)) return;
    auto loaded = filesafety::load(state.path);
    if (!loaded.result.ok) { state.loadError = loaded.result.error; showToast(state, i18n::tr("safety.open_failed"), state.loadError); return; }
    state.doc = std::move(loaded.result.document); state.diskFingerprint = loaded.fingerprint;
    state.unavailable = false; state.loadError.clear();
    ++state.revision; state.savedRevision = state.revision; state.editorStateDirty = true;
    state.editorMemory.reset(); state.statsRevision = static_cast<unsigned long long>(-1);
    persistDocumentSession(state); app::requestUpdate();
}

void restoreStartupDocument(AppState& state, const std::string& commandLinePath) {
    std::vector<sessionstorage::ReadRecord> records;
    std::uint64_t active = 0;
    const bool storageAvailable = !settings::configDirectory().empty();
    if (storageAvailable && !sessionstorage::read(records, active)) {
        // Preserve malformed recovery evidence; no automatic rewrite/cleanup can erase it.
        state.sessionStorageBlocked = true;
        showToast(state, i18n::tr("safety.open_failed"), i18n::tr("tabs.recovery_failed"));
    }
    // Recovery is for uncommitted drafts, not reopening a previous workspace.
    // An explicitly requested clean file may retain its old ordering beside
    // recovered drafts; every other clean record is ignored, including v1 data.
    records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& record) {
        return !record.dirty && (commandLinePath.empty() || record.path.empty() ||
                                !sameDocumentFile(record.path, commandLinePath));
    }), records.end());
    if (!records.empty()) {
        state.tabOrder.clear(); state.inactiveTabs.clear();
        for (auto& record : records) {
            // A recovered draft may have been discarded beside its original
            // file. Restore one clean buffer per physical file, while retaining
            // every dirty recovery record as an independent unsaved manuscript.
            if (!record.dirty && !record.path.empty()) {
                const auto duplicate = std::find_if(state.inactiveTabs.begin(), state.inactiveTabs.end(),
                    [&](const auto& entry) { return sameDocumentFile(entry.second.path, record.path); });
                if (duplicate != state.inactiveTabs.end()) {
                    if (active == record.id) active = duplicate->first;
                    continue;
                }
            }
            DocumentSession document;
            document.tabId = record.id; document.vaultRoot = record.vaultRoot;
            document.vaultAddress = record.vaultRoot; document.wrapOverride = record.wrapOverride;
            document.newDocumentLanguage = record.language.empty() ? "text" : record.language;
            if (record.dirty) {
                document.doc = std::move(record.document);
                document.recovered = true; document.recoveredFrom = record.path;
                document.revision = 1;
            } else if (!record.path.empty()) {
                auto loaded = filesafety::load(record.path);
                if (!loaded.result.ok) {
                    showToast(state, i18n::tr("safety.open_failed"), loaded.result.error);
                    document.path = record.path;
                    document.unavailable = true; document.loadError = loaded.result.error;
                } else {
                    document.doc = std::move(loaded.result.document);
                    document.path = record.path; document.diskFingerprint = loaded.fingerprint;
                }
                document.languageOverride = record.language;
            } // Clean blank pages keep the platform's new-document defaults.
            document.editorStateDirty = true;
            state.nextTabId = std::max(state.nextTabId, record.id + 1);
            state.tabOrder.push_back(record.id);
            state.inactiveTabs.emplace(record.id, std::move(document));
        }
        if (!state.tabOrder.empty()) {
            if (!state.inactiveTabs.count(active)) active = state.tabOrder.front();
            static_cast<DocumentSession&>(state) = std::move(state.inactiveTabs.at(active));
            state.inactiveTabs.erase(active);
        } else { state.tabOrder.push_back(1); static_cast<DocumentSession&>(state) = DocumentSession{}; }
    } else {
        settings::RecoverySnapshot snapshot;
        if (settings::readRecovery(snapshot)) {
            state.doc = std::move(snapshot.doc); state.doc.text = std::move(snapshot.text);
            state.path.clear(); state.diskFingerprint = {};
            state.recovered = true; state.recoveredFrom = std::move(snapshot.originPath);
            state.newDocumentLanguage = snapshot.language.empty() ? filetypes::detect(state.recoveredFrom).language : snapshot.language;
            ++state.revision; state.savedRevision = state.revision - 1; state.editorStateDirty = true;
            // Migration commits every recovered manuscript before removing the legacy copy.
            persistDocumentSession(state);
        }
    }
    if (!commandLinePath.empty()) {
        loadDocument(state, commandLinePath);
    }
    // 初始化/恢复：建立展示分组（首帧绘制即可显示正确库色）。
    refreshTabPresentation(state);
}

void openFileFromDialog(AppState& state) {
    eui::platform::FileDialogOptions options;
    options.prompt = i18n::tr("safety.open_text");
    options.allowedExtensions = filetypes::extensions();
    options.filterName = i18n::tr("safety.text_filter");
    options.initialDirectory = vaultInitialDirectory(state);

    const eui::platform::FileDialogResult result = eui::platform::openFileDialog(options);
    if (result.status == eui::platform::FileDialogStatus::Cancelled) {
        return;
    }
    if (!result.selected()) {
        showToast(state, i18n::tr("safety.open_failed"), result.error.empty() ? i18n::tr("safety.dialog_empty") : result.error);
        return;
    }
    loadDocument(state, result.paths.front());
}

void openRecentFile(AppState& state, std::size_t index) {
    const auto& recent = settings::current().recentFiles;
    if (index >= recent.size()) {
        return;
    }
    const std::string path = recent[index];
    std::error_code error;
    if (!std::filesystem::is_regular_file(textfile::pathFromUtf8(path), error) || error) {
        showToast(state, i18n::tr("safety.recent_unavailable"), i18n::format("safety.recent_missing", {{"path", path}}));
        return;
    }
    requestOpenPath(state, path);
}

namespace {
void finishPending(AppState& state) {
    const auto action = state.pending;
    state.pending = PendingAction::None;
    state.continueAfterSave = false;
    performPending(state, action);
    if (action != PendingAction::CloseWindow && !state.settingsOpen) state.findEditorFocusPending = true;
}
void cancelPending(AppState& state) {
    state.pending = PendingAction::None;
    state.pendingNewMarkdown = false;
    state.confirmationTabId = 0;
    state.continueAfterSave = false;
    state.exitClosing = false;
    if (!state.settingsOpen) state.findEditorFocusPending = true;
}
void showConflict(AppState& state, const std::string& path, const filesafety::Fingerprint& observed) {
    state.conflictPath = path;
    state.conflictFingerprint = observed;
    state.saveConflictOpen = true;
    state.confirmationTabId = state.tabId;
    state.safetyChoice = 2;
    app::requestUpdate();
}
bool saveToPath(AppState& state, const std::string& path, const filesafety::Fingerprint& expected) {
    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (id != state.tabId && document && sameDocumentFile(document->path, path)) {
            showToast(state, i18n::tr("safety.save_failed"), i18n::tr("tabs.already_open_save")); return false;
        }
    }
    if (expected.status != filesafety::DiskStatus::Present && expected.status != filesafety::DiskStatus::Missing) {
        showToast(state, i18n::tr("safety.save_failed"), i18n::tr("safety.inspect_failed"));
        return false;
    }
    auto current = filesafety::inspect(path);
    if (current.status == filesafety::DiskStatus::Error) {
        showToast(state, i18n::tr("safety.save_failed"), current.error); return false;
    }
    if (!filesafety::same(current, expected)) { showConflict(state, path, current); return false; }
    bool guardRejected = false;
    std::string error;
    if (!textfile::save(path, state.doc, error, [&] {
        current = filesafety::inspect(path);
        guardRejected = !filesafety::same(expected, current);
        return !guardRejected;
    })) {
        if (guardRejected && current.status != filesafety::DiskStatus::Error) showConflict(state, path, current);
        else showToast(state, i18n::tr("safety.save_failed"), guardRejected ? current.error : error);
        return false;
    }
    state.path = path;
    // Do not baseline a competing post-save write as our own saved version.
    const textfile::ForcedEncoding savedEncoding{state.doc.encoding, state.doc.ansiCodePage};
    auto verified = filesafety::load(path, &savedEncoding);
    const auto& verifiedDoc = verified.result.document;
    const bool sameSavedVersion = verified.result.ok && verifiedDoc.text == state.doc.text &&
        verifiedDoc.hadBom == state.doc.hadBom && verifiedDoc.encoding == state.doc.encoding &&
        (state.doc.encoding != textfile::Encoding::Ansi || verifiedDoc.ansiCodePage == state.doc.ansiCodePage) &&
        (state.doc.text.find('\n') == std::string::npos || verifiedDoc.lineEnding == state.doc.lineEnding);
    state.diskFingerprint = sameSavedVersion ? verified.fingerprint : filesafety::Fingerprint{};
    state.savedRevision = state.revision;
    state.recovered = false;
    state.recoveredFrom.clear();
    state.suppressRecovery = false;
    settings::current().lastFile = path;
    recordRecentFile(path);
    if (followDocumentDirectory(state, path) | mergeRelatedVaultRoots(state)) refreshVault(state, true);
    persistDocumentSession(state);
    showToast(state, i18n::tr("safety.saved"), textfile::fileName(path));
    return true;
}
} // namespace

bool saveDocument(AppState& state) {
    if (state.unavailable) { showToast(state, i18n::tr("safety.open_failed"), state.loadError); return false; }
    if (state.path.empty()) return saveDocumentAs(state);
    if (!state.dirty()) {
        showToast(state, i18n::tr("safety.no_save"), i18n::tr("safety.unchanged")); return true;
    }
    const auto current = filesafety::inspect(state.path);
    if (current.status == filesafety::DiskStatus::Error) {
        showToast(state, i18n::tr("safety.save_failed"), current.error); return false;
    }
    if (!filesafety::same(state.diskFingerprint, current)) {
        showConflict(state, state.path, current); return false;
    }
    return saveToPath(state, state.path, current);
}

bool requestCloseDocument(AppState& state) {
    if (state.closeApproved) return true;
    if (state.sessionClosePending) return false; // repeated WM_CLOSE must not reopen the editable tree
    if (documentModalOpen(state)) return false;
    state.settingsOpen = false;
    state.exitClosing = true;
    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (!document || !document->dirty() || document->suppressRecovery) continue;
        activateDocumentTabInternal(state, id);
        state.pending = PendingAction::CloseWindow; state.confirmationTabId = id;
        state.safetyChoice = 2; state.openMenu = MenuKind::None; state.tabListOpen = false;
        app::requestUpdate(); return false;
    }
    state.exitClosing = false;
    if (state.sessionStorageBlocked) {
        // Startup could not validate this recovery store. No writes are accepted
        // for it, and normal closing must not erase the only remaining manuscript
        // evidence. Keep the existing recovery-error notice and files for repair.
        settings::current().lastFile.clear();
        settings::current().vault.clear();
        state.closeApproved = true;
        app::requestClose();
        return true;
    }
    state.sessionClosePending = true;
    state.recoveryPendingWrite = false;
    // Stop accepting writes, drain the one already running, then remove owned
    // records. The event loop remains alive; never wait for a commit inside WM_CLOSE.
    const bool started = sessionwriter::beginDiscardOnClose([&state](bool ok) {
        state.sessionClosePending = false;
        if (ok) {
            settings::current().lastFile.clear();
            settings::current().vault.clear();
            settings::clearRecovery();
            state.closeApproved = true;
            app::requestClose();
        } else {
            for (const auto id : state.tabOrder)
                if (auto* document = documentTab(state, id)) document->suppressRecovery = false;
            persistDocumentSession(state);
            showToast(state, i18n::tr("safety.close_failed"), i18n::tr("safety.close_cleanup_failed"));
            app::requestUpdate();
        }
    });
    if (!started) {
        state.sessionClosePending = false;
        showToast(state, i18n::tr("safety.close_failed"), i18n::tr("safety.close_cleanup_failed"));
    }
    app::requestUpdate();
    return state.closeApproved; // inline tests can complete before returning
}

void resolveDocumentConfirmation(AppState& state, filesafety::UnsavedChoice choice) {
    if (state.pending == PendingAction::None || state.saveConflictOpen ||
        (state.confirmationTabId && state.confirmationTabId != state.tabId)) return;
    if (choice == filesafety::UnsavedChoice::Cancel) {
        state.exitClosing = false;
        for (const auto id : state.tabOrder) if (auto* document = documentTab(state, id)) document->suppressRecovery = false;
        cancelPending(state); persistDocumentSession(state); return;
    }
    state.continueAfterSave = choice == filesafety::UnsavedChoice::Save;
    filesafety::resolveUnsaved(choice,
        [&] { return saveDocument(state); },
        [&] {
            if (state.pending == PendingAction::CloseWindow || state.pending == PendingAction::CloseTab)
                state.suppressRecovery = true;
        }, [&] { finishPending(state); });
    if (!state.saveConflictOpen) state.continueAfterSave = false;
    app::requestUpdate();
}

void resolveSaveConflict(AppState& state, int choice) {
    if (!state.saveConflictOpen || (state.confirmationTabId && state.confirmationTabId != state.tabId)) return;
    const auto path = state.conflictPath;
    const auto expected = state.conflictFingerprint;
    const bool continueAction = state.continueAfterSave;
    state.saveConflictOpen = false;
    bool saved = false;
    if (choice == 0) saved = saveToPath(state, path, expected);
    else if (choice == 1) saved = saveDocumentAs(state);
    else {
        for (const auto id : state.tabOrder) if (auto* document = documentTab(state, id)) document->suppressRecovery = false;
        cancelPending(state); persistDocumentSession(state); app::requestUpdate(); return;
    }
    if (saved && continueAction) finishPending(state);
    else if (!state.saveConflictOpen) {
        state.continueAfterSave = false;
        if (!state.settingsOpen) state.findEditorFocusPending = true;
    }
    app::requestUpdate();
}

// 菜单子项与分发共用一份顺序，避免两处维护同一个编码清单。
const std::vector<ReopenEncodingOption>& reopenEncodingOptions() {
    static std::vector<ReopenEncodingOption> options = {
        {"UTF-8", {textfile::Encoding::Utf8, 0}},
        {"UTF-16 LE", {textfile::Encoding::Utf16Le, 0}},
        {"UTF-16 BE", {textfile::Encoding::Utf16Be, 0}},
#if defined(_WIN32)
        {i18n::tr("safety.encoding_gbk"), {textfile::Encoding::Ansi, 936u}},
        {i18n::tr("safety.encoding_big5"), {textfile::Encoding::Ansi, 950u}},
        {i18n::tr("safety.encoding_sjis"), {textfile::Encoding::Ansi, 932u}},
#endif
    };
#if defined(_WIN32)
    options[3].label = i18n::tr("safety.encoding_gbk");
    options[4].label = i18n::tr("safety.encoding_big5");
    options[5].label = i18n::tr("safety.encoding_sjis");
#endif
    return options;
}

void performReloadWithEncoding(AppState& state, const textfile::ForcedEncoding& forced) {
    // 失败不动当前文档：错误提示完，用户手里的文稿与撤销历史原样保留。
    auto loaded = filesafety::load(state.path, &forced);
    textfile::LoadResult& result = loaded.result;
    if (!result.ok) {
        showToast(state, i18n::tr("safety.reload_failed"), result.error);
        return;
    }
    state.languageOverride.clear(); state.wrapOverride = -1;
    state.doc = std::move(result.document);
    state.diskFingerprint = loaded.fingerprint;
    state.suppressRecovery = false;
    ++state.revision;
    state.savedRevision = state.revision;
    state.recovered = false;
    state.recoveredFrom.clear();
    state.editorStateDirty = true;
    state.statsRevision = static_cast<unsigned long long>(-1);
    persistDocumentSession(state);
}

void requestReloadWithEncoding(AppState& state, std::size_t optionIndex) {
    const auto& options = reopenEncodingOptions();
    if (optionIndex >= options.size()) {
        return;
    }
    if (state.path.empty()) {
        showToast(state, i18n::tr("safety.cannot_reload"), i18n::tr("safety.reload_unsaved"));
        return;
    }
    if (state.dirty()) {
        state.pending = PendingAction::ReloadWithEncoding;
        state.pendingEncoding = options[optionIndex].forced;
        return;
    }
    performReloadWithEncoding(state, options[optionIndex].forced);
}

// ── 图片粘贴落盘（R2，需求 §9.3）────────────────────────────────────────────
// 事件/每帧阶段执行（compose 不能做 IO）：读剪贴板位图 → PNG 内存编码 → 附件
// 目录原子落盘 → 只把最终 Markdown 链接排进 InsertImageLink，由 compose 插入。
// 任何一步失败都保持文档与磁盘原样（或只留提示），不插半截链接。
void pasteImageAsAttachment(AppState& state) {
    state.pendingImagePaste = false;

    // 未落盘的文档先要求另存；取消即放弃本次粘贴（不建目录、不插链接）。
    if (state.path.empty()) {
        saveDocumentAs(state);
        if (state.path.empty()) {
            showToast(state, i18n::tr("safety.cannot_insert_image"), i18n::tr("safety.save_before_image"));
            return;
        }
    }

    attachment::Target target;
    std::string error;
    if (!attachment::resolveTarget(state.path, settings::current().attachmentMode, target, error)) {
        showToast(state, i18n::tr("safety.cannot_insert_image"), error);
        return;
    }

    clipboardimage::ImageData image;
    if (!clipboardimage::capture(image, error)) {
        showToast(state, i18n::tr("safety.image_failed"), error);
        return;
    }

    // PNG 全部在内存编完才动磁盘：编码失败时连附件目录都不创建。
    std::string png;
    if (!pngencode::encodeRgba(image.width, image.height, image.rgba.data(), png, error)) {
        showToast(state, i18n::tr("safety.image_failed"), error);
        return;
    }

    const std::string stem = "image";
    const std::string fileName = attachment::uniqueFileName(target.absoluteDir, stem);

    std::error_code fsError;
    std::filesystem::create_directories(textfile::pathFromUtf8(target.absoluteDir), fsError);
    if (fsError) {
        showToast(state, i18n::tr("safety.image_failed"), i18n::format("safety.attachment_dir", {{"path", target.absoluteDir}}));
        return;
    }

    const std::string separator = target.absoluteDir.back() == '/' ? "" : "/";
    const std::string absoluteFile = target.absoluteDir + separator + fileName;
    if (!atomicwrite::writeFile(textfile::pathFromUtf8(absoluteFile), png)) {
        showToast(state, i18n::tr("safety.image_failed"), i18n::tr("safety.attachment_failed"));
        return;
    }

    // 链接用相对路径（'/' 分隔），目标包 <>：空格/括号可往返（MD4C 与 Obsidian 一致）。
    state.pendingImageLink = attachment::markdownLink(target.relativeDir, fileName);
    state.pendingEditorCommand = EditorCommand::InsertImageLink;
    app::requestUpdate();
    // 附件目录/文件立刻进文档库树（不等 watcher 的 0.8s 防抖）。
    if (!state.vaultRoot.empty()) {
        refreshVault(state, false);
    }
}

bool saveDocumentAs(AppState& state) {
    if (state.unavailable) { showToast(state, i18n::tr("safety.open_failed"), state.loadError); return false; }
    if (state.savePickerOpen) return false;
    state.savePickerOpen = true;
    struct ResetPicker { bool& open; ~ResetPicker() { open = false; } } resetPicker{state.savePickerOpen};
    const std::string initialDirectory = vaultInitialDirectory(state);
    std::string suggestedName = state.path.empty() ? std::string(state.markdownCapable() ? i18n::tr("safety.untitled_md") : i18n::tr("safety.untitled_txt")) : textfile::fileName(state.path);
    if (suggestedName.empty()) {
        suggestedName = i18n::tr("safety.untitled_md");
    }

    const dialogs::PickResult result =
        dialogs::pickSavePath(initialDirectory, suggestedName, state.path.empty() ? std::vector<std::string>{state.markdownCapable() ? "md" : "txt"} : std::vector<std::string>{});
    if (result.cancelled) {
        return false;
    }
    if (!result.ok) {
        showToast(state, i18n::tr("safety.save_as_failed"), result.error);
        return false;
    }

    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (id != state.tabId && document && sameDocumentFile(document->path, result.path)) {
            showToast(state, i18n::tr("safety.save_failed"), i18n::tr("tabs.already_open_save")); return false;
        }
    }
    const auto target = filesafety::inspect(result.path);
    if (target.status == filesafety::DiskStatus::Error) {
        showToast(state, i18n::tr("safety.save_failed"), target.error); return false;
    }
    if (target.status == filesafety::DiskStatus::Present &&
        (result.path != state.path || !filesafety::same(target, state.diskFingerprint))) {
        showConflict(state, result.path, target); return false;
    }
    return saveToPath(state, result.path, target);
}

void rebuildRows(AppState& state) {
    if (!state.vaultScan) {
        state.rows.clear();
        return;
    }
    if (state.filter.empty()) {
        vault::flatten(*state.vaultScan, state.expanded, state.rows);
    } else {
        vault::flattenFiltered(*state.vaultScan, state.filter, state.rows);
    }
}

// 采纳/请求共享扫描快照（阶段 B）。不做 mergeRelatedVaultRoots（那是调用方的职责），
// 因为本函数会被每帧的 tickVaultScan 调用，而合并要做文件系统规范化，不能进每帧路径。
void adoptVaultScan(AppState& state, bool resetScroll) {
    if (state.vaultRoot.empty()) {
        state.vaultScan.reset();
        state.vaultRowsGeneration = 0;
        state.rows.clear();
        return;
    }
    const vaultcache::ScanView view = vaultcache::query(state.vaultRoot);
    if (!view.scan) {
        // 首次没有快照：显示明确加载态（rows 空）并排一次后台扫描，不在主线程阻塞。
        vaultcache::requestScan(state.vaultRoot);
        state.vaultScan.reset();
        state.vaultRowsGeneration = 0;
        state.rows.clear();
        return;
    }
    state.vaultScan = view.scan;
    if (state.vaultRowsGeneration == view.generation) {
        return;  // 同一代次：直接复用快照与该页视图，不重建 rows。
    }
    const bool firstRows = state.vaultRowsGeneration == 0;
    if (firstRows || resetScroll) {
        state.vaultScroll = 0.0f;
    }
    if (firstRows) {
        // 只展开当前打开文档所在的目录链（对齐 Obsidian，与大库首扫口径一致）。
        const std::string docRelative = vaultRelativePath(state, state.path);
        if (!docRelative.empty() && docRelative != "." && docRelative.rfind("..", 0) != 0) {
            for (std::size_t i = 0; i < docRelative.size(); ++i) {
                if (docRelative[i] == '/') {
                    state.expanded.insert(docRelative.substr(0, i));
                }
            }
        }
    }
    state.vaultRowsGeneration = view.generation;
    rebuildRows(state);
    if (!view.scan->warning.empty()) {
        showToast(state, i18n::tr("safety.vault_partial"), view.scan->warning);
    }
}

// 每帧轻量采纳：共享快照代次变了才重建 rows（切页/后台扫描完成后各发生一次）。
void tickVaultScan(AppState& state) {
    if (state.vaultRoot.empty() || !state.vaultScan) {
        return;
    }
    const vaultcache::ScanView view = vaultcache::query(state.vaultRoot);
    if (view.scan && view.generation != state.vaultRowsGeneration) {
        adoptVaultScan(state, false);
    }
}

// 显式请求后台重扫（监听目录事件、附件落盘后的"立刻进树"等）。UI 不等待扫描结束。
void requestVaultRefresh(AppState& state) {
    if (state.vaultRoot.empty()) {
        return;
    }
    vaultcache::requestScan(state.vaultRoot);
}

void refreshVault(AppState& state, bool resetScroll) {
    mergeRelatedVaultRoots(state);
    // 选择库/编辑库地址/重命名/打开文件等显式动作都会走这里：根可能变了，重解算展示分组。
    refreshTabPresentation(state);
    if (state.vaultRoot.empty()) {
        state.vaultScan.reset();
        state.vaultRowsGeneration = 0;
        state.rows.clear();
        return;
    }

    // 用户显式动作（新建/重命名/删除/打开/换根）要求列表立刻反映结果：这里同步扫一次
    // 并发布进共享缓存；切页那条路径走 adoptVaultScan，不再同步扫描。
    tracelog::Span span("vault-scan");
    vaultcache::requestScanSync(state.vaultRoot);
    span.note("root=" + std::to_string(state.vaultRoot.size()));
    const vaultcache::ScanView view = vaultcache::query(state.vaultRoot);
    if (!view.scan) {
        state.rows.clear();
        return;
    }
    state.vaultScan = view.scan;
    state.vaultRowsGeneration = view.generation;
    if (!view.scan->ok) {
        state.rows.clear();
        showToast(state, i18n::tr("safety.vault_failed"), view.scan->error);
        return;
    }

    if (resetScroll) {
        state.vaultScroll = 0.0f;
    }
    // 首次有快照时展开当前文档所在目录链。
    const std::string docRelative = vaultRelativePath(state, state.path);
    if (!docRelative.empty() && docRelative != "." && docRelative.rfind("..", 0) != 0) {
        for (std::size_t i = 0; i < docRelative.size(); ++i) {
            if (docRelative[i] == '/') {
                state.expanded.insert(docRelative.substr(0, i));
            }
        }
    }

    rebuildRows(state);
    if (!view.scan->warning.empty()) {
        showToast(state, i18n::tr("safety.vault_partial"), view.scan->warning);
    }
    vaultcache::prune(state.vaultRoot);
}

void chooseVaultDirectory(AppState& state) {
    const dialogs::PickResult result = dialogs::pickDirectory(state.vaultRoot);
    if (result.cancelled) {
        return;
    }
    if (!result.ok) {
        showToast(state, i18n::tr("safety.folder_pick_failed"), result.error);
        return;
    }

    state.vaultRoot = result.path;
    state.vaultAddress = state.vaultRoot;
    state.vaultScan.reset();
    state.vaultRowsGeneration = 0;
    state.expanded.clear();
    state.filter.clear();
    state.mode = EditorMode::Vault;

    // 手动切目录只改"当前浏览根"，不落盘成常驻库（2026-09-26 口径），也不动当前文档 ——
    // 用户仍要从侧栏点文件才打开。下次打开任何文档时根会被跟随逻辑换走。
    settings::current().mode = static_cast<int>(state.mode);
    settings::flush();

    refreshVault(state, true);
    persistDocumentSession(state);
}

void toggleFolder(AppState& state, const std::string& relative) {
    if (state.expanded.count(relative) > 0) {
        state.expanded.erase(relative);
    } else {
        state.expanded.insert(relative);
    }
    rebuildRows(state);
}

namespace {

// 地址栏里的路径常是从资源管理器"复制为路径"粘来的，带首尾引号和空白。
std::string cleanPastedPath(std::string text) {
    const auto trim = [](std::string value) {
        const std::size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            return std::string{};
        }
        const std::size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    };
    text = trim(std::move(text));
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = trim(text.substr(1, text.size() - 2));
    }
    return text;
}

// 把 relative 路径上的每一级目录都标成展开，目标才会出现在扁平列表里。
// 传目录时末尾加 '/'，这样目录自身也会被展开。
void expandAlongPath(AppState& state, const std::string& relative) {
    for (std::size_t i = 0; i < relative.size(); ++i) {
        if (relative[i] != '/') {
            continue;
        }
        const std::string prefix = relative.substr(0, i);
        if (!prefix.empty()) {
            state.expanded.insert(prefix);
        }
    }
}

} // namespace

void navigateToVaultPath(AppState& state, std::string text) {
    text = cleanPastedPath(std::move(text));
    if (text.empty()) {
        return;
    }

    std::error_code error;
    const std::filesystem::path target = textfile::pathFromUtf8(text);
    const bool directory = std::filesystem::is_directory(target, error);
    error.clear();
    const bool file = !directory && std::filesystem::is_regular_file(target, error);
    if (!directory && !file) {
        showToast(state, i18n::tr("safety.path_missing"), text);
        return;
    }

    // 目标在当前文档库之内：展开到那一层就行，不动根（从根往里跳是常见操作，
    // 直接换根会把上面几层弄丢）。文件顺带打开。
    if (!state.vaultRoot.empty()) {
        const std::string relative = vaultRelativePath(state, textfile::pathToUtf8(target));
        const bool inside = !relative.empty() && relative != "." && relative.rfind("..", 0) != 0;
        if (inside) {
            state.filter.clear();
            expandAlongPath(state, directory ? relative + "/" : relative);
            state.vaultPendingReveal = relative;
            rebuildRows(state);
            if (file) {
                requestOpenPath(state, textfile::pathToUtf8(target));
            }
            app::requestUpdate();
            return;
        }
    }

    // 目标在库外：目录直接换根；文件则换到它所在目录，再把文件打开。
    const std::filesystem::path root = directory ? target : target.parent_path();
    state.vaultRoot = textfile::pathToUtf8(root);
    state.vaultAddress = state.vaultRoot;
    state.vaultScan.reset();
    state.vaultRowsGeneration = 0;
    state.expanded.clear();
    state.filter.clear();
    state.mode = EditorMode::Vault;
    refreshVault(state, true);
    if (file) {
        requestOpenPath(state, textfile::pathToUtf8(target));
    }
    app::requestUpdate();
}

void requestOpenPath(AppState& state, const std::string& path) {
    if (path.empty() || documentModalOpen(state)) return;
    loadDocument(state, path);
}

// ── 文档库条目操作（2026-09-26，右键菜单）────────────────────────────────────
namespace {

// 名称只能是一段文件名：剥掉首尾空白；带路径分隔符的一律拒绝（避免"新建"
// 变成越权写到库外）。
std::string cleanEntryName(std::string name, bool& ok) {
    const std::size_t begin = name.find_first_not_of(" \t\r\n");
    const std::size_t end = name.find_last_not_of(" \t\r\n");
    name = begin == std::string::npos ? std::string{} : name.substr(begin, end - begin + 1);
    ok = !name.empty() && name.find_first_of("/\\") == std::string::npos && name != "." &&
         name != "..";
    return name;
}

std::string joinRelative(const std::string& parent, const std::string& name) {
    return parent.empty() ? name : parent + "/" + name;
}

std::string parentRelativeOf(const std::string& relative) {
    const std::size_t slash = relative.find_last_of('/');
    return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
}

// 刷新列表、展开到目标并把目标行滚进视野（新建/重命名成功后的公共收尾）。
void revealEntry(AppState& state, const std::string& relative, bool isDir) {
    state.filter.clear();
    expandAlongPath(state, isDir ? relative + "/" : relative);
    state.vaultPendingReveal = relative;
    rebuildRows(state);
    app::requestUpdate();
}

// 打开文档是否受这次文件系统操作影响（正好是它，或它所在的目录被删/改名）。
bool pathAffected(const DocumentSession& state, const std::string& changedAbsolute) {
    if (state.path.empty() || changedAbsolute.empty()) {
        return false;
    }
    std::error_code error;
    const std::filesystem::path current =
        std::filesystem::absolute(textfile::pathFromUtf8(state.path), error).lexically_normal();
    if (error) return false;
    const std::filesystem::path changed =
        std::filesystem::absolute(textfile::pathFromUtf8(changedAbsolute), error).lexically_normal();
    if (error) return false;
    auto currentPart = current.begin();
    for (auto changedPart = changed.begin(); changedPart != changed.end();
         ++changedPart, ++currentPart) {
        if (currentPart == current.end()) return false;
#if defined(_WIN32)
        const std::wstring left = currentPart->native();
        const std::wstring right = changedPart->native();
        if (::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                   right.data(), static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) {
            return false;
        }
#else
        if (*currentPart != *changedPart) return false;
#endif
    }
    return true;
}

bool pathDefinitelyMissing(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(textfile::pathFromUtf8(path), error);
    return !error && !exists;
}

bool preserveDeletedDocumentAsUnsaved(AppState& state, bool clearPath) {
    if (clearPath && !state.path.empty()) {
        // recoveryOrigin() uses this after path becomes empty; retain the deleted
        // document's origin so the emergency copy remains attributable.
        state.recoveredFrom = state.path;
        state.path.clear();
        state.diskFingerprint = {};
    }
    if (!state.dirty()) {
        // Text and undo history are unchanged; only make the existing document
        // participate in the normal save/recovery flow.
        state.savedRevision = state.revision == 0 ? 1 : state.revision - 1;
    }
    return persistDocumentSession(state);
}

} // namespace

void vaultCreateFile(AppState& state, const std::string& parentRelative, std::string name) {
    bool ok = false;
    name = cleanEntryName(std::move(name), ok);
    if (!ok) {
        showToast(state, i18n::tr("safety.cannot_create"), i18n::tr("safety.invalid_name"));
        return;
    }
    const std::string relative = joinRelative(parentRelative, name);
    const std::filesystem::path target = textfile::pathFromUtf8(vaultAbsolutePath(state, relative));
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
        showToast(state, i18n::tr("safety.cannot_create"), i18n::format("safety.name_exists", {{"name", name}}));
        return;
    }
    // 空文档落盘走 textfile::save：BOM/换行口径与打开保存的其余路径一致。
    textfile::Document empty;
    empty.lineEnding = textfile::LineEnding::Lf;
    std::string saveError;
    if (!textfile::save(textfile::pathToUtf8(target), empty, saveError)) {
        showToast(state, i18n::tr("safety.create_failed"), saveError);
        return;
    }
    refreshVault(state, false);
    revealEntry(state, relative, false);
    requestOpenPath(state, textfile::pathToUtf8(target));
}

void vaultCreateFolder(AppState& state, const std::string& parentRelative, std::string name) {
    bool ok = false;
    name = cleanEntryName(std::move(name), ok);
    if (!ok) {
        showToast(state, i18n::tr("safety.cannot_create"), i18n::tr("safety.invalid_name"));
        return;
    }
    const std::string relative = joinRelative(parentRelative, name);
    const std::filesystem::path target = textfile::pathFromUtf8(vaultAbsolutePath(state, relative));
    std::error_code error;
    if (!std::filesystem::create_directory(target, error) || error) {
        showToast(state, i18n::tr("safety.create_failed"),
                  error ? error.message() : i18n::format("safety.name_exists", {{"name", name}}));
        return;
    }
    refreshVault(state, false);
    revealEntry(state, relative, true);
    showToast(state, i18n::tr("safety.folder_created"), name);
}

void beginVaultRename(AppState& state, const std::string& relative, bool isDirectory, bool returnToRow) {
    if (documentModalOpen(state) || relative.empty()) return;
    state.vaultContextPath = relative;
    state.vaultContextIsDir = isDirectory;
    state.vaultSelectedPath = relative;
    state.vaultPromptKind = VaultPromptKind::Rename;
    state.vaultPromptText = textfile::fileName(relative);
    state.vaultPromptError.clear();
    state.vaultRenameSuggestedName.clear();
    state.vaultRenameExtensionApprovalName.clear();
    state.vaultPromptFocusPending = true;
    state.vaultPromptScroll = 0;
    state.vaultRenameReturnToRow = returnToRow;
    app::requestUpdate();
}

void cancelVaultPrompt(AppState& state) {
    state.vaultPromptKind = VaultPromptKind::None;
    state.vaultPromptError.clear();
    state.vaultRenameSuggestedName.clear();
    state.vaultRenameExtensionApprovalName.clear();
    if (state.vaultRenameReturnToRow) state.vaultFocusRestorePath = state.vaultSelectedPath;
    else if (!state.settingsOpen) state.findEditorFocusPending = true;
    state.vaultPromptFocusPending = false;
    app::requestUpdate();
}

bool confirmVaultRename(AppState& state, bool useSuggestedName) {
    if (state.vaultPromptKind != VaultPromptKind::Rename) return false;
    const std::string name = useSuggestedName ? state.vaultRenameSuggestedName : state.vaultPromptText;
    if (name.empty() && useSuggestedName) return false;
    if (useSuggestedName) state.vaultPromptText = name;
    const bool consent = state.vaultRenameExtensionApprovalName == name;
    if (!vaultRenameEntry(state, state.vaultContextPath, name, consent)) {
        app::requestUpdate(); return false;
    }
    cancelVaultPrompt(state);
    return true;
}

bool vaultRenameEntry(AppState& state, const std::string& oldRelative, std::string newName,
                      bool allowExtensionChange) {
    state.vaultRenameSuggestedName.clear();
    const auto invalid = platform::validateRenameName(newName);
    if (!invalid.empty()) {
        state.vaultPromptError = invalid;
        showToast(state, i18n::tr("safety.cannot_rename"), invalid);
        return false;
    }
    const auto oldPath = textfile::pathFromUtf8(oldRelative);
    std::error_code typeError;
    const bool oldIsDirectory = std::filesystem::is_directory(
        std::filesystem::symlink_status(textfile::pathFromUtf8(vaultAbsolutePath(state, oldRelative)), typeError));
    if (!oldIsDirectory && textfile::extensionLower(oldRelative) != textfile::extensionLower(newName) &&
        !allowExtensionChange) {
        state.vaultPromptError = i18n::tr("rename.extension_warning");
        state.vaultRenameExtensionApprovalName = newName;
        return false;
    }
    const auto result = platform::renameVaultEntry(state.vaultRoot, oldRelative, newName);
    if (result.status != platform::RenameStatus::Success) {
        state.vaultPromptError = result.message;
        state.vaultRenameSuggestedName = result.suggestedName;
        showToast(state, i18n::tr("safety.rename_failed"), result.message);
        return false;
    }
    state.vaultPromptError.clear();
    state.vaultRenameExtensionApprovalName.clear();
    const std::string newRelative = joinRelative(parentRelativeOf(oldRelative), newName);
    const std::filesystem::path target = textfile::pathFromUtf8(vaultAbsolutePath(state, newRelative));
    std::error_code error;
    // 打开的文档正好被改名：路径悄悄跟上，内容与未保存状态都不动。
    const std::string oldAbsolute = vaultAbsolutePath(state, oldRelative);
    const std::string newAbsolute = vaultAbsolutePath(state, newRelative);
    if (pathAffected(state, oldAbsolute)) {
        const auto priorOpenedPath = state.path;
        // Include documents inside a renamed folder, including case-varied paths.
        const auto oldNative = textfile::pathFromUtf8(oldAbsolute).lexically_normal();
        const auto currentNative = textfile::pathFromUtf8(state.path).lexically_normal();
        auto tail = currentNative.begin();
        for (auto part = oldNative.begin(); part != oldNative.end() && tail != currentNative.end(); ++part) ++tail;
        auto movedPath = textfile::pathFromUtf8(newAbsolute);
        for (; tail != currentNative.end(); ++tail) movedPath /= *tail;
        state.path = textfile::pathToUtf8(movedPath);
        // Rename preserves the opened version: never rebaseline external edits.
        const auto moved = filesafety::inspect(state.path);
        if (!filesafety::same(state.diskFingerprint, moved)) state.diskFingerprint.status = filesafety::DiskStatus::Unknown;
        settings::current().lastFile = state.path;
        auto& recent = settings::current().recentFiles;
        recent.erase(std::remove(recent.begin(), recent.end(), priorOpenedPath), recent.end());
        recordRecentFile(state.path);
        if (state.dirty()) persistDocumentSession(state);
    }
    for (const auto id : state.tabOrder) {
        if (id == state.tabId) continue;
        auto* document = documentTab(state, id);
        if (!document) continue;
        const auto priorRoot = document->vaultRoot;
        if (pathAffected(*document, oldAbsolute)) {
            const auto current = textfile::pathFromUtf8(document->path).lexically_normal();
            const auto oldNative = textfile::pathFromUtf8(oldAbsolute).lexically_normal();
            auto tail = current.begin();
            for (auto part = oldNative.begin(); part != oldNative.end() && tail != current.end(); ++part) ++tail;
            auto movedPath = textfile::pathFromUtf8(newAbsolute);
            for (; tail != current.end(); ++tail) movedPath /= *tail;
            document->path = textfile::pathToUtf8(movedPath);
            const auto observed = filesafety::inspect(document->path);
            if (!filesafety::same(document->diskFingerprint, observed)) document->diskFingerprint.status = filesafety::DiskStatus::Unknown;
        }
        if (!priorRoot.empty() && documentWithinRoot(priorRoot, oldAbsolute)) {
            std::error_code relativeError;
            auto tail = std::filesystem::relative(textfile::pathFromUtf8(priorRoot), textfile::pathFromUtf8(oldAbsolute), relativeError);
            if (!relativeError) {
                document->vaultRoot = textfile::pathToUtf8((textfile::pathFromUtf8(newAbsolute) / tail).lexically_normal());
                document->vaultAddress = document->vaultRoot;
            }
        }
        document->vaultScan.reset(); document->rows.clear(); document->vaultRowsGeneration = 0;
    }
    mergeRelatedVaultRoots(state);
    persistDocumentSession(state);
    std::set<std::string> movedExpanded;
    for (const auto& entry : state.expanded) {
        if (entry == oldRelative || entry.rfind(oldRelative + "/", 0) == 0)
            movedExpanded.insert(newRelative + entry.substr(oldRelative.size()));
        else movedExpanded.insert(entry);
    }
    state.expanded = std::move(movedExpanded);
    refreshVault(state, false);
    const bool isDir = std::filesystem::is_directory(target, error);
    revealEntry(state, newRelative, isDir);
    state.vaultSelectedPath = newRelative;
    state.vaultContextPath = newRelative;
    if (state.vaultRowFocusedPath == oldRelative) state.vaultRowFocusedPath = newRelative;
    showToast(state, i18n::tr("safety.renamed"), newName);
    return true;
}

void vaultDeleteEntry(AppState& state, const std::string& relative, bool isDir) {
    if (state.vaultRoot.empty()) {
        showToast(state, i18n::tr("safety.delete_failed"), i18n::tr("safety.vault_delete_unavailable"));
        return;
    }
    const std::string absolute = vaultAbsolutePath(state, relative);
    // Resolve relationships before Shell can remove any of the path components.
    const bool affectsCurrentDocument = pathAffected(state, absolute);
    const platform::DeleteResult result =
        platform::deleteVaultEntry(state.vaultRoot, relative, isDir);
    // Shell may partially delete before cancellation/failure. Preserve every actually
    // missing open document, including clean background pages, as an unsaved draft.
    for (const auto id : state.tabOrder) {
        if (id == state.tabId) continue;
        auto* document = documentTab(state, id);
        if (!document || !pathAffected(*document, absolute) || !pathDefinitelyMissing(document->path)) continue;
        document->recoveredFrom = document->path;
        document->newDocumentLanguage = document->language();
        document->path.clear(); document->diskFingerprint = {};
        if (!document->dirty()) ++document->revision;
        document->suppressRecovery = false;
    }
    persistDocumentSession(state);
    if (result.status == platform::DeleteStatus::Cancelled) {
        refreshVault(state, false);
        app::requestUpdate();
        const bool documentMissing = affectsCurrentDocument && pathDefinitelyMissing(state.path);
        if (documentMissing) {
            const bool recoveryWritten = preserveDeletedDocumentAsUnsaved(state, true);
            showToast(state, i18n::tr("safety.manuscript_kept"),
                      i18n::format(recoveryWritten ? "safety.deleted_kept" : "safety.deleted_kept_failed",
                                   {{"detail", result.message}}));
        } else {
            showToast(state, i18n::tr("safety.delete_canceled"), result.message);
        }
        return;
    }
    if (result.status == platform::DeleteStatus::Error) {
        refreshVault(state, false);
        app::requestUpdate();
        const bool documentMissing = affectsCurrentDocument && pathDefinitelyMissing(state.path);
        if (documentMissing) {
            const bool recoveryWritten = preserveDeletedDocumentAsUnsaved(state, true);
            showToast(state, i18n::tr("safety.manuscript_kept"),
                      i18n::format(recoveryWritten ? "safety.deleted_kept" : "safety.deleted_kept_failed",
                                   {{"detail", result.message}}));
        } else {
            showToast(state, i18n::tr("safety.delete_failed"), result.message);
        }
        return;
    }
    // 打开的文档被删掉了：内容留在内存里当"未保存的文稿"，用户还能另存。
    if (affectsCurrentDocument) {
        const bool recoveryWritten = preserveDeletedDocumentAsUnsaved(state, true);
        showToast(state, result.message.empty() ? i18n::tr("safety.delete_done") : i18n::tr("safety.delete_permanent"),
                  i18n::format(recoveryWritten ? "safety.deleted_unsaved" : "safety.deleted_unsaved_failed",
                                {{"detail", result.message}}));
    } else {
        showToast(state, result.message.empty() ? i18n::tr("safety.delete_done") : i18n::tr("safety.delete_permanent"),
                  result.message.empty() ? relative : i18n::format("safety.delete_detail", {{"path", relative}, {"detail", result.message}}));
    }
    refreshVault(state, false);
    app::requestUpdate();
}

void openLinkTarget(AppState& state, std::string target, const std::string& docDir) {
    // 首尾空白与自动链接的尖括号剥掉；URL 编码的空格最常见，先解掉。
    const std::size_t begin = target.find_first_not_of(" \t\r\n<");
    const std::size_t end = target.find_last_not_of(" \t\r\n>");
    if (begin == std::string::npos) {
        return;
    }
    target = target.substr(begin, end - begin + 1);
    for (std::size_t pos = target.find("%20"); pos != std::string::npos;
         pos = target.find("%20", pos + 1)) {
        target.replace(pos, 3, " ");
    }
    if (target.empty()) {
        return;
    }

    const auto startsWith = [&target](const char* prefix) {
        return target.rfind(prefix, 0) == 0;
    };
    if (startsWith("http://") || startsWith("https://") || startsWith("mailto:") ||
        startsWith("file://")) {
        if (!core::platform::openUrl(target)) {
            showToast(state, i18n::tr("safety.link_failed"), target);
        }
        return;
    }

    // 本地路径：链接里的 Windows 反斜杠统一成正斜杠再解析。
    std::replace(target.begin(), target.end(), '\\', '/');
    const std::filesystem::path raw = textfile::pathFromUtf8(target);
    std::vector<std::filesystem::path> bases;
    if (raw.is_absolute()) {
        bases.push_back({});
    } else {
        if (!docDir.empty()) {
            bases.push_back(textfile::pathFromUtf8(docDir));
        }
        if (!state.vaultRoot.empty()) {
            bases.push_back(textfile::pathFromUtf8(state.vaultRoot));
        }
    }
    std::error_code error;
    for (const std::filesystem::path& base : bases) {
        std::filesystem::path candidate = base.empty() ? raw : base / raw;
        if (std::filesystem::is_regular_file(candidate, error)) {
            requestOpenPath(state, textfile::pathToUtf8(candidate));
            return;
        }
        if (!candidate.has_extension()) {
            candidate += ".md";
            if (std::filesystem::is_regular_file(candidate, error)) {
                requestOpenPath(state, textfile::pathToUtf8(candidate));
                return;
            }
        }
    }
    showToast(state, i18n::tr("safety.link_missing"), target);
}

void performPending(AppState& state, PendingAction action) {
    switch (action) {
        case PendingAction::NewDocument:
            newDocument(state);
            break;
        case PendingAction::OpenFile:
            openFileFromDialog(state);
            break;
        case PendingAction::OpenPath: {
            const std::string path = state.pendingPath;
            state.pendingPath.clear();
            loadDocument(state, path);
            break;
        }
        case PendingAction::ReloadWithEncoding: {
            const textfile::ForcedEncoding forced = state.pendingEncoding;
            state.pendingEncoding = textfile::ForcedEncoding{};
            performReloadWithEncoding(state, forced);
            break;
        }
        case PendingAction::CloseWindow:
            state.confirmationTabId = 0;
            state.exitClosing = false;
            // The finalizer owns the close request, including the inline path.
            requestCloseDocument(state);
            break;
        case PendingAction::CloseTab:
            state.confirmationTabId = 0;
            closeActiveDocumentTab(state);
            break;
        case PendingAction::None:
            break;
    }
}

void persistSettings(const AppState& state) {
    settings::Data& data = settings::current();
    // 文档库不是常驻仓库（2026-09-26 口径）：vault 由当前文档决定，不落盘。
    // 这里显式清空，免得早前版本残留在 settings.ini 里的旧库根继续误导启动逻辑。
    data.vault.clear();
    data.uiLanguage = i18n::preference();
    data.mode = static_cast<int>(state.mode);
    data.showStatusBar = state.showStatusBar;
    data.lineNumbers = state.showLineNumbers;
    data.readableWidth = state.readableWidth;
    data.animations = state.animations;
    data.vaultWidth = state.vaultWidth;
    data.uiScale = state.uiScale;
    data.editorFontSize = state.editorFontSize;
    data.uiFontSize = state.uiFontSize;
    data.editorFontFile = state.editorFontFile;
    data.uiFontFile = state.uiFontFile;
    data.codeFontFile = state.codeFontFile;
    data.theme = static_cast<int>(state.theme);
    data.lastThemeFile = state.themeFile;
    settings::flush();
}

void applyUiScale(AppState& state, float scale) {
    state.uiScale = std::clamp(scale, kMinimumUiScale, kMaximumUiScale);
    // 框架每帧都从配置里读 uiScale，所以改完立刻生效；
    // 但逻辑尺寸随之改变，必须主动请求一帧，否则不会重新 compose。
    mutableAppConfig().uiScaleValue = state.uiScale;
    persistSettings(state);
    app::requestUpdate();
}

void applyEditorFontSize(AppState& state, float fontSize) {
    state.editorFontSize = std::clamp(fontSize, kMinimumEditorFontSize, kMaximumEditorFontSize);
    // 输入组件的排版缓存按 fontSize 判定，字号一改它自己会重排，不用额外处理。
    persistSettings(state);
    app::requestUpdate();
}

void applyShowStatusBar(AppState& state) {
    persistSettings(state);
    app::requestUpdate();
}

void applyTheme(AppState& state, ThemeMode mode) {
    if (state.theme == mode) {
        return;
    }
    state.theme = mode;
    // 清屏色只在启动时被配置读一次，所以这里必须手动同步（否则换到浅色主题后
    // 窗口底色仍是深色，缩窗口或最小化时会露出一圈旧底）。
    // editorColors() 自 T13 起是按 themeRevision() 失效的版本号缓存，所以这一行
    // 同时兜住"切深浅"与"主题文件覆盖过的配色"两种情况。
    mutableAppConfig().clearColorValue = editorColors().window;
    persistSettings(state);
    // 颜色是画的时候烘进图元的，保留层里的旧色必须整屏作废——与换字体同理。
    app::requestUpdate();
    app::detail::requestFullPaint();
    // 剩余项（T13 第一阶段不做，见报告）：Live Preview 的装饰缓存键
    // lp::DecorationCache::theme 目前只吃 ThemeMode 枚举，还没吃 themeRevision() ——
    // 深浅切换因此是好的，但"换主题文件、外观侧不变"时行内装饰要等下次键变才刷新。
    // 补法是在 lp_decorations.h 的缓存键里加一列 themeRevision（那是 T5 在改的文件）。
}

void applyUiFontSize(AppState& state, float fontSize) {
    state.uiFontSize = std::clamp(fontSize, kMinimumUiFontSize, kMaximumUiFontSize);
    // 界面尺寸没有缓存，下一次 compose 就从 uiMetrics(state) 重新算出来，
    // 所以这里只需要请求一帧。
    persistSettings(state);
    app::requestUpdate();
}

// 界面字体换掉之后，还要告诉框架：所有"没显式指定 fontFamily"的文字
// （按钮、对话框、下拉、列表项……）应该用哪个字体。
// setDefaultFontFiles 内部会在值变化时清掉共享字体栈缓存，所以运行时调用是有效的；
// 传空串表示回到框架自带的默认字体。
namespace {
bool prepareFontChange(AppState& state, const std::string& path, FontPickerTarget target) {
    const auto result = fontsafety::validate(path);
    if (!result.ok || (target == FontPickerTarget::Code && !path.empty() && !result.monospace)) {
        showToast(state, i18n::tr("safety.font_unchanged"), result.ok ? i18n::tr("safety.font_monospace") : result.error);
        return false;
    }
    settings::Data next = settings::current();
    next.editorFontFile = target == FontPickerTarget::Editor ? path : state.editorFontFile;
    next.uiFontFile = target == FontPickerTarget::Ui ? path : state.uiFontFile;
    next.codeFontFile = target == FontPickerTarget::Code ? path : state.codeFontFile;
    std::string error;
    if (!fontsafety::armSession(next, error)) {
        showToast(state, i18n::tr("safety.font_unchanged"), error);
        return false;
    }
    return true;
}
} // namespace

void applyUiFontFile(AppState& state, std::string path) {
    if (!prepareFontChange(state, path, FontPickerTarget::Ui)) return;
    state.uiFontFile = std::move(path);
    core::TextPrimitive::setDefaultFontFiles(uiFontFamily(state), "");
    persistSettings(state);
    // 改字体等于改了字体栈缓存，已排好的文字要整屏重画一遍。
    app::requestUpdate();
    app::detail::requestFullPaint();
}

// 编辑区/预览的字体是逐个元素显式传的，改完输入组件下一次 compose 就会重排；
// 但已经排好的文字在保留层里存的是旧字形，所以要整屏重画。
// 代码字体同理：装饰表按 codeFontFamily 进键会重建，整屏重画兜住保留层旧字形。
void applyEditorFontFile(AppState& state, std::string path) {
    if (!prepareFontChange(state, path, FontPickerTarget::Editor)) return;
    state.editorFontFile = std::move(path);
    persistSettings(state);
    app::requestUpdate();
    app::detail::requestFullPaint();
}

void applyCodeFontFile(AppState& state, std::string path) {
    if (!prepareFontChange(state, path, FontPickerTarget::Code)) return;
    state.codeFontFile = std::move(path);
    persistSettings(state);
    app::requestUpdate();
    app::detail::requestFullPaint();
}

// 文件关联入口：真正动注册表的是 platform/file_assoc，这里只包一层用户反馈。
void registerDefaultFileType(AppState& state) {
    const fileassoc::RegisterOutcome result = fileassoc::registerAsDefault();
    if (!result.ok) {
        showToast(state, i18n::tr("safety.register_failed"), result.error);
        refreshFileAssocState(state); // Partial writes must remain visible for repair/cleanup.
        return;
    }
    if (result.defaultsApplied) {
        showToast(state, i18n::tr("safety.register_done"), i18n::tr("safety.default_done"));
    } else {
        showToast(state, i18n::tr("safety.register_done"),
                  i18n::tr("safety.default_help"));
    }
    refreshFileAssocState(state);
}

void unregisterDefaultFileType(AppState& state) {
    std::string error;
    if (fileassoc::unregister(error)) {
        showToast(state, i18n::tr("safety.unregister_done"), i18n::tr("safety.unregister_help"));
    } else {
        showToast(state, i18n::tr("safety.unregister_failed"), error);
    }
    refreshFileAssocState(state);
}

void refreshFileAssocState(AppState& state) {
    state.fileAssocRegistered = fileassoc::isRegistered();
    state.fileAssocEntriesPresent = fileassoc::hasRegistrationEntries();
    state.associationStatuses = fileassoc::queryTypes();
    for(const auto& type:state.associationStatuses) {if(type.extension=="txt") state.fileAssocDefaults.txt=type.defaultApp;if(type.extension=="md") state.fileAssocDefaults.md=type.defaultApp;}
}

void resetViewSettings(AppState& state) {
    // 与 settings::Data 的默认值保持一处定义；只恢复视觉和编辑偏好。
    // 文档、最近文件、恢复副本与 Windows 文件关联不属于此操作。
    const settings::Data defaults;
    state.mode = static_cast<EditorMode>(defaults.mode);
    state.showStatusBar = defaults.showStatusBar;
    state.showLineNumbers = defaults.lineNumbers;
    state.readableWidth = defaults.readableWidth;
    state.vaultWidth = defaults.vaultWidth;
    state.editorFontSize = defaults.editorFontSize;
    state.uiFontSize = defaults.uiFontSize;
    state.uiScale = defaults.uiScale;
    state.animations = defaults.animations;
    state.editorFontFile = defaults.editorFontFile;
    state.uiFontFile = defaults.uiFontFile;
    state.codeFontFile = defaults.codeFontFile;
    // 主题文件一并回内置。必须排在下面那句 clearColor 之前：editorColors() 按
    // themeRevision() 失效，先 reset 才能取到内置配色的窗口底色。
    themeloader::reset();
    state.themeFile = defaults.lastThemeFile;
    state.theme = static_cast<ThemeMode>(defaults.theme);
    mutableAppConfig().uiScaleValue = state.uiScale;
    mutableAppConfig().clearColorValue = editorColors().window;
    // 界面字体一并回到框架默认。
    core::TextPrimitive::setDefaultFontFiles(uiFontFamily(state), "");
    persistSettings(state);
    app::requestUpdate();
    app::detail::requestFullPaint();
}

std::string vaultRelativePath(const AppState& state, const std::string& absolutePath) {
    if (state.vaultRoot.empty() || absolutePath.empty()) {
        return {};
    }
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(textfile::pathFromUtf8(absolutePath), textfile::pathFromUtf8(state.vaultRoot), error);
    if (error) {
        return {};
    }
    std::string text = textfile::pathToUtf8(relative);
    // 树里的 relative 统一用 '/'，Windows 上要换掉分隔符。
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

std::string vaultAbsolutePath(const AppState& state, const std::string& relativePath) {
    if (state.vaultRoot.empty() || relativePath.empty()) {
        return {};
    }
    return textfile::pathToUtf8(textfile::pathFromUtf8(state.vaultRoot) / textfile::pathFromUtf8(relativePath));
}

} // namespace neo
