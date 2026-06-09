#pragma once

namespace nxtdbg::app { struct App; }

namespace nxtdbg::panels
{

// Interactive interface browser. Three panes:
//   1. Open interfaces — polled from snapshot.openIfaces every frame.
//   2. Component tree — get_interface_tree RPC, periodically refreshed.
//   3. Selected component — full ComponentInfo field dump + cache names.
//
// Pick mode arms a 5Hz GetCursorPos poll that issues find_component_at on
// every tick; the agent does the screen-to-client conversion so the
// debugger never publishes its HWND.
void DrawInterfacePanel(app::App &);

}
