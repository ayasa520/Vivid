/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * CEF subprocess executable for the Vivid web backend (renderer, GPU,
 * zygote, utility processes). The renderer process injects the Wallpaper
 * Engine bridge JS into every page context before project scripts run.
 * Frame ported from gstcefsrc's gstcefsubprocess.cc.
 */
#include <include/cef_app.h>
#include <include/cef_render_process_handler.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "vivid_web_bridge_js.h"

namespace
{

enum ProcessType
{
    PROCESS_TYPE_BROWSER,
    PROCESS_TYPE_RENDERER,
    PROCESS_TYPE_OTHER,
};

/* These flag names must match the Chromium values. */
constexpr char kProcessType[] = "type";
constexpr char kRendererProcess[] = "renderer";
constexpr char kZygoteProcess[] = "zygote";
constexpr char kDriPrimeEnv[] = "DRI_PRIME";
constexpr char kNvPrimeRenderOffloadEnv[] = "__NV_PRIME_RENDER_OFFLOAD";
constexpr char kEglVendorLibraryFilenamesEnv[] = "__EGL_VENDOR_LIBRARY_FILENAMES";
constexpr char kVividRenderNodeSwitch[] = "vivid-render-node";
constexpr char kVividDriPrimeSwitch[] = "vivid-dri-prime";
constexpr char kVividEglVendorSwitch[] = "vivid-egl-vendor";

const char*
process_type_name(ProcessType process_type)
{
    switch (process_type) {
    case PROCESS_TYPE_BROWSER:
        return "browser";
    case PROCESS_TYPE_RENDERER:
        return "renderer-or-zygote";
    case PROCESS_TYPE_OTHER:
    default:
        return "other";
    }
}

const char*
env_value_for_log(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value ? value : "(unset)";
}

CefRefPtr<CefCommandLine>
command_line_from_args(const CefMainArgs& main_args)
{
    CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
    command_line->InitFromArgv(main_args.argc, main_args.argv);
    return command_line;
}

std::string
switch_value_for_log(CefRefPtr<CefCommandLine> command_line, const char* name)
{
    if (!command_line || !command_line->HasSwitch(name))
        return "(unset)";
    const std::string value = command_line->GetSwitchValue(name);
    return value.empty() ? "(empty)" : value;
}

std::string
chromium_process_type_for_log(CefRefPtr<CefCommandLine> command_line)
{
    if (!command_line || !command_line->HasSwitch(kProcessType))
        return "browser";
    const std::string process_type = command_line->GetSwitchValue(kProcessType);
    return process_type.empty() ? "(empty)" : process_type;
}

void
log_gpu_environment(ProcessType process_type, CefRefPtr<CefCommandLine> command_line)
{
    const std::string chromium_process_type = chromium_process_type_for_log(command_line);
    const std::string vivid_render_node =
        switch_value_for_log(command_line, kVividRenderNodeSwitch);
    const std::string vivid_dri_prime =
        switch_value_for_log(command_line, kVividDriPrimeSwitch);
    const std::string vivid_egl_vendor =
        switch_value_for_log(command_line, kVividEglVendorSwitch);

    /*
     * This helper is the executable CEF uses for the GPU process, zygote,
     * renderer and utility processes. Logging the inherited environment here
     * proves whether the browser process exported the resolved GPU policy
     * before Chromium's Linux process tree begins forking children.
     */
    std::fprintf(stderr,
                 "VividWebHelper: pid=%ld ppid=%ld process=%s chromium-type=%s "
                 "env %s=%s %s=%s %s=%s policy render-node=%s dri-prime=%s "
                 "egl-vendor=%s\n",
                 (long)getpid(),
                 (long)getppid(),
                 process_type_name(process_type),
                 chromium_process_type.c_str(),
                 kDriPrimeEnv,
                 env_value_for_log(kDriPrimeEnv),
                 kNvPrimeRenderOffloadEnv,
                 env_value_for_log(kNvPrimeRenderOffloadEnv),
                 kEglVendorLibraryFilenamesEnv,
                 env_value_for_log(kEglVendorLibraryFilenamesEnv),
                 vivid_render_node.c_str(),
                 vivid_dri_prime.c_str(),
                 vivid_egl_vendor.c_str());
    std::fflush(stderr);
}

ProcessType
get_process_type(CefRefPtr<CefCommandLine> command_line)
{
    if (!command_line || !command_line->HasSwitch(kProcessType))
        return PROCESS_TYPE_BROWSER;

    const std::string process_type = command_line->GetSwitchValue(kProcessType);
    if (process_type == kRendererProcess)
        return PROCESS_TYPE_RENDERER;
    /*
     * On Linux the zygote process spawns the real renderers, so it has to
     * carry the renderer app as well.
     */
    if (process_type == kZygoteProcess)
        return PROCESS_TYPE_RENDERER;

    return PROCESS_TYPE_OTHER;
}

class VividWebRendererApp : public CefApp, public CefRenderProcessHandler
{
public:
    VividWebRendererApp() = default;

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override
    {
        /* Runs before any page script in this context. */
        frame->ExecuteJavaScript(kVividWebBridgeJs, frame->GetURL(), 0);
    }

private:
    IMPLEMENT_REFCOUNTING(VividWebRendererApp);
    DISALLOW_COPY_AND_ASSIGN(VividWebRendererApp);
};

} // namespace

int
main(int argc, char* argv[])
{
    CefMainArgs args(argc, argv);
    CefRefPtr<CefCommandLine> command_line = command_line_from_args(args);
    const ProcessType process_type = get_process_type(command_line);
    log_gpu_environment(process_type, command_line);

    CefRefPtr<CefApp> app = nullptr;
    switch (process_type) {
    case PROCESS_TYPE_RENDERER:
        app = new VividWebRendererApp();
        break;
    case PROCESS_TYPE_BROWSER:
    case PROCESS_TYPE_OTHER:
        break;
    }

    return CefExecuteProcess(args, app, nullptr);
}
