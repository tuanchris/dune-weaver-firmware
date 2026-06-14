// Sand-table headless API helpers.

#pragma once

#include <string>

namespace SandApi {
    // Build the $Sand/Status JSON snapshot synchronously.  Used both by the
    // $Sand/Status command handler and by the multi-client /sand_status HTTP
    // route (which returns it directly in the HTTP body, so any number of app
    // clients can poll status without contending for the single-client
    // webui-v3 WebSocket).
    std::string statusJson();
}
