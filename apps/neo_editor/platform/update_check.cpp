#include "model/update_check.h"
#include "model/version.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <memory>
#endif

namespace neo::updates {

std::string fetchLatest() {
#ifdef _WIN32
    using Handle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
    const Handle session(WinHttpOpen(L"EUI-Edits", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                         WinHttpCloseHandle);
    constexpr int timeoutMs = 2000;
    if (!session || !WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs)) return {};
    const Handle connection(WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0),
                            WinHttpCloseHandle);
    if (!connection) return {};
    const Handle request(WinHttpOpenRequest(connection.get(), L"GET", L"/repos/fclx512/EUI-Edits/releases/latest",
                                           nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE),
                         WinHttpCloseHandle);
    if (!request) return {};
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSendRequest(request.get(), L"Accept: application/vnd.github+json\r\n",
                            DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) return {};
    DWORD status = 0, statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX) ||
        status != HTTP_STATUS_OK) return {};

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    constexpr std::size_t maxResponse = 256 * 1024;
    std::string body;
    char buffer[8192];
    while (Clock::now() < deadline) {
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &received) ||
            body.size() + received > maxResponse) return {};
        if (received == 0) return newerStableRelease(body, version::kVersion);
        body.append(buffer, received);
    }
#endif
    return {};
}

} // namespace neo::updates
