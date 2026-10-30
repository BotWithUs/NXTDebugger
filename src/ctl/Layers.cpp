#include "ctl/Commands.h"

#include "app/App.h"
#include "overlay/LayerSync.h"

#include "imgui.h"
#include "imgui_internal.h"   // MarkIniSettingsDirty

#include <cstdio>
#include <cstdlib>

// `layers` -- read and set the in-game overlay layers (panels/LayersPanel.cpp)
// from the control pipe:
//
//   nxtdbgctl layers                              -> every layer's settings + state
//   nxtdbgctl layers set npc=on loc=off           -> toggle
//   nxtdbgctl layers set npc.filter=Banker npc.labels=name_id loc.max_distance=10
//   nxtdbgctl layers set refresh_ms=1200 label_budget=20 iface=on
//   nxtdbgctl layers retry                        -> re-arm layers the agent refused
//
// nxtdbgctl turns the bare word after the command into `action`. Any
// key=value is applied whether or not `action` says "set".
namespace nxtdbg::ctl
{

namespace
{

using overlay::LayerId;
using overlay::LayerSettings;

bool ParseSwitch(const JsonValue &v, bool &out)
{
    if (v.IsBool() || v.IsNumber())
    {
        out = v.IsBool() ? v.boolVal : v.AsInt() != 0;
        return true;
    }
    const std::string s = v.AsString();
    if (s == "on" || s == "true" || s == "1")
    {
        out = true;
        return true;
    }
    if (s == "off" || s == "false" || s == "0")
    {
        out = false;
        return true;
    }
    return false;
}

// 0xAARRGGBB, AARRGGBB, #RRGGBB (opaque) or a plain integer.
bool ParseColor(const JsonValue &v, uint32_t &out)
{
    if (v.IsNumber())
    {
        out = static_cast<uint32_t>(v.AsInt());
        return true;
    }
    std::string s = v.AsString();
    const bool isRgb = !s.empty() && s[0] == '#';
    if (isRgb)
    {
        s.erase(0, 1);
    }
    char         *end = nullptr;
    const unsigned long n = std::strtoul(s.c_str(), &end, 16);
    if (s.empty() || end == nullptr || *end != '\0')
    {
        return false;
    }
    out = isRgb ? (0xFF000000u | static_cast<uint32_t>(n)) : static_cast<uint32_t>(n);
    return true;
}

bool SetLayerField(LayerSettings &s, const std::string &field, const JsonValue &v,
                   std::string &outError)
{
    bool isOk = true;
    if (field.empty() || field == "enabled")
    {
        isOk = ParseSwitch(v, s.isEnabled);
    }
    else if (field == "color")
    {
        isOk = ParseColor(v, s.colorArgb);
    }
    else if (field == "labels")
    {
        isOk = overlay::ParseLabelMode(v.AsString(), s.labels);
    }
    else if (field == "coords")
    {
        isOk = ParseSwitch(v, s.showCoords);
    }
    else if (field == "max_distance" || field == "max_count")
    {
        int &target = field == "max_count" ? s.maxCount : s.maxDistance;
        isOk   = v.IsNumber();
        target = static_cast<int>(v.AsInt(target));
    }
    else if (field == "filter")
    {
        std::snprintf(s.filter, sizeof(s.filter), "%s", v.AsString().c_str());
    }
    else
    {
        outError = "unknown layer field '" + field
                 + "' (enabled, color, labels, max_distance, max_count, filter)";
        return false;
    }
    if (!isOk)
    {
        outError = "bad value for '" + field + "'";
    }
    return isOk;
}

// The edit a request makes, applied to a copy and committed only when every
// argument parsed: a bad third argument must not leave the first two applied.
struct LayerEdit
{
    overlay::LayerConfig config;
    bool                 isIfaceOn       = false;
    bool                 isIfaceAllLabels = false;
};

// refresh_ms / label_budget / iface / iface_all_labels. False when `key` is
// not a global.
bool SetGlobal(LayerEdit &e, const std::string &key, const JsonValue &v, std::string &outError)
{
    const bool isRefresh = key == "refresh_ms" || key == "interval_ms";
    if (isRefresh || key == "label_budget")
    {
        int &field = isRefresh ? e.config.refreshMs : e.config.labelBudget;
        if (!v.IsNumber())
        {
            outError = key + " takes a number";
        }
        field = static_cast<int>(v.AsInt(field));
        return true;
    }
    bool *target = (key == "iface" || key == "interfaces") ? &e.isIfaceOn
                 : (key == "iface_all_labels")             ? &e.isIfaceAllLabels
                                                           : nullptr;
    if (target == nullptr)
    {
        return false;
    }
    if (!ParseSwitch(v, *target))
    {
        outError = key + " takes on / off";
    }
    return true;
}

// One `key=value`: a global, `<layer>` (= enabled) or `<layer>.<field>`.
bool ApplyArg(LayerEdit &e, const std::string &key, const JsonValue &v, std::string &outError)
{
    if (SetGlobal(e, key, v, outError))
    {
        return outError.empty();
    }
    const size_t      dot   = key.find('.');
    const std::string token = key.substr(0, dot);
    const std::string field = dot == std::string::npos ? std::string() : key.substr(dot + 1);
    LayerId id = LayerId::Npc;
    if (!overlay::ParseLayerToken(token, id))
    {
        outError = "unknown layer '" + token + "' (npc, player, self, loc, ground, iface)";
        return false;
    }
    return SetLayerField(e.config.layers[static_cast<int>(id)], field, v, outError);
}

JsonValue LayerJson(const app::App &a, int i)
{
    const LayerSettings          &s  = a.layers.config.layers[i];
    const overlay::LayerRuntime  &rt = a.layers.runtime[i];
    char color[16];
    std::snprintf(color, sizeof(color), "0x%08X", s.colorArgb);
    JsonValue o = JObj();
    Put(o, "layer", JStr(overlay::LayerToken(static_cast<LayerId>(i))));
    Put(o, "enabled", JBool(s.isEnabled));
    Put(o, "color", JStr(color));
    Put(o, "labels", JStr(overlay::LabelModeToken(s.labels)));
    Put(o, "coords", JBool(s.showCoords));
    Put(o, "max_distance", JInt(s.maxDistance));
    Put(o, "max_count", JInt(s.maxCount));
    Put(o, "filter", JStr(s.filter));
    Put(o, "shown", JInt(rt.shown));
    Put(o, "matched", JInt(rt.matched));
    Put(o, "sent", JInt(static_cast<int64_t>(rt.sent.size())));
    Put(o, "rejected", JBool(rt.isRejected));
    Put(o, "message", rt.message.empty() ? JNull() : JStr(rt.message));
    return o;
}

void Describe(const app::App &a, JsonValue &out)
{
    Put(out, "refresh_ms", JInt(a.layers.config.refreshMs));
    Put(out, "ttl_ms", JInt(overlay::DrawTtlMs(a.layers.config.refreshMs)));
    JsonValue iface = JObj();
    Put(iface, "enabled", JBool(a.overlayEnabled));
    Put(iface, "all_labels", JBool(a.overlayShowAllLabels));
    Put(out, "iface", std::move(iface));
    JsonValue list = JArr();
    for (int i = 0; i < overlay::kLayerCount; ++i)
    {
        Push(list, LayerJson(a, i));
    }
    Put(out, "layers", std::move(list));
    Put(out, "label_budget", JInt(a.layers.config.labelBudget));
    Put(out, "labels_shown", JInt(a.layers.labels.given));
    Put(out, "labels_wanted", JInt(a.layers.labels.wanted));
    Put(out, "rpcs_last_refresh", JInt(a.layers.rpcsLastRefresh));
    Put(out, "agent_mode", JStr(overlay::AgentModeToken(a.layers.agentMode)));
    Put(out, "rpc_connected", JBool(a.rpc.IsConnected()));
}

}   // namespace

bool CmdLayers(app::App &a, const JsonValue &args, JsonValue &out, std::string &outError)
{
    const std::string action = ArgStr(args, "action");
    if (!action.empty() && action != "get" && action != "set" && action != "retry")
    {
        outError = "layers action is get, set or retry";
        return false;
    }
    LayerEdit edit{ a.layers.config, a.overlayEnabled, a.overlayShowAllLabels };
    bool      isChanged = false;
    for (const auto &kv : args.objVal)
    {
        if (kv.first == "action" || kv.first == "cmd" || kv.first == "req_id")
        {
            continue;
        }
        if (!ApplyArg(edit, kv.first, kv.second, outError))
        {
            return false;   // nothing applied: the edit was on a copy
        }
        isChanged = true;
    }
    if (action == "retry")
    {
        for (int i = 0; i < overlay::kLayerCount; ++i)
        {
            overlay::RetryLayer(a.layers, static_cast<LayerId>(i));
        }
    }
    if (isChanged)
    {
        a.layers.config        = edit.config;
        a.overlayEnabled       = edit.isIfaceOn;
        a.overlayShowAllLabels = edit.isIfaceAllLabels;
        overlay::NotifyLayerConfigChanged(a.layers);
        ImGui::MarkIniSettingsDirty();
    }
    Describe(a, out);
    return true;
}

}
