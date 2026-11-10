#include "anim/anim_graph_compiler.h"

#include "anim/anim_data.h"
#include "core/json.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace fx::anim {

uint32_t AnimDataSource::skeletonCount() const { return data_.skeletonCount(); }

std::string AnimDataSource::skeletonName(uint32_t skeleton) const { return data_.name(data_.skeleton(skeleton).name); }

int32_t AnimDataSource::clip(const std::string& name, uint32_t skeleton) const
{
    for (uint32_t c = 0; c < data_.clipCount(); ++c)
        if (data_.clip(c).skeleton == skeleton && name == data_.name(data_.clip(c).name))
            return int32_t(c);
    return -1;
}

void AnimDataSource::bones(uint32_t skeleton, std::vector<std::string>& names, std::vector<int32_t>& parents) const
{
    names.clear();
    parents.clear();
    if (skeleton >= data_.skeletonCount())
        return;
    const fmt::Skeleton& s = data_.skeleton(skeleton);
    const fmt::Bone* b = data_.bones(s);
    for (uint32_t i = 0; i < s.boneCount; ++i) {
        names.push_back(data_.name(b[i].name));
        parents.push_back(b[i].parent);
    }
}

namespace {

class GraphCompiler {
public:
    GraphCompiler(const CompileSource& source, const Json& graph, CompiledGraph& out) : source_(source), graph_(graph), out_(out) {}

    void compile()
    {
        out_ = {};
        out_.name = str(graph_, "name", "graph");
        const Json* sk = graph_.get("skeleton");
        skeleton_ = 0;
        if (sk && sk->type == Json::Type::Number)
            skeleton_ = uint32_t(sk->number);
        else if (sk && sk->type == Json::Type::String) {
            skeleton_ = ~0u;
            // Its name, or a model's source (a library's skeletons are "<source>#<skin>"): its first.
            for (uint32_t i = 0; i < source_.skeletonCount() && skeleton_ == ~0u; ++i) {
                const std::string name = source_.skeletonName(i);
                if (name == sk->str || (name.size() > sk->str.size() && name.compare(0, sk->str.size(), sk->str) == 0 &&
                                        name[sk->str.size()] == '#'))
                    skeleton_ = i;
            }
        }
        if (skeleton_ >= source_.skeletonCount()) {
            error("", "unknown skeleton");
            return;
        }

        if (const Json* params = graph_.get("parameters")) {
            for (const auto& [name, spec] : params->object) {
                const std::string type = spec.type == Json::Type::String ? spec.str : str(spec, "type", "float");
                if (type == "entity") {
                    out_.entityParameters.push_back(name);
                    continue;
                }
                const Json* def = spec.isObject() ? spec.get("default") : nullptr;
                fmt::GraphNode n = blank(type == "bool" ? fmt::NodeType::ParamBool
                                         : type == "id" ? fmt::NodeType::ParamId
                                                        : fmt::NodeType::ParamFloat);
                if (type != "bool" && type != "id" && type != "float")
                    error("$" + name, "unknown parameter type '" + type + "'");
                if (def && def->type == Json::Type::Bool)
                    n.flags = def->b ? 1 : 0;
                if (def && def->type == Json::Type::Number)
                    n.f[0] = float(def->number);
                if (def && def->type == Json::Type::String)
                    n.id = fmt::hashId(def->str.c_str());
                parameters_[name] = add(n, "$" + name);
                out_.parameterNames.push_back(name);
            }
        }
        nodes_ = graph_.get("nodes");
        const Json* root = graph_.get("root");
        if (!root) {
            error("", "graph has no root");
            return;
        }
        out_.root = node(*root, "root");
        out_.skeleton = skeleton_;
        if (const Json* ik = graph_.get("ik"))
            parseIk(*ik, out_.entityParameters,
                    [&](const std::string& name) {
                        const auto it = parameters_.find(name);
                        return it != parameters_.end() && out_.nodes[size_t(it->second)].type != fmt::NodeType::ParamId;
                    },
                    out_.ik, out_.errors);
        if (out_.nodes.size() > 32767)
            error("", "too many nodes");
    }

private:
    static std::string str(const Json& j, const char* key, const char* fallback)
    {
        const Json* v = j.isObject() ? j.get(key) : nullptr;
        return v && v->type == Json::Type::String ? v->str : fallback;
    }
    static float num(const Json& j, const char* key, float fallback)
    {
        const Json* v = j.get(key);
        return v && v->type == Json::Type::Number ? float(v->number) : fallback;
    }
    static bool flag(const Json& j, const char* key, bool fallback)
    {
        const Json* v = j.get(key);
        return v && v->type == Json::Type::Bool ? v->b : fallback;
    }
    static fmt::GraphNode blank(fmt::NodeType t)
    {
        fmt::GraphNode n = {};
        n.type = t;
        n.n[0] = n.n[1] = n.n[2] = n.n[3] = -1;
        return n;
    }
    static bool isPose(const std::string& type)
    {
        return type == "clip" || type == "blend1d" || type == "blend2d" || type == "speedScale" || type == "selector" ||
               type == "reference" || type == "stateMachine" || type == "layers";
    }
    void error(const std::string& node, const std::string& text) { out_.errors.push_back({node, text}); }
    int16_t add(const fmt::GraphNode& n, const std::string& id)
    {
        out_.nodes.push_back(n);
        out_.nodeIds.push_back(id);
        return int16_t(out_.nodes.size() - 1);  // > 32767 nodes is reported once compiled
    }
    int16_t add(const fmt::GraphNode& n) { return add(n, path_); }
    // Stands in for a node with errors, so the rest of the graph still compiles (and is checked).
    int16_t placeholder(bool pose, const std::string& id)
    {
        if (pose)
            return add(blank(fmt::NodeType::ReferencePose), id);
        return add(blank(fmt::NodeType::ConstFloat), id);
    }
    uint32_t list(const std::vector<uint32_t>& values)
    {
        const uint32_t at = uint32_t(out_.pool.size());
        out_.pool.insert(out_.pool.end(), values.begin(), values.end());
        return at;
    }
    static uint32_t bits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    std::string sub(const std::string& key) const { return path_ + "/" + key; }
    // Optional node-or-constant input.
    int16_t optional(const Json& j, const char* key)
    {
        const Json* v = j.get(key);
        return v ? node(*v, sub(key)) : int16_t(-1);
    }
    int16_t required(const Json& j, const char* key)
    {
        const Json* v = j.get(key);
        if (!v)
            throw std::runtime_error(std::string("missing '") + key + "'");
        return node(*v, sub(key));
    }

    int16_t node(const Json& j, const std::string& path)
    {
        if (j.type == Json::Type::String && !j.str.empty() && j.str[0] == '$') {
            auto it = parameters_.find(j.str.substr(1));
            if (it != parameters_.end())
                return it->second;
            if (std::find(out_.entityParameters.begin(), out_.entityParameters.end(), j.str.substr(1)) != out_.entityParameters.end())
                error(path, "entity parameter " + j.str + " is not a value (the graph's IK reads it)");
            else
                error(path, "unknown parameter " + j.str);
            return placeholder(false, path);
        }
        if (j.type == Json::Type::String && !j.str.empty() && j.str[0] == '@')
            return reference(j.str.substr(1), path);
        if (j.type == Json::Type::Number) {
            fmt::GraphNode n = blank(fmt::NodeType::ConstFloat);
            n.f[0] = float(j.number);
            return add(n, path);
        }
        if (j.type == Json::Type::Bool) {
            fmt::GraphNode n = blank(fmt::NodeType::ConstBool);
            n.flags = j.b ? 1 : 0;
            return add(n, path);
        }
        if (!j.isObject()) {
            error(path, "expected a node");
            return placeholder(false, path);
        }
        const std::string type = str(j, "type", "");
        const std::string saved = path_;
        path_ = path;
        int16_t r;
        try {
            r = build(j, type);
        } catch (const std::exception& e) {
            error(path, e.what());
            r = placeholder(isPose(type), path);
        }
        path_ = saved;
        return r;
    }

    // An entry of `nodes`: compiled once, then shared (values only).
    int16_t reference(const std::string& id, const std::string& path)
    {
        auto memo = refs_.find(id);
        if (memo != refs_.end()) {
            const Ref& r = memo->second;
            if (r.pose) {
                error(path, "@" + id + " is a pose already used elsewhere: a pose has one parent");
                return placeholder(true, path);
            }
            if (r.state >= 0) {
                if (r.state != currentState_) {
                    error(path, "@" + id + " uses its state (stateCompleted, time) and can't be shared with another state");
                    return placeholder(false, path);
                }
                usesState_ = true;
            }
            return r.index;
        }
        if (inProgress_.count(id)) {
            error(path, "@" + id + " depends on itself");
            return placeholder(false, path);
        }
        const Json* def = nodes_ ? nodes_->get(id.c_str()) : nullptr;
        if (!def) {
            error(path, "unknown node @" + id);
            return placeholder(false, path);
        }
        inProgress_.insert(id);
        const bool outerUses = usesState_;
        usesState_ = false;
        const int16_t index = node(*def, id);
        refs_[id] = {index, def->isObject() && isPose(str(*def, "type", "")), usesState_ ? currentState_ : int16_t(-1)};
        usesState_ = outerUses || usesState_;
        inProgress_.erase(id);
        return index;
    }

    int16_t build(const Json& j, const std::string& type)
    {
        using N = fmt::NodeType;

        if (type == "clip") {
            fmt::GraphNode n = blank(N::Clip);
            const std::string name = str(j, "clip", "");
            const int32_t c = source_.clip(name, skeleton_);
            if (c < 0)
                throw std::runtime_error("unknown clip '" + name + "'");
            n.n[0] = int16_t(c);
            n.n[1] = optional(j, "reverse");
            n.n[2] = optional(j, "reset");
            n.n[3] = int16_t(num(j, "syncOffset", 0.0f));
            n.flags = (flag(j, "rootMotion", true) ? 1 : 0) | (flag(j, "loop", true) ? 2 : 0);
            n.f[0] = num(j, "speed", 1.0f);
            return add(n);
        }
        if (type == "blend1d") {
            const Json* sources = j.get("sources");
            if (!sources || sources->array.empty())
                throw std::runtime_error("blend1d needs sources");
            std::vector<uint32_t> nodes, values;
            for (size_t i = 0; i < sources->array.size(); ++i) {
                const Json& s = sources->array[i];
                const Json* pose = s.get("pose");
                if (!pose)
                    throw std::runtime_error("source " + std::to_string(i) + " has no pose");
                nodes.push_back(uint32_t(node(*pose, sub("sources/" + std::to_string(i) + "/pose"))));
                values.push_back(bits(num(s, "value", 0.0f)));
            }
            fmt::GraphNode n = blank(N::Blend1D);
            n.n[0] = required(j, "input");
            n.flags = flag(j, "loop", true) ? 1 : 0;
            nodes.insert(nodes.end(), values.begin(), values.end());
            n.list = list(nodes);
            n.listCount = uint32_t(nodes.size());
            return add(n);
        }
        if (type == "blend2d") {
            // Points (x, y) with a pose each: triangulated here (Delaunay), the hull kept for
            // inputs outside it. Pool: n, poses, xs, ys, triangle count, triangles (3 each), hull
            // count, hull (closed: its first point again at the end).
            const Json* sources = j.get("sources");
            if (!sources || sources->array.size() < 3)
                throw std::runtime_error("blend2d needs at least 3 sources");
            if (sources->array.size() > 32)
                throw std::runtime_error("blend2d takes at most 32 sources");
            std::vector<uint32_t> poses;
            std::vector<float> xs, ys;
            for (size_t i = 0; i < sources->array.size(); ++i) {
                const Json& src = sources->array[i];
                const Json* pose = src.get("pose");
                const Json* v = src.get("value");
                if (!pose)
                    throw std::runtime_error("source " + std::to_string(i) + " has no pose");
                if (!v || v->array.size() != 2)
                    throw std::runtime_error("source " + std::to_string(i) + " needs a value [x, y]");
                const float x = float(v->array[0].number), y = float(v->array[1].number);
                for (size_t k = 0; k < xs.size(); ++k)
                    if (std::fabs(xs[k] - x) < 1e-5f && std::fabs(ys[k] - y) < 1e-5f)
                        throw std::runtime_error("sources " + std::to_string(k) + " and " + std::to_string(i) + " are at the same point");
                xs.push_back(x);
                ys.push_back(y);
                poses.push_back(uint32_t(node(*pose, sub("sources/" + std::to_string(i) + "/pose"))));
            }
            // (on one line: no area to blend over)
            double span = 1e-9, area = 0.0;
            for (size_t i = 0; i < xs.size(); ++i)
                span = std::max({span, std::fabs(double(xs[i] - xs[0])), std::fabs(double(ys[i] - ys[0]))});
            for (size_t a = 1; a < xs.size(); ++a)
                for (size_t b = a + 1; b < xs.size(); ++b)
                    area = std::max(area, std::fabs(double(xs[a] - xs[0]) * double(ys[b] - ys[0]) - double(ys[a] - ys[0]) * double(xs[b] - xs[0])));
            if (area < 1e-6 * span * span)
                throw std::runtime_error("blend2d's points are on one line");
            std::vector<uint32_t> tris, hull;
            triangulate(xs, ys, tris, hull);
            std::vector<uint32_t> data = {uint32_t(poses.size())};
            data.insert(data.end(), poses.begin(), poses.end());
            for (const float x : xs)
                data.push_back(bits(x));
            for (const float y : ys)
                data.push_back(bits(y));
            data.push_back(uint32_t(tris.size() / 3));
            data.insert(data.end(), tris.begin(), tris.end());
            data.push_back(uint32_t(hull.size()));
            data.insert(data.end(), hull.begin(), hull.end());
            fmt::GraphNode n = blank(N::Blend2D);
            n.n[0] = required(j, "x");
            n.n[1] = required(j, "y");
            n.flags = flag(j, "loop", true) ? 1 : 0;
            n.list = list(data);
            n.listCount = uint32_t(data.size());
            return add(n);
        }
        if (type == "layers") {
            // A base pose and layers over it, each: pose, weight (node or constant), bone mask,
            // mode (0 overlay, 1 additive), flags (1 synchronized to the base, 2 its events left
            // out, 4 its root motion blended in). Pool: 6 words a layer: pose, weight node, mask
            // node, mode, flags, constant weight bits.
            fmt::GraphNode n = blank(N::Layers);
            n.n[0] = required(j, "base");
            const Json* layers = j.get("layers");
            std::vector<uint32_t> data;
            for (size_t i = 0; layers && i < layers->array.size(); ++i) {
                const Json& l = layers->array[i];
                const std::string at = "layers/" + std::to_string(i);
                const Json* pose = l.get("pose");
                if (!pose)
                    throw std::runtime_error("layer " + std::to_string(i) + " has no pose");
                data.push_back(uint32_t(node(*pose, sub(at + "/pose"))));
                const Json* w = l.get("weight");
                float constant = 1.0f;
                if (w && w->type == Json::Type::Number) {
                    constant = float(w->number);
                    data.push_back(uint32_t(int32_t(-1)));
                } else {
                    data.push_back(uint32_t(int32_t(w ? node(*w, sub(at + "/weight")) : int16_t(-1))));
                }
                const Json* mask = l.get("mask");
                data.push_back(uint32_t(int32_t(mask && mask->type != Json::Type::Null ? node(*mask, sub(at + "/mask")) : int16_t(-1))));
                const std::string mode = str(l, "mode", "overlay");
                if (mode != "overlay" && mode != "additive")
                    throw std::runtime_error("layer " + std::to_string(i) + ": mode is overlay or additive");
                data.push_back(mode == "additive" ? 1u : 0u);
                data.push_back((flag(l, "sync", false) ? 1u : 0u) | (flag(l, "ignoreEvents", false) ? 2u : 0u) |
                               (flag(l, "rootMotion", false) ? 4u : 0u));
                data.push_back(bits(constant));
            }
            n.list = list(data);
            n.listCount = uint32_t(data.size());
            return add(n);
        }
        if (type == "boneMask") {
            // Per-bone weights: a mask of the graph's "boneMasks" ("mask": its name), or its own
            // ("default", "bones": {name: weight}, "propagate": children take their parent's).
            const std::string named = str(j, "mask", "");
            const Json* spec = &j;
            if (!named.empty()) {
                const Json* masks = graph_.get("boneMasks");
                spec = masks ? masks->get(named.c_str()) : nullptr;
                if (!spec)
                    throw std::runtime_error("unknown bone mask '" + named + "'");
            }
            std::vector<std::string> names;
            std::vector<int32_t> parents;
            source_.bones(skeleton_, names, parents);
            std::vector<float> w(names.size(), float(spec->getNumber("default", 0.0)));
            std::vector<bool> set(names.size(), false);
            if (const Json* bones = spec->get("bones"))
                for (const auto& [bone, value] : bones->object) {
                    const auto it = std::find(names.begin(), names.end(), bone);
                    if (it == names.end())
                        throw std::runtime_error("no bone '" + bone + "' in the skeleton");
                    w[size_t(it - names.begin())] = std::clamp(float(value.number), 0.0f, 1.0f);
                    set[size_t(it - names.begin())] = true;
                }
            if (spec->getBool("propagate", true))
                for (size_t b = 0; b < names.size(); ++b)  // (parents precede children)
                    if (!set[b] && parents[b] >= 0 && set[size_t(parents[b])]) {
                        w[b] = w[size_t(parents[b])];
                        set[b] = true;
                    }
            std::vector<uint32_t> data;
            for (const float x : w)
                data.push_back(bits(x));
            fmt::GraphNode n = blank(N::BoneMask);
            n.list = list(data);
            n.listCount = uint32_t(data.size());
            return add(n);
        }
        if (type == "boneMaskBlend") {
            fmt::GraphNode n = blank(N::BoneMaskBlend);
            n.n[0] = required(j, "a");
            n.n[1] = required(j, "b");
            const Json* w = j.get("weight");
            if (w && w->type == Json::Type::Number)
                n.f[0] = float(w->number);
            else
                n.n[2] = optional(j, "weight");
            return add(n);
        }
        if (type == "speedScale") {
            fmt::GraphNode n = blank(N::SpeedScale);
            n.n[0] = required(j, "pose");
            const Json* in = j.get("input");
            if (in && in->type == Json::Type::Number)
                n.f[0] = float(in->number);
            else
                n.n[1] = optional(j, "input");
            return add(n);
        }
        if (type == "selector") {
            std::vector<uint32_t> pairs;
            const Json* options = j.get("options");
            for (size_t i = 0; options && i < options->array.size(); ++i) {
                const Json& o = options->array[i];
                const std::string at = "options/" + std::to_string(i);
                const Json* pose = o.get("pose");
                if (!pose)
                    throw std::runtime_error("option " + std::to_string(i) + " has no pose");
                pairs.push_back(uint32_t(node(*pose, sub(at + "/pose"))));
                pairs.push_back(uint32_t(node(o.get("when") ? *o.get("when") : Json{Json::Type::Bool, true}, sub(at + "/when"))));
            }
            fmt::GraphNode n = blank(N::Selector);
            n.list = list(pairs);
            n.listCount = uint32_t(pairs.size());
            return add(n);
        }
        if (type == "reference")
            return add(blank(N::ReferencePose));
        if (type == "stateMachine")
            return stateMachine(j);

        // values
        if (type == "and" || type == "or") {
            std::vector<uint32_t> inputs;
            const Json* in = j.get("inputs");
            for (size_t i = 0; in && i < in->array.size(); ++i)
                inputs.push_back(uint32_t(node(in->array[i], sub("inputs/" + std::to_string(i)))));
            fmt::GraphNode n = blank(type == "and" ? N::And : N::Or);
            n.list = list(inputs);
            n.listCount = uint32_t(inputs.size());
            return add(n);
        }
        if (type == "not") {
            fmt::GraphNode n = blank(N::Not);
            n.n[0] = required(j, "input");
            return add(n);
        }
        if (type == "compare") {
            fmt::GraphNode n = blank(N::FloatCompare);
            n.n[0] = required(j, "input");
            const Json* v = j.get("value");
            if (v && v->type == Json::Type::Number)
                n.f[0] = float(v->number);
            else
                n.n[1] = optional(j, "value");
            const std::string op = str(j, "op", ">=");
            n.flags = op == ">=" ? 0 : op == "<=" ? 1 : op == "~=" ? 2 : op == ">" ? 3 : op == "<" ? 4 : 0xff;
            if (n.flags == 0xff)
                throw std::runtime_error("bad compare op " + op);
            n.f[1] = num(j, "epsilon", 1e-4f);
            return add(n);
        }
        if (type == "range") {
            fmt::GraphNode n = blank(N::FloatRange);
            n.n[0] = required(j, "input");
            n.f[0] = num(j, "min", 0.0f);
            n.f[1] = num(j, "max", 1.0f);
            n.flags = flag(j, "inclusive", true) ? 1 : 0;
            return add(n);
        }
        if (type == "math") {
            static const char* ops[] = {"add", "sub", "mul", "div", "mod", "abs", "negate", "floor", "ceil", "int", "frac", "invfrac"};
            fmt::GraphNode n = blank(N::FloatMath);
            n.n[0] = required(j, "a");
            const Json* b = j.get("b");
            if (b && b->type == Json::Type::Number)
                n.f[0] = float(b->number);
            else
                n.n[1] = optional(j, "b");
            const std::string op = str(j, "op", "add");
            uint16_t code = 0xff;
            for (uint16_t k = 0; k < 12; ++k)
                if (op == ops[k])
                    code = k;
            if (code == 0xff)
                throw std::runtime_error("bad math op " + op);
            n.flags = uint16_t(code | (flag(j, "absResult", false) ? 0x100 : 0) | (flag(j, "negateResult", false) ? 0x200 : 0));
            return add(n);
        }
        if (type == "remap") {
            fmt::GraphNode n = blank(N::FloatRemap);
            n.n[0] = required(j, "input");
            const Json *from = j.get("from"), *to = j.get("to");
            if (!from || !to || from->array.size() != 2 || to->array.size() != 2)
                throw std::runtime_error("remap needs from: [a, b] and to: [c, d]");
            n.f[0] = float(from->array[0].number), n.f[1] = float(from->array[1].number);
            n.f[2] = float(to->array[0].number), n.f[3] = float(to->array[1].number);
            return add(n);
        }
        if (type == "clamp") {
            fmt::GraphNode n = blank(N::FloatClamp);
            n.n[0] = required(j, "input");
            n.f[0] = num(j, "min", 0.0f);
            n.f[1] = num(j, "max", 1.0f);
            return add(n);
        }
        if (type == "id") {
            fmt::GraphNode n = blank(N::ConstId);
            n.id = fmt::hashId(str(j, "value", "").c_str());
            return add(n);
        }
        if (type == "idCompare") {
            std::vector<uint32_t> ids;
            for (const Json& id : j.get("ids") ? j.get("ids")->array : std::vector<Json>{})
                ids.push_back(fmt::hashId(id.str.c_str()));
            fmt::GraphNode n = blank(N::IdCompare);
            n.n[0] = required(j, "input");
            n.flags = flag(j, "not", false) ? 1 : 0;
            n.list = list(ids);
            n.listCount = uint32_t(ids.size());
            return add(n);
        }
        if (type == "event" || type == "eventPercent") {
            // This update's events: any (or all) of `ids`; or how far through the event `id` is.
            fmt::GraphNode n = blank(type == "event" ? N::EventCondition : N::EventPercent);
            std::vector<uint32_t> ids;
            if (const Json* list = j.get("ids"))
                for (const Json& id : list->array)
                    ids.push_back(fmt::hashId(id.str.c_str()));
            if (type == "event" && ids.empty())
                throw std::runtime_error("event needs ids");
            n.id = fmt::hashId(str(j, "id", "").c_str());
            if (type == "eventPercent" && str(j, "id", "").empty())
                throw std::runtime_error("eventPercent needs an id");
            n.flags = uint16_t((flag(j, "all", false) ? 1 : 0) | (flag(j, "inactive", false) ? 2 : 0));
            n.list = list(ids);
            n.listCount = uint32_t(ids.size());
            return add(n);
        }
        if (type == "stateCompleted" || type == "time") {
            if (currentState_ < 0)
                throw std::runtime_error(type + " is only valid in a transition condition");
            usesState_ = true;
            fmt::GraphNode n = blank(type == "time" ? N::TimeCondition : N::StateCompleted);
            n.n[0] = currentState_;
            if (type == "stateCompleted") {
                n.f[0] = num(j, "duration", 0.0f);
            } else {
                const std::string cmp = str(j, "comparison", "percent");
                const std::string op = str(j, "op", ">=");
                const uint16_t c = cmp == "percent" ? 0 : cmp == "syncEvent" ? 1 : 2;
                const uint16_t o = op == "<" ? 0 : op == "<=" ? 1 : op == ">" ? 2 : 3;
                n.flags = uint16_t(c | o << 4);
                const Json* v = j.get("value");
                if (v && v->type == Json::Type::Number)
                    n.f[0] = float(v->number);
                else
                    n.n[1] = optional(j, "value");
            }
            return add(n);
        }
        throw std::runtime_error(type.empty() ? "node has no type" : "unknown node type '" + type + "'");
    }

    int16_t stateMachine(const Json& j)
    {
        const Json* states = j.get("states");
        if (!states || states->object.empty())
            throw std::runtime_error("stateMachine needs states");
        if (states->object.size() > 64)
            throw std::runtime_error("a state machine has at most 64 states");
        const std::string machine = path_;
        // States first (transitions and conditions reference them), then their transitions.
        std::map<std::string, int32_t> index;
        std::vector<int16_t> stateNodes;
        for (const auto& [name, s] : states->object) {
            fmt::GraphNode n = blank(fmt::NodeType::State);
            n.id = fmt::hashId(name.c_str());  // (scripts: Animation.InState)
            const Json* pose = s.get("pose");
            if (pose && pose->type != Json::Type::Null)
                n.n[0] = node(*pose, machine + ":" + name + "/pose");
            // Its events: "events": {"entry": [...], "exit": [...], "fullyIn": [...]} (ids).
            std::vector<uint32_t> events;
            const Json* ev = s.get("events");
            for (const char* key : {"entry", "exit", "fullyIn"}) {
                const Json* l = ev && ev->isObject() ? ev->get(key) : nullptr;
                events.push_back(l ? uint32_t(l->array.size()) : 0u);
                for (size_t k = 0; l && k < l->array.size(); ++k)
                    events.push_back(fmt::hashId(l->array[k].str.c_str()));
            }
            n.list = list(events);
            n.listCount = uint32_t(events.size());
            index[name] = int32_t(stateNodes.size());
            stateNodes.push_back(add(n, machine + ":" + name));
        }
        std::vector<uint32_t> data;
        uint32_t s = 0;
        for (const auto& [name, st] : states->object) {
            const std::string state = machine + ":" + name;
            const int16_t saved = currentState_;
            currentState_ = stateNodes[s];
            const Json* entryJson = st.get("entry");
            const int16_t entry = entryJson ? node(*entryJson, state + "/entry") : int16_t(-1);
            std::vector<uint32_t> transitions;
            std::map<std::string, int> seen;
            const Json* list = st.get("transitions");
            for (size_t k = 0; list && k < list->array.size(); ++k) {
                const Json& t = list->array[k];
                const std::string to = str(t, "to", "");
                std::string id = str(t, "id", "");
                if (id.empty()) {
                    id = state + "->" + to;
                    if (const int dup = seen[to]++)
                        id += "#" + std::to_string(dup);
                }
                auto it = index.find(to);
                if (it == index.end()) {
                    error(id, "unknown state '" + to + "'");
                    continue;
                }
                fmt::GraphNode tn = blank(fmt::NodeType::Transition);
                tn.n[0] = stateNodes[it->second];
                path_ = id;
                const Json* d = t.get("duration");
                if (d && d->type == Json::Type::Number)
                    tn.f[0] = float(d->number);
                else
                    tn.n[1] = optional(t, "duration");
                const Json* off = t.get("offset");
                if (off && off->type == Json::Type::Number)
                    tn.f[1] = float(off->number);
                else
                    tn.n[2] = optional(t, "offset");
                path_ = machine;
                uint16_t o = 0;
                o |= flag(t, "clamp", false) ? 1u << 0 : 0;
                o |= flag(t, "sync", false) ? 1u << 1 : 0;
                const std::string match = str(t, "match", "");
                if (!match.empty())
                    o |= 1u << 2;
                o |= match == "syncEventIndex" ? 1u << 3 : 0;
                o |= match == "syncEventId" ? 1u << 4 : 0;
                o |= flag(t, "matchPercentage", match == "syncEventIndex" || match == "syncEventId") ? 1u << 5 : 0;
                o |= flag(t, "preferClosestSyncEventId", false) ? 1u << 6 : 0;
                o |= match == "seconds" ? 1u << 7 : 0;
                o |= flag(t, "offsetInSeconds", false) ? 1u << 8 : 0;
                tn.flags = o;
                const std::string easing = str(t, "easing", "linear");
                const std::string rm = str(t, "rootMotion", "blend");
                tn.extra = (easing == "smooth" ? 1u : 0u) |
                           (rm == "additive" ? 1u : rm == "ignoreSource" ? 2u : rm == "ignoreTarget" ? 3u : 0u) << 8;
                const int16_t transitionNode = add(tn, id);
                const Json* when = t.get("when");
                const int16_t condition = when ? node(*when, id + "/when") : int16_t(-1);
                transitions.insert(transitions.end(), {uint32_t(it->second), uint32_t(transitionNode), uint32_t(int32_t(condition)),
                                                       flag(t, "forceable", false) ? 1u : 0u});
            }
            currentState_ = saved;
            data.push_back(uint32_t(stateNodes[s]));
            data.push_back(uint32_t(int32_t(entry)));
            data.push_back(uint32_t(transitions.size() / 4));
            data.insert(data.end(), transitions.begin(), transitions.end());
            ++s;
        }
        fmt::GraphNode n = blank(fmt::NodeType::StateMachine);
        const std::string def = str(j, "default", "");
        if (!def.empty() && !index.count(def))
            error(machine, "unknown default state '" + def + "'");
        n.n[0] = int16_t(index.count(def) ? index[def] : 0);
        n.list = list(data);
        n.listCount = uint32_t(data.size());
        return add(n, machine);
    }

    // Delaunay triangles (counter-clockwise) of up to 32 points, brute force (a circumcircle with no
    // other point inside), and their convex hull (monotone chain, closed).
    static void triangulate(const std::vector<float>& px, const std::vector<float>& py, std::vector<uint32_t>& tris,
                            std::vector<uint32_t>& hull)
    {
        const size_t n = px.size();
        // Nudged a little by index (deterministically): four points on one circle (a grid) then
        // give one of the two triangulations, not both.
        std::vector<double> x(n), y(n);
        double span = 1e-9;
        for (size_t i = 0; i < n; ++i)
            span = std::max({span, std::fabs(double(px[i])), std::fabs(double(py[i]))});
        for (size_t i = 0; i < n; ++i) {
            x[i] = px[i] + span * 1e-6 * double(i);
            y[i] = py[i] + span * 1e-6 * double(i * i % 7);
        }
        const auto cross = [&](size_t a, size_t b, size_t c) { return (x[b] - x[a]) * (y[c] - y[a]) - (y[b] - y[a]) * (x[c] - x[a]); };
        for (size_t a = 0; a < n; ++a)
            for (size_t b = a + 1; b < n; ++b)
                for (size_t c = b + 1; c < n; ++c) {
                    double area = cross(a, b, c);
                    if (std::fabs(area) < 1e-12 * span * span)
                        continue;
                    size_t i0 = a, i1 = b, i2 = c;
                    if (area < 0)
                        std::swap(i1, i2);
                    bool empty = true;
                    for (size_t d = 0; d < n && empty; ++d) {
                        if (d == i0 || d == i1 || d == i2)
                            continue;
                        // In the circumcircle of a counter-clockwise triangle: the determinant > 0.
                        const double ax = x[i0] - x[d], ay = y[i0] - y[d], bx = x[i1] - x[d], by = y[i1] - y[d];
                        const double cx = x[i2] - x[d], cy = y[i2] - y[d];
                        const double det = (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
                                           (cx * cx + cy * cy) * (ax * by - bx * ay);
                        empty = det <= 0.0;
                    }
                    if (empty)
                        tris.insert(tris.end(), {uint32_t(i0), uint32_t(i1), uint32_t(i2)});
                }
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return x[a] < x[b] || (x[a] == x[b] && y[a] < y[b]); });
        std::vector<size_t> h;
        for (int pass = 0; pass < 2; ++pass) {
            const size_t start = h.size();
            for (size_t k = 0; k < n; ++k) {
                const size_t p = pass ? order[n - 1 - k] : order[k];
                while (h.size() >= start + 2 && cross(h[h.size() - 2], h.back(), p) <= 0)
                    h.pop_back();
                h.push_back(p);
            }
            h.pop_back();
        }
        for (const size_t p : h)
            hull.push_back(uint32_t(p));
        if (!hull.empty())
            hull.push_back(hull.front());
    }

    struct Ref {
        int16_t index;
        bool pose;
        int16_t state;  // the state its stateCompleted/time nodes read (-1: none)
    };

    const CompileSource& source_;
    const Json& graph_;
    CompiledGraph& out_;
    const Json* nodes_ = nullptr;
    uint32_t skeleton_ = 0;
    std::map<std::string, int16_t> parameters_;
    std::map<std::string, Ref> refs_;
    std::set<std::string> inProgress_;
    std::string path_;
    int16_t currentState_ = -1;
    bool usesState_ = false;
};

}  // namespace

bool parseIk(const Json& ik, const std::vector<std::string>& entities, const std::function<bool(const std::string&)>& isValue,
             IkSettings& out, std::vector<CompileMessage>& errors)
{
    out = {};
    const size_t before = errors.size();
    const auto fail = [&](const std::string& text) { errors.push_back({"ik", text}); };
    if (!ik.isObject()) {
        fail("'ik' is not an object");
        return false;
    }
    // "$Entity" (empty: none, when optional).
    const auto target = [&](const Json& part, const char* key, const std::string& what, bool needed, std::string& into) {
        const std::string ref = part.getString(key);
        if (ref.empty()) {
            if (needed)
                fail(what + " has no " + key);
            return;
        }
        const std::string name = ref[0] == '$' ? ref.substr(1) : ref;
        if (std::find(entities.begin(), entities.end(), name) == entities.end())
            fail(what + ": " + ref + " is not an entity parameter");
        else
            into = name;
    };
    const auto weight = [&](const Json& part, const std::string& what, IkWeight& into) {
        const Json* w = part.get("weight");
        if (!w)
            return;
        if (w->type == Json::Type::Number) {
            into.value = std::clamp(float(w->number), 0.0f, 1.0f);
        } else if (w->type == Json::Type::String && w->str.size() > 1 && w->str[0] == '$') {
            if (!isValue || !isValue(w->str.substr(1)))
                fail(what + ": weight " + w->str + " is not a float or bool parameter");
            else
                into.parameter = w->str.substr(1);
        } else {
            fail(what + ": weight is a number or a \"$Parameter\"");
        }
    };
    const auto string = [](const Json& part, const char* key, const char* fallback) {
        const std::string s = part.getString(key);
        return s.empty() ? std::string(fallback) : s;
    };
    for (const auto& [key, part] : ik.object) {
        if (key == "editor")
            continue;
        if (key == "reach") {
            if (part.type != Json::Type::Array) {
                fail("'reach' is a list");
                continue;
            }
            for (const Json& r : part.array) {
                IkSettings::Reach reach;
                if (!r.isObject() || r.getString("bone").empty()) {
                    fail("a reach has no bone");
                    continue;
                }
                reach.bone = r.getString("bone");
                target(r, "target", "reach " + reach.bone, true, reach.target);
                weight(r, "reach " + reach.bone, reach.weight);
                if (out.reaches.size() < 4)
                    out.reaches.push_back(std::move(reach));
                else
                    fail("at most 4 reaches");
            }
            continue;
        }
        if (!part.isObject()) {
            fail("'" + key + "' is not an object");
            continue;
        }
        if (key == "feet") {
            out.feet.on = true;
            out.feet.reach = std::clamp(float(part.getNumber("reach", 0.45)), 0.01f, 2.0f);
            weight(part, "feet", out.feet.weight);
        } else if (key == "look") {
            out.look.on = true;
            target(part, "target", "look", true, out.look.target);
            out.look.bone = string(part, "bone", "head");
            weight(part, "look", out.look.weight);
        } else if (key == "aim") {
            out.aim.on = true;
            target(part, "target", "aim", false, out.aim.target);
            target(part, "hold", "aim", false, out.aim.hold);
            if (part.getString("target").empty() && part.getString("hold").empty())
                fail("aim has no target or hold");
            out.aim.bone = string(part, "bone", "hand_r");
            out.aim.support = part.getString("support");
            out.aim.spine = std::clamp(float(part.getNumber("spine", 1.0)), 0.0f, 1.0f);
            if (const Json* a = part.get("axis"); a && a->type == Json::Type::Array && a->array.size() == 3)
                out.aim.axis = {float(a->array[0].number), float(a->array[1].number), float(a->array[2].number)};
            else if (a)
                fail("aim: axis is [x, y, z]");
            weight(part, "aim", out.aim.weight);
        } else {
            fail("unknown IK part '" + key + "'");
        }
    }
    return errors.size() == before;
}

bool compileGraph(const Json& graph, const CompileSource& source, CompiledGraph& out)
{
    GraphCompiler(source, graph, out).compile();
    return out.ok();
}

bool compileGraph(std::string_view text, const CompileSource& source, CompiledGraph& out)
{
    Json graph;
    try {
        graph = Json::parse(text);
    } catch (const std::exception& e) {
        out = {};
        out.errors.push_back({"", e.what()});
        return false;
    }
    return compileGraph(graph, source, out);
}

}  // namespace fx::anim
