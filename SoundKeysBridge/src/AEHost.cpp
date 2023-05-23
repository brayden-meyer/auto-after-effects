#include "AEHost.hpp"
#include "WindowsIO.hpp"
#include "AE_EffectUI.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <utility>

namespace skb {

static void check(A_Err err, const char* call) {
    if (err) {
        throw Error("ae_error", std::string(call) + " returned " + std::to_string(err));
    }
}

#define AE_CHECK(expression) check((expression), #expression)

// Explicit frozen suite versions: every suite below was available before 2018.
// Adobe's AE_GeneralPlug.h includes AE_GeneralPlugOld.h for older suite layouts.
template <class T>
class Suite {
public:
    Suite(SPBasicSuite* basic, const char* name, int version)
        : basic_(basic),
          name_(name),
          version_(version) {
        const void* raw = nullptr;
        const auto err = basic->AcquireSuite(name, version, &raw);
        if (err || !raw) {
            throw Error(
                "missing_suite",
                std::string(name) + " version " + std::to_string(version));
        }
        suite_ = static_cast<const T*>(raw);
    }

    ~Suite() {
        if (suite_) {
            basic_->ReleaseSuite(name_, version_);
        }
    }

    Suite(const Suite&) = delete;
    Suite& operator=(const Suite&) = delete;

    const T* operator->() const { return suite_; }

private:
    SPBasicSuite* basic_;
    const char* name_;
    int version_;
    const T* suite_ = nullptr;
};

struct Host {
    AEGP_PluginID id;
    Suite<AEGP_MemorySuite1> mem;
    Suite<AEGP_ProjSuite6> proj;
    Suite<AEGP_ItemSuite8> item;
    Suite<AEGP_CompSuite9> comp;
    Suite<AEGP_LayerSuite7> layer;
    Suite<AEGP_EffectSuite3> effect;
    Suite<AEGP_StreamSuite4> stream;
    Suite<AEGP_DynamicStreamSuite4> dynamic;
    Suite<AEGP_KeyframeSuite4> keys;
    Suite<AEGP_UtilitySuite5> utility;

    Host(SPBasicSuite* basic, AEGP_PluginID pluginId)
        : id(pluginId),
          mem(basic, kAEGPMemorySuite, kAEGPMemorySuiteVersion1),
          proj(basic, kAEGPProjSuite, kAEGPProjSuiteVersion6),
          item(basic, kAEGPItemSuite, kAEGPItemSuiteVersion8),
          comp(basic, kAEGPCompSuite, kAEGPCompSuiteVersion9),
          layer(basic, kAEGPLayerSuite, kAEGPLayerSuiteVersion7),
          effect(basic, kAEGPEffectSuite, kAEGPEffectSuiteVersion3),
          stream(basic, kAEGPStreamSuite, kAEGPStreamSuiteVersion4),
          dynamic(basic, kAEGPDynamicStreamSuite, kAEGPDynamicStreamSuiteVersion4),
          keys(basic, kAEGPKeyframeSuite, kAEGPKeyframeSuiteVersion4),
          utility(basic, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion5) {}
};

struct Mem {
    Host& h;
    AEGP_MemHandle value = nullptr;

    explicit Mem(Host& host) : h(host) {}

    ~Mem() {
        if (value) {
            h.mem->AEGP_FreeMemHandle(value);
        }
    }

    Mem(const Mem&) = delete;
    Mem& operator=(const Mem&) = delete;

    std::wstring text() {
        if (!value) {
            return {};
        }

        const A_UTF16Char* p = nullptr;
        AE_CHECK(h.mem->AEGP_LockMemHandle(value, &p));

        std::wstring decoded;
        try {
            static_assert(sizeof(A_UTF16Char) == sizeof(wchar_t), "Windows UTF-16 required");
            decoded.assign(reinterpret_cast<const wchar_t*>(p));
        } catch (...) {
            h.mem->AEGP_UnlockMemHandle(value);
            throw;
        }

        AE_CHECK(h.mem->AEGP_UnlockMemHandle(value));
        return decoded;
    }
};

struct Effect {
    Host& h;
    AEGP_EffectRefH value = nullptr;

    explicit Effect(Host& host) : h(host) {}

    ~Effect() {
        if (value) {
            h.effect->AEGP_DisposeEffect(value);
        }
    }

    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
};

struct Stream {
    Host& h;
    AEGP_StreamRefH value = nullptr;

    explicit Stream(Host& host) : h(host) {}

    Stream(Host& host, AEGP_EffectRefH e, int i) : h(host) {
        AE_CHECK(h.stream->AEGP_GetNewEffectStreamByIndex(h.id, e, i, &value));
    }

    ~Stream() {
        if (value) {
            h.stream->AEGP_DisposeStream(value);
        }
    }

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
};

struct Value {
    Host& h;
    AEGP_StreamValue2 value = {};
    bool owned = false;

    explicit Value(Host& host) : h(host) {}

    ~Value() {
        if (owned) {
            h.stream->AEGP_DisposeStreamValue(&value);
        }
    }

    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
};

struct Undo {
    Host& h;
    bool active = false;

    explicit Undo(Host& host) : h(host) {
        AE_CHECK(h.utility->AEGP_StartUndoGroup("Sound Keys Bridge Apply"));
        active = true;
    }

    void close() {
        if (active) {
            active = false;
            AE_CHECK(h.utility->AEGP_EndUndoGroup());
        }
    }

    ~Undo() {
        if (active) {
            h.utility->AEGP_EndUndoGroup();
        }
    }
};

struct Param {
    int index;
    PF_ParamType type;
    std::string name;
    std::string match;
    A_Err streamError = 0;
};

static double seconds(const A_Time& time) {
    if (!time.scale) {
        throw Error("bad_time", "Zero time scale");
    }
    return static_cast<double>(time.value) / time.scale;
}

static std::vector<Param> parameters(Host& h, AEGP_EffectRefH effect) {
    A_long count = 0;
    AE_CHECK(h.stream->AEGP_GetEffectNumParamStreams(effect, &count));
    if (count < 1 || count > 100000) {
        throw Error("bad_effect", "Invalid parameter count");
    }

    std::vector<Param> result;
    for (int i = 0; i < count; ++i) {
        Param p = {};
        p.index = i;
        AE_CHECK(h.effect->AEGP_GetEffectParamUnionByIndex(h.id, effect, i, &p.type, nullptr));

        Stream s(h);
        p.streamError = h.stream->AEGP_GetNewEffectStreamByIndex(h.id, effect, i, &s.value);
        if (!p.streamError) {
            Mem name(h);
            const auto nameError = h.stream->AEGP_GetStreamName(h.id, s.value, TRUE, &name.value);
            if (!nameError) {
                p.name = win::utf8(name.text());
            }

            char match[AEGP_MAX_STREAM_MATCH_NAME_SIZE] = {};
            if (!h.dynamic->AEGP_GetMatchName(s.value, match)) {
                p.match = match;
            }
        }
        result.push_back(p);
    }
    return result;
}

static std::string inventory(const std::vector<Param>& params) {
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        if (i) {
            out << ',';
        }
        out << "{\"native_index\":" << param.index
            << ",\"pf_type\":" << static_cast<int>(param.type)
            << ",\"name\":" << jsonString(param.name)
            << ",\"match\":" << jsonString(param.match)
            << ",\"stream_error\":" << param.streamError
            << '}';
    }
    out << ']';
    return out.str();
}

static const Param& byName(const std::vector<Param>& params, const std::string& name) {
    const Param* found = nullptr;
    for (const auto& param : params) {
        if (param.name == name) {
            if (found) {
                throw Error("ambiguous_parameter", "Multiple parameters named " + name);
            }
            found = &param;
        }
    }
    if (!found) {
        throw Error("missing_parameter", "No parameter named " + name);
    }
    return *found;
}

static const Param& command(const std::vector<Param>& params, const Profile& profile) {
    const Param* found = nullptr;
    for (const auto& param : params) {
        if (profile.commandIndex >= 0 && param.index != profile.commandIndex) {
            continue;
        }
        if (!profile.commandMatch.empty() && param.match != profile.commandMatch) {
            continue;
        }
        if (param.name != profile.commandName) {
            continue;
        }
        if (found) {
            throw Error("ambiguous_command", "Command identity matched multiple parameters");
        }
        found = &param;
    }
    if (!found) {
        throw Error(
            "missing_command",
            "No command matches the profile identity; numeric match-name suffixes are not native indices");
    }
    if (profile.mode == "standard_button" && found->type != PF_Param_BUTTON) {
        throw Error("wrong_parameter_type", "Standard activation requires PF_Param_BUTTON");
    }
    if (profile.mode == "custom_event" && found->type != PF_Param_NO_DATA) {
        throw Error("wrong_parameter_type", "This custom Commands profile requires PF_Param_NO_DATA");
    }
    return *found;
}

static void dispatch(
    Host& h,
    AEGP_EffectRefH effect,
    const A_Time& layerTime,
    const Param& commandParam,
    const Profile& profile) {
    if (profile.mode == "standard_button") {
        PF_UserChangedParamExtra extra = {};
        extra.param_index = commandParam.index;
        AE_CHECK(h.effect->AEGP_EffectCallGeneric(h.id, effect, &layerTime, PF_Cmd_USER_CHANGED_PARAM, &extra));
        return;
    }

    PF_EventExtra extra = {};
    extra.e_type = PF_Event_DO_CLICK;
    extra.evt_in_flags = PF_EI_DONT_DRAW;
#ifdef PF_USE_NEW_WINDOW_UNION
    auto& effectWindow = extra.window_union.effect_win;
#else
    auto& effectWindow = extra.effect_win;
#endif
    effectWindow.index = commandParam.index;
    effectWindow.area = PF_EA_CONTROL;
    effectWindow.current_frame.left = static_cast<A_short>(profile.left);
    effectWindow.current_frame.top = static_cast<A_short>(profile.top);
    effectWindow.current_frame.right = static_cast<A_short>(profile.left + profile.width);
    effectWindow.current_frame.bottom = static_cast<A_short>(profile.top + profile.height);
    effectWindow.param_title_frame = effectWindow.current_frame;
    extra.u.do_click.when = GetTickCount();
    extra.u.do_click.screen_point.h = static_cast<A_short>(profile.left + profile.clickX);
    extra.u.do_click.screen_point.v = static_cast<A_short>(profile.top + profile.clickY);
    extra.u.do_click.num_clicks = 1;
    extra.u.do_click.last_time = FALSE;
    AE_CHECK(h.effect->AEGP_EffectCallGeneric(h.id, effect, &layerTime, PF_Cmd_EVENT, &extra));

    if (extra.u.do_click.send_drag) {
        if (!profile.releaseDrag) {
            throw Error(
                "drag_requested",
                "Effect requested a drag sequence not enabled in the profile");
        }

        // Preserve continue_refcon returned by the target, and issue one terminal drag.
        extra.e_type = PF_Event_DRAG;
        extra.evt_out_flags = PF_EO_NONE;
        extra.u.do_click.when = GetTickCount();
        extra.u.do_click.send_drag = FALSE;
        extra.u.do_click.last_time = TRUE;
        AE_CHECK(h.effect->AEGP_EffectCallGeneric(h.id, effect, &layerTime, PF_Cmd_EVENT, &extra));
    }
}

RunResult applySoundKeys(
    SPBasicSuite* basic,
    AEGP_PluginID id,
    const Job& job,
    const Profile& profile,
    const std::function<void(const std::string&)>& phase) {
    Host h(basic, id);
    phase("preflight");

    const auto input = win::canonical(win::wide(job.input));
    const auto output = win::canonical(win::wide(job.output));
    win::requireNewAep(output);
    if (win::samePath(input, output)) {
        throw Error("same_path", "Input and output must be distinct");
    }

    A_long projects = 0;
    AE_CHECK(h.proj->AEGP_GetNumProjects(&projects));
    if (projects != 1) {
        throw Error("wrong_project", "Exactly one project must be open");
    }

    AEGP_ProjectH project = nullptr;
    AE_CHECK(h.proj->AEGP_GetProjectByIndex(0, &project));
    Mem projectPath(h);
    AE_CHECK(h.proj->AEGP_GetProjectPath(project, &projectPath.value));
    if (!win::samePath(projectPath.text(), input)) {
        throw Error("wrong_project", "Open project does not match input_project");
    }

    A_Boolean dirty = FALSE;
    AE_CHECK(h.proj->AEGP_ProjectIsDirty(project, &dirty));
    if (dirty) {
        throw Error("dirty_project", "Save the prepared project before submitting the job");
    }

    AEGP_ItemH item = nullptr;
    AE_CHECK(h.item->AEGP_GetFirstProjItem(project, &item));
    AEGP_CompH comp = nullptr;
    while (item) {
        A_long itemId = 0;
        AE_CHECK(h.item->AEGP_GetItemID(item, &itemId));
        if (itemId == job.compId) {
            AE_CHECK(h.comp->AEGP_GetCompFromItem(item, &comp));
            break;
        }

        AEGP_ItemH next = nullptr;
        AE_CHECK(h.item->AEGP_GetNextProjItem(project, item, &next));
        item = next;
    }
    if (!comp) {
        throw Error("missing_comp", "comp_id does not identify a composition");
    }

    A_long layerCount = 0;
    AE_CHECK(h.layer->AEGP_GetCompNumLayers(comp, &layerCount));
    if (job.layerIndex > layerCount) {
        throw Error("missing_layer", "Layer index exceeds composition layer count");
    }

    AEGP_LayerH layer = nullptr;
    AE_CHECK(h.layer->AEGP_GetCompLayerByIndex(comp, job.layerIndex - 1, &layer));
    Mem layerName(h);
    AE_CHECK(h.layer->AEGP_GetLayerName(id, layer, &layerName.value, nullptr));
    if (win::utf8(layerName.text()) != job.layerName) {
        throw Error("wrong_layer", "Layer name does not match queued identity");
    }

    A_long effectCount = 0;
    AE_CHECK(h.effect->AEGP_GetLayerNumEffects(layer, &effectCount));
    if (job.effectIndex > effectCount) {
        throw Error("missing_effect", "Effect index exceeds layer effect count");
    }

    Effect effect(h);
    AE_CHECK(h.effect->AEGP_GetLayerEffectByIndex(id, layer, job.effectIndex - 1, &effect.value));
    AEGP_InstalledEffectKey installed = AEGP_InstalledEffectKey_NONE;
    AE_CHECK(h.effect->AEGP_GetInstalledKeyFromLayerEffect(effect.value, &installed));

    char match[AEGP_MAX_EFFECT_MATCH_NAME_SIZE] = {};
    AE_CHECK(h.effect->AEGP_GetEffectMatchName(installed, match));
    if (profile.effectMatch != match) {
        throw Error("wrong_effect", "Installed effect match name does not match profile");
    }

    AEGP_EffectFlags flags = 0;
    AE_CHECK(h.effect->AEGP_GetEffectFlags(effect.value, &flags));
    if (flags & AEGP_EffectFlags_MISSING) {
        throw Error("missing_effect", "Effect is missing");
    }

    const auto params = parameters(h, effect.value);
    RunResult result;
    result.inventoryJson = inventory(params);
    result.route = profile.mode;
    phase("inventory:" + result.inventoryJson);

    const auto& commandParam = command(params, profile);
    std::vector<int> outputIndices;
    for (int outputNumber : profile.outputs) {
        const auto& param = byName(params, "Output " + std::to_string(outputNumber));
        Stream s(h, effect.value, param.index);

        AEGP_StreamType type = AEGP_StreamType_NO_DATA;
        AE_CHECK(h.stream->AEGP_GetStreamType(s.value, &type));
        if (type != AEGP_StreamType_OneD) {
            throw Error("wrong_output", "Output must be a scalar stream");
        }

        A_Boolean expression = FALSE;
        AE_CHECK(h.stream->AEGP_GetExpressionState(id, s.value, &expression));
        if (expression) {
            throw Error("output_expression", "Output stream has an enabled expression");
        }

        A_long count = 0;
        AE_CHECK(h.keys->AEGP_GetStreamNumKFs(s.value, &count));
        if (count != 0) {
            throw Error(
                "stale_keys",
                "Selected Output streams must be empty in the saved prepared project");
        }
        outputIndices.push_back(param.index);
    }

    A_Time start = {};
    A_Time duration = {};
    A_Time layerTime = {};
    AE_CHECK(h.comp->AEGP_GetCompWorkAreaStart(comp, &start));
    AE_CHECK(h.comp->AEGP_GetCompWorkAreaDuration(comp, &duration));
    if (seconds(duration) <= 0) {
        throw Error("bad_work_area", "Work area must have positive duration");
    }
    AE_CHECK(h.layer->AEGP_ConvertCompToLayerTime(layer, &start, &layerTime));

    // Nothing mutating occurs before this point. Dispatch may block synchronously.
    Undo undo(h);
    phase("dispatching");
    dispatch(h, effect.value, layerTime, commandParam, profile);

    phase("verifying");
    std::vector<Output> outputs;
    for (size_t i = 0; i < outputIndices.size(); ++i) {
        // Obtain fresh stream references after the target may have edited keys.
        Stream s(h, effect.value, outputIndices[i]);
        A_long count = 0;
        AE_CHECK(h.keys->AEGP_GetStreamNumKFs(s.value, &count));
        if (count < 0 || count > profile.maxKeys) {
            throw Error("verification_failed", "Invalid or excessive keyframe count");
        }

        Output o;
        o.number = profile.outputs[i];
        o.keys.reserve(count);
        for (A_long k = 0; k < count; ++k) {
            A_Time t = {};
            AE_CHECK(h.keys->AEGP_GetKeyframeTime(s.value, k, AEGP_LTimeMode_CompTime, &t));

            Value v(h);
            AE_CHECK(h.keys->AEGP_GetNewKeyframeValue(id, s.value, k, &v.value));
            v.owned = true;
            o.keys.push_back({seconds(t), v.value.val.one_d});
        }
        outputs.push_back(std::move(o));
    }

    verifyOutputs(profile, outputs, seconds(start), seconds(duration));
    result.keysJson = outputsJson(outputs);
    undo.close();

    phase("saving");
    // Recheck immediately before saving; controller must own this output namespace.
    win::requireNewAep(output);
    AE_CHECK(h.proj->AEGP_SaveProjectToPath(project, reinterpret_cast<const A_UTF16Char*>(output.c_str())));
    win::requireNonemptyFile(output);
    phase("saved");
    return result;
}

}  // namespace skb
