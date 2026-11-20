#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include "resource.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"MiMediaPlayerWindow";
constexpr UINT kOutputMessage = WM_APP + 1;
constexpr UINT kFinishedMessage = WM_APP + 2;
constexpr COLORREF kPageColor = RGB(246, 249, 253);
constexpr COLORREF kHeaderColor = RGB(229, 245, 252);
constexpr COLORREF kTextColor = RGB(33, 48, 69);
constexpr COLORREF kMutedColor = RGB(92, 111, 132);

enum ControlId {
    IdPath = 100,
    IdBrowse,
    IdPlay,
    IdStop,
    IdPack,
    IdDecode,
    IdInfo,
    IdLog,
    IdStatus
};

struct AppState {
    HWND window = nullptr;
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND fileLabel = nullptr;
    HWND path = nullptr;
    HWND browse = nullptr;
    HWND play = nullptr;
    HWND stop = nullptr;
    HWND pack = nullptr;
    HWND decode = nullptr;
    HWND info = nullptr;
    HWND log = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HBRUSH pageBrush = nullptr;
    HBRUSH headerBrush = nullptr;
    HBRUSH whiteBrush = nullptr;
    HICON appIcon = nullptr;
    std::vector<std::wstring> selectedFiles;
    std::atomic<bool> running{false};
    std::atomic<bool> closing{false};
    std::mutex processMutex;
    HANDLE childProcess = nullptr;
    std::thread worker;
};

AppState app;

std::wstring QuoteArgument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (length == 0) {
        codePage = CP_ACP;
        length = MultiByteToWideChar(codePage, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    }
    if (length == 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return result;
}

void AppendLog(const std::wstring& text) {
    constexpr int kMaximumLogLength = 200000;
    int currentLength = GetWindowTextLengthW(app.log);
    if (currentLength > kMaximumLogLength) {
        SendMessageW(app.log, EM_SETSEL, 0, currentLength / 2);
        SendMessageW(app.log, EM_REPLACESEL, FALSE,
                     reinterpret_cast<LPARAM>(L""));
    }
    SendMessageW(app.log, EM_SETSEL, GetWindowTextLengthW(app.log), -1);
    SendMessageW(app.log, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(app.log, EM_SCROLLCARET, 0, 0);
}

void PostOutput(const std::wstring& text) {
    auto* message = new std::wstring(text);
    if (!PostMessageW(app.window, kOutputMessage, 0,
                      reinterpret_cast<LPARAM>(message))) {
        delete message;
    }
}

void SetBusy(bool busy) {
    app.running = busy;
    EnableWindow(app.path, !busy);
    EnableWindow(app.browse, !busy);
    EnableWindow(app.play, !busy);
    EnableWindow(app.pack, !busy);
    EnableWindow(app.decode, !busy);
    EnableWindow(app.info, !busy);
    InvalidateRect(app.play, nullptr, TRUE);
    InvalidateRect(app.pack, nullptr, TRUE);
    EnableWindow(app.stop, busy);
    SetWindowTextW(app.status, busy ? L"Đang xử lý..." : L"Sẵn sàng");
}

void RunBackend(std::vector<std::wstring> arguments) {
    if (app.worker.joinable()) {
        app.worker.join();
    }
    SetBusy(true);
    AppendLog(L"\r\n> Mi Media Player\r\n");

    app.worker = std::thread([arguments = std::move(arguments)]() {
        wchar_t executablePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
        const auto scriptPath =
            std::filesystem::path(executablePath).parent_path() / L"play_mi.py";
        if (!std::filesystem::exists(scriptPath)) {
            PostOutput(L"Không tìm thấy play_mi.py cạnh ứng dụng.\r\n");
            PostMessageW(app.window, kFinishedMessage, 1, 0);
            return;
        }

        std::wstring command =
            L"python -X utf8 -u " + QuoteArgument(scriptPath.wstring());
        for (const auto& argument : arguments) {
            command += L" " + QuoteArgument(argument);
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
            PostOutput(L"Không tạo được pipe để đọc kết quả từ Python.\r\n");
            PostMessageW(app.window, kFinishedMessage, 1, 0);
            return;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        HANDLE inputHandle = CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (inputHandle == INVALID_HANDLE_VALUE) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            PostOutput(L"Không mở được thiết bị đầu vào NUL cho tiến trình Python.\r\n");
            PostMessageW(app.window, kFinishedMessage, 1, 0);
            return;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.hStdInput = inputHandle;

        PROCESS_INFORMATION processInfo{};
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        const auto workingDirectory = scriptPath.parent_path().wstring();
        const BOOL created = CreateProcessW(
            nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup,
            &processInfo);
        CloseHandle(inputHandle);
        CloseHandle(writePipe);

        if (!created) {
            const DWORD error = GetLastError();
            CloseHandle(readPipe);
            PostOutput(L"Không chạy được Python. Hãy cài Python và thêm vào PATH "
                       L"(mã lỗi " + std::to_wstring(error) + L").\r\n");
            PostMessageW(app.window, kFinishedMessage, 1, 0);
            return;
        }

        CloseHandle(processInfo.hThread);
        {
            std::lock_guard<std::mutex> lock(app.processMutex);
            app.childProcess = processInfo.hProcess;
            if (app.closing) {
                TerminateProcess(app.childProcess, 1);
            }
        }

        std::string pending;
        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) &&
               bytesRead > 0) {
            for (DWORD i = 0; i < bytesRead; ++i) {
                const char character = buffer[i];
                if (character == '\r' || character == '\n') {
                    if (!pending.empty()) {
                        PostOutput(Utf8ToWide(pending) + L"\r\n");
                        pending.clear();
                    }
                } else {
                    pending.push_back(character);
                }
            }
        }
        if (!pending.empty()) {
            PostOutput(Utf8ToWide(pending) + L"\r\n");
        }
        CloseHandle(readPipe);

        WaitForSingleObject(processInfo.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        {
            std::lock_guard<std::mutex> lock(app.processMutex);
            app.childProcess = nullptr;
        }
        CloseHandle(processInfo.hProcess);
        PostMessageW(app.window, kFinishedMessage, exitCode, 0);
    });
}

std::wstring SelectedPath() {
    const int length = GetWindowTextLengthW(app.path);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(app.path, value.data(), length + 1);
    }
    value.resize(static_cast<size_t>(length));
    return value;
}

std::vector<std::wstring> SelectedAudioFiles() {
    if (!app.selectedFiles.empty()) {
        return app.selectedFiles;
    }
    const auto path = SelectedPath();
    return path.empty() ? std::vector<std::wstring>{}
                        : std::vector<std::wstring>{path};
}

bool HasMiExtension(const std::wstring& path) {
    auto extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return extension == L".mi";
}

std::wstring ChooseFile(bool save, const wchar_t* filter,
                        const std::wstring& initialName = L"") {
    wchar_t fileName[MAX_PATH] = {};
    if (!initialName.empty()) {
        const auto length = std::min(initialName.size(), size_t(MAX_PATH - 1));
        initialName.copy(fileName, length);
        fileName[length] = L'\0';
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = app.window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    dialog.lpstrDefExt = save ? L"mi" : nullptr;

    const BOOL chosen = save ? GetSaveFileNameW(&dialog)
                             : GetOpenFileNameW(&dialog);
    return chosen ? fileName : L"";
}

void BrowseForAudio() {
    static const wchar_t filter[] =
        L"Tất cả tệp âm thanh và định dạng được FFmpeg hỗ trợ\0*.*\0"
        L"Tất cả tệp (*.*)\0*.*\0\0";
    std::vector<wchar_t> fileNames(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = app.window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = fileNames.data();
    dialog.nMaxFile = static_cast<DWORD>(fileNames.size());
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST |
                   OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    app.selectedFiles.clear();
    const wchar_t* cursor = fileNames.data();
    const std::wstring first = cursor;
    cursor += first.size() + 1;
    if (*cursor == L'\0') {
        app.selectedFiles.push_back(first);
    } else {
        const std::filesystem::path directory(first);
        while (*cursor != L'\0') {
            app.selectedFiles.push_back(
                (directory / std::filesystem::path(cursor)).wstring());
            cursor += wcslen(cursor) + 1;
        }
    }

    const std::wstring display = app.selectedFiles.size() == 1
                                     ? app.selectedFiles.front()
                                     : std::to_wstring(app.selectedFiles.size()) +
                                           L" tệp âm thanh đã chọn";
    SetWindowTextW(app.path, display.c_str());
    SetWindowTextW(app.status,
                   (std::to_wstring(app.selectedFiles.size()) +
                    L" tệp đã sẵn sàng").c_str());
}

void StartSelectedAction(int command) {
    const auto inputs = SelectedAudioFiles();
    if (inputs.empty()) {
        MessageBoxW(app.window, L"Hãy chọn một tệp trước.", L"Mi Media Player",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto& input = inputs.front();
    if ((command == IdDecode || command == IdInfo) && !HasMiExtension(input)) {
        MessageBoxW(app.window, L"Chức năng này chỉ áp dụng cho tệp .mi.",
                    L"Mi Media Player", MB_OK | MB_ICONWARNING);
        return;
    }

    if (command == IdPlay) {
        if (inputs.size() > 1) {
            AppendLog(L"Đang phát tệp đầu tiên trong danh sách đã chọn.\r\n");
        }
        RunBackend({input});
    } else if (command == IdInfo) {
        RunBackend({L"-i", input});
    } else if (command == IdPack) {
        std::vector<std::wstring> sources;
        std::set<std::wstring> outputPaths;
        bool outputExists = false;
        for (const auto& path : inputs) {
            if (HasMiExtension(path)) {
                continue;
            }
            auto output = std::filesystem::path(path);
            output.replace_extension(L".mi");
            auto normalizedOutput = output.lexically_normal().wstring();
            std::transform(normalizedOutput.begin(), normalizedOutput.end(),
                           normalizedOutput.begin(),
                           [](wchar_t character) {
                               return static_cast<wchar_t>(
                                   std::towlower(character));
                           });
            if (!outputPaths.insert(normalizedOutput).second) {
                MessageBoxW(
                    app.window,
                    L"Nhiều tệp có cùng tên đầu ra .mi. Hãy chọn lại danh sách.",
                    L"Mi Media Player", MB_OK | MB_ICONWARNING);
                return;
            }
            outputExists = outputExists || std::filesystem::exists(output);
            sources.push_back(path);
        }
        if (sources.empty()) {
            MessageBoxW(app.window, L"Danh sách không có tệp audio cần chuyển.",
                        L"Mi Media Player", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (outputExists &&
            MessageBoxW(app.window,
                        L"Một số tệp .mi đã tồn tại cạnh tệp nguồn. Ghi đè?",
                        L"Mi Media Player", MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
        std::vector<std::wstring> arguments{L"-b"};
        arguments.insert(arguments.end(), sources.begin(), sources.end());
        RunBackend(std::move(arguments));
    } else if (command == IdDecode) {
        static const wchar_t filter[] =
            L"PCM thô (*.raw)\0*.raw\0Tất cả tệp (*.*)\0*.*\0\0";
        auto output = std::filesystem::path(input);
        output.replace_extension(L".raw");
        const auto chosen = ChooseFile(true, filter, output.filename().wstring());
        if (!chosen.empty()) {
            RunBackend({L"-d", input, chosen});
        }
    }
}

HWND AddControl(HWND parent, const wchar_t* className, const wchar_t* text,
                DWORD style, int id) {
    HWND control = CreateWindowExW(
        0, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return control;
}

void Layout(int width, int height) {
    constexpr int margin = 24;
    constexpr int gap = 10;
    constexpr int buttonHeight = 42;
    const int contentWidth = std::max(320, width - margin * 2);
    MoveWindow(app.title, margin + 62, 24, contentWidth - 70, 38, TRUE);
    MoveWindow(app.subtitle, margin + 62, 61, contentWidth - 70, 24, TRUE);
    MoveWindow(app.fileLabel, margin, 106, contentWidth, 18, TRUE);
    MoveWindow(app.path, margin, 128, contentWidth - 116, 36, TRUE);
    MoveWindow(app.browse, width - margin - 106, 128, 106, 36, TRUE);

    const int buttonY = 178;
    const int buttonWidth = std::max(72, (contentWidth - gap * 4) / 5);
    HWND buttons[] = {app.play, app.stop, app.pack, app.decode, app.info};
    for (int i = 0; i < 5; ++i) {
        const int x = margin + i * (buttonWidth + gap);
        MoveWindow(buttons[i], x, buttonY, buttonWidth, buttonHeight, TRUE);
    }

    MoveWindow(app.status, margin + 2, 231, contentWidth, 24, TRUE);
    MoveWindow(app.log, margin, 260, contentWidth,
               std::max(70, height - 282), TRUE);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        app.window = window;
        app.pageBrush = CreateSolidBrush(kPageColor);
        app.headerBrush = CreateSolidBrush(kHeaderColor);
        app.whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
        app.appIcon = LoadIconW(GetModuleHandleW(nullptr),
                                MAKEINTRESOURCEW(IDI_APP_ICON));
        app.font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        app.title = AddControl(window, L"STATIC", L"Mi Media Player",
                               SS_LEFT, 0);
        app.titleFont = CreateFontW(
            -27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(app.title, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app.titleFont), TRUE);
        app.subtitle = AddControl(
            window, L"STATIC", L"Chuyển đổi mọi audio bạn có và bắt đầu phát",
            SS_LEFT, 0);
        app.fileLabel =
            AddControl(window, L"STATIC", L"CHỌN MỘT HOẶC NHIỀU TỆP AUDIO",
                       SS_LEFT, 0);
        app.path = AddControl(window, L"EDIT", L"",
                              ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP |
                                  WS_BORDER,
                              IdPath);
        app.browse = AddControl(window, L"BUTTON", L"Chọn tệp",
                                BS_OWNERDRAW | WS_TABSTOP, IdBrowse);
        app.play = AddControl(window, L"BUTTON", L"Phát",
                              BS_OWNERDRAW | WS_TABSTOP, IdPlay);
        app.stop = AddControl(window, L"BUTTON", L"Dừng",
                              BS_OWNERDRAW | WS_TABSTOP, IdStop);
        app.pack = AddControl(window, L"BUTTON", L"Chuyển .mi",
                              BS_OWNERDRAW | WS_TABSTOP, IdPack);
        app.decode = AddControl(window, L"BUTTON", L"Xuất RAW",
                                BS_OWNERDRAW | WS_TABSTOP, IdDecode);
        app.info = AddControl(window, L"BUTTON", L"Thông tin",
                              BS_OWNERDRAW | WS_TABSTOP, IdInfo);
        app.status = AddControl(window, L"STATIC", L"Sẵn sàng",
                                SS_LEFT, IdStatus);
        app.log = AddControl(
            window, L"EDIT",
            L"Chọn nhiều tệp để chuyển hàng loạt. FFmpeg hỗ trợ hàng trăm định dạng audio.\r\n",
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL |
                WS_BORDER,
            IdLog);
        Layout(820, 560);
        EnableWindow(app.stop, FALSE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT area{};
        GetClientRect(window, &area);
        FillRect(reinterpret_cast<HDC>(wParam), &area, app.pageBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT header{};
        GetClientRect(window, &header);
        header.bottom = 171;
        FillRect(dc, &header, app.headerBrush);
        DrawIconEx(dc, 24, 24, app.appIcon, 48, 48, 0, nullptr, DI_NORMAL);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        if (control == app.title) {
            SetTextColor(dc, RGB(27, 49, 77));
            return reinterpret_cast<LRESULT>(app.headerBrush);
        }
        if (control == app.subtitle || control == app.fileLabel) {
            SetTextColor(dc, kMutedColor);
            return reinterpret_cast<LRESULT>(app.headerBrush);
        }
        SetTextColor(dc, kTextColor);
        return reinterpret_cast<LRESULT>(app.pageBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kTextColor);
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(app.whiteBrush);
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        COLORREF fill = RGB(255, 255, 255);
        COLORREF text = kTextColor;
        COLORREF border = RGB(220, 229, 238);
        switch (draw->CtlID) {
        case IdPlay:
            fill = RGB(42, 133, 225);
            text = RGB(255, 255, 255);
            border = fill;
            break;
        case IdPack:
            fill = RGB(19, 157, 139);
            text = RGB(255, 255, 255);
            border = fill;
            break;
        case IdStop:
            fill = RGB(255, 239, 238);
            text = RGB(190, 75, 74);
            border = RGB(250, 220, 218);
            break;
        default:
            break;
        }
        if ((draw->itemState & ODS_DISABLED) != 0) {
            fill = RGB(235, 239, 244);
            text = RGB(145, 157, 171);
            border = fill;
        }
        RECT button = draw->rcItem;
        InflateRect(&button, -1, -1);
        HBRUSH buttonBrush = CreateSolidBrush(fill);
        HPEN buttonPen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBrush = SelectObject(draw->hDC, buttonBrush);
        HGDIOBJ oldPen = SelectObject(draw->hDC, buttonPen);
        RoundRect(draw->hDC, button.left, button.top, button.right,
                  button.bottom, 12, 12);
        SelectObject(draw->hDC, oldBrush);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(buttonBrush);
        DeleteObject(buttonPen);

        wchar_t label[80] = {};
        GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, text);
        HFONT oldFont = static_cast<HFONT>(
            SelectObject(draw->hDC, app.font));
        DrawTextW(draw->hDC, label, -1, &button,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(draw->hDC, oldFont);
        if ((draw->itemState & ODS_FOCUS) != 0) {
            InflateRect(&button, -5, -5);
            DrawFocusRect(draw->hDC, &button);
        }
        return TRUE;
    }
    case WM_SIZE:
        Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 720;
        limits->ptMinTrackSize.y = 500;
        return 0;
    }
    case WM_COMMAND: {
        const int command = LOWORD(wParam);
        if (command == IdBrowse) {
            BrowseForAudio();
        } else if (command == IdStop) {
            std::lock_guard<std::mutex> lock(app.processMutex);
            if (app.childProcess) {
                TerminateProcess(app.childProcess, 1);
                SetWindowTextW(app.status, L"Đang dừng...");
            }
        } else if (command == IdPlay || command == IdPack ||
                   command == IdDecode || command == IdInfo) {
            StartSelectedAction(command);
        }
        return 0;
    }
    case kOutputMessage: {
        std::unique_ptr<std::wstring> text(
            reinterpret_cast<std::wstring*>(lParam));
        AppendLog(*text);
        return 0;
    }
    case kFinishedMessage:
        SetBusy(false);
        if (wParam == 0) {
            AppendLog(L"Hoàn tất.\r\n");
        } else {
            AppendLog(L"Kết thúc với lỗi hoặc đã dừng (mã "
                      + std::to_wstring(wParam) + L").\r\n");
        }
        return 0;
    case WM_DESTROY: {
        app.closing = true;
        {
            std::lock_guard<std::mutex> lock(app.processMutex);
            if (app.childProcess) {
                TerminateProcess(app.childProcess, 1);
            }
        }
        if (app.worker.joinable()) {
            app.worker.join();
        }
        MSG queued{};
        while (PeekMessageW(&queued, window, kOutputMessage, kOutputMessage,
                            PM_REMOVE)) {
            delete reinterpret_cast<std::wstring*>(queued.lParam);
        }
        DeleteObject(app.font);
        DeleteObject(app.titleFont);
        DeleteObject(app.pageBrush);
        DeleteObject(app.headerBrush);
        DeleteObject(app.whiteBrush);
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    HWND window = CreateWindowExW(
        0, kWindowClass, L"Mi Media Player", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 560, nullptr, nullptr, instance,
        nullptr);
    if (!window) {
        return 1;
    }
    SendMessageW(window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(windowClass.hIcon));
    SendMessageW(window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(windowClass.hIcon));

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
