#include "filter.hpp"
#include "ui.hpp"
#include "ws_vendor.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("OBS Parameter Animator contributors")
MODULE_EXPORT const char *obs_module_description(void) {
    return "Six-zone GPU distortion with time-based C++ parameter animation";
}
static QWidget *panel = nullptr;
bool obs_module_load(void) {
    opa::registerFilter();
    blog(LOG_INFO, "[OPA] Version %s loaded", PLUGIN_VERSION);
    return true;
}
void obs_module_post_load(void) {
    panel = createAnimatorPanel(static_cast<QWidget *>(obs_frontend_get_main_window()));
    if (!obs_frontend_add_dock_by_id("obs-parameter-animator", "Parameter Animator", panel)) {
        delete panel;
        panel = nullptr;
    }
    if (!register_websocket_vendor())
        blog(LOG_WARNING, "[OPA] Vendor API unavailable; local controls still available");
}
void obs_module_unload(void) {
    /* This runs inside obs_shutdown() while libobs walks the module list: every other module that
       was loaded earlier (obs-websocket, obs-frontend-api internals, ...) may already be gone, so no
       cross-module calls are made here. The vendor requests are released on
       OBS_FRONTEND_EVENT_EXIT (see ws_vendor.cpp) and the dock widget belongs to the OBS main
       window, which destroys it together with the rest of the UI. */
    detach_websocket_vendor();
    panel = nullptr;
}
