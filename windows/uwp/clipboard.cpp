// UWP clipboard access through Windows.ApplicationModel.DataTransfer.
//
// SDL2's Windows clipboard code uses OpenClipboard/GetClipboardData, which a UWP
// app cannot use. SDL also runs its host on an MTA, while Clipboard.GetContent
// is unsupported from an MTA, so the read is marshalled through the CoreWindow
// dispatcher. The host polls for the answer from its normal frame loop.
#include <windows.h>
#include <ppltasks.h>

#include <cstring>
#include <mutex>
#include <string>
#include <utility>

using namespace concurrency;
using namespace Platform;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::UI::Core;

namespace {

std::mutex g_lock;
std::string g_text;
bool g_pending;
bool g_ready;
bool g_failed;

void finish(std::string text, bool failed) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_text.swap(text);
    g_failed = failed;
    g_ready = true;
    g_pending = false;
}

std::string wide_to_utf8(const wchar_t *wide) {
    if (!wide || !wide[0])
        return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return std::string();
    std::string result(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &result[0], needed, nullptr, nullptr);
    return result;
}

std::string to_utf8(String ^ text) {
    return text ? wide_to_utf8(text->Data()) : std::string();
}

} // namespace

extern "C" void pd_uwp_clipboard_request(void) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_pending)
            return;
        g_pending = true;
        g_ready = false;
        g_failed = false;
        g_text.clear();
    }

    CoreWindow ^ window = CoreWindow::GetForCurrentThread();
    if (!window) {
        finish(std::string(), true);
        return;
    }
    try {
        window->Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([] {
            try {
                DataPackageView ^ content = Clipboard::GetContent();
                if (!content || !content->Contains(StandardDataFormats::Text)) {
                    finish(std::string(), false);
                    return;
                }
                create_task(content->GetTextAsync()).then([](task<String ^> completed) {
                    std::string text;
                    bool failed = false;
                    try {
                        text = to_utf8(completed.get());
                    } catch (Exception ^) {
                        failed = true;
                    } catch (...) {
                        failed = true;
                    }
                    finish(std::move(text), failed);
                });
            } catch (Exception ^) {
                finish(std::string(), true);
            } catch (...) {
                finish(std::string(), true);
            }
        }));
    } catch (Exception ^) {
        finish(std::string(), true);
    } catch (...) {
        finish(std::string(), true);
    }
}

// 0 = still reading, 1 = text copied to out, -1 = unavailable or no request.
extern "C" int pd_uwp_clipboard_poll(char *out, int capacity) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_ready)
        return g_pending ? 0 : -1;

    int result = g_failed ? -1 : 1;
    if (result > 0 && out && capacity > 0) {
        size_t length = g_text.size();
        if (length >= static_cast<size_t>(capacity))
            length = static_cast<size_t>(capacity) - 1;
        if (length)
            std::memcpy(out, g_text.data(), length);
        out[length] = '\0';
    }
    g_ready = false;
    g_failed = false;
    return result;
}
