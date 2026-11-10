// Animation graph runtime, ported from Esoterica (MIT, Bobby Anguelov and contributors):
// Engine/Animation/Graph (Instance, Node, Context) and Graph/Nodes (AnimationClip, Blend1D,
// SpeedScale, Selector, Pose, State, StateMachine, Transition, StateConditions, Bools,
// Floats, IDs, Parameters). Sampled events, layers, bone masks, cached poses and recording
// are left out (see anim_graph.h).
//
// GraphNode field use per type (compiler: anim_graph_compiler.cpp):
//   Param*/Const*      Bool: flags bit 0; Float: f[0]; Id: id
//   And, Or            list: inputs                       Not: n[0]
//   FloatCompare       n[0] input, n[1] comparand (-1: f[0]), flags op (>= <= ~= > <), f[1] epsilon
//   FloatRange         n[0], f[0] begin, f[1] end, flags bit 0 inclusive
//   FloatMath          n[0] A, n[1] B (-1: f[0]), flags & 0xff op, bit 8 abs result, bit 9 negate result
//   FloatRemap         n[0], f[0..1] input range, f[2..3] output range
//   FloatClamp         n[0], f[0] min, f[1] max
//   IdCompare          n[0], flags bit 0 "doesn't match", list: IDs
//   StateCompleted     n[0] state, n[1] duration override, f[0] transition duration
//   TimeCondition      n[0] state, n[1] input (-1: f[0]), flags & 0xf comparison
//                      (% through state, % through sync event, seconds), flags >> 4 op (< <= > >=)
//   Clip               n[0] clip, n[1] play-in-reverse (Bool), n[2] reset time (Bool), n[3] start
//                      sync event offset, flags bit 0 sample root motion, bit 1 loop, f[0] speed
//   Blend1D            n[0] input, flags bit 0 loop, list: sources then their values (float bits)
//   SpeedScale         n[0] child, n[1] input (-1: f[0])
//   Selector           list: (option, condition) pairs
//   State              n[0] child (-1: off state)
//   State              n[0] child, list: entry, exit, fully-in event ids (each: count, ids)
//   EventCondition     list: event ids, flags bit 0 all (else any), bit 1 inactive branches too
//   EventPercent       id, flags bit 1 as above
//   Blend2D            n[0] x, n[1] y, flags bit 0 loop, list: count, poses, xs, ys, triangle count,
//                      triangles (3 each), hull count, hull (closed)
//   Layers             n[0] base, list: 6 per layer (pose, weight node, mask node, mode 0 overlay /
//                      1 additive, flags 1 sync / 2 no events / 4 root motion, constant weight bits)
//   BoneMask           list: a weight per bone (float bits)
//   BoneMaskBlend      n[0] mask a, n[1] mask b, n[2] weight (-1: f[0])
//   StateMachine       n[0] default state, list per state: node, entry condition, transition
//                      count, then (target state index, transition node, condition, forceable)
//   Transition         n[0] target state node, n[1] duration override, n[2] time offset override,
//                      n[3] target sync ID, f[0] duration, f[1] time offset, flags options,
//                      extra & 0xff easing (linear, smoothstep), extra >> 8 root motion blend

#include "anim/anim_graph.h"

#include "anim/anim_data.h"
#include "anim/anim_graph_compiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace fx::anim {

namespace {

bool nearZero(float v, float eps = 1e-4f) { return std::fabs(v) <= eps; }
float asFloat(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

const SyncTrack& defaultTrack()
{
    static const SyncTrack track;
    return track;
}

}  // namespace

// ---- node base

void Node::initialize(GraphContext& ctx)
{
    if (isInitialized())
        ++initCount_;
    else
        initializeInternal(ctx);
}

void Node::shutdown(GraphContext& ctx)
{
    if (initCount_ > 0 && --initCount_ == 0)
        shutdownInternal(ctx);
}

void Node::initializeInternal(GraphContext&) { ++initCount_; }
void Node::shutdownInternal(GraphContext&) { lastUpdate_ = 0xffffffffu; }

void PoseNode::initialize(GraphContext& ctx, const SyncTrackTime& initialTime)
{
    if (isInitialized())
        ++initCount_;
    else
        initializePose(ctx, initialTime);
}

void PoseNode::initializePose(GraphContext& ctx, const SyncTrackTime&)
{
    Node::initializeInternal(ctx);
    previous_ = current_ = 0.0f;
    duration_ = 0.0f;
}

const SyncTrack& PoseNode::syncTrack() const { return defaultTrack(); }

namespace {

// ---- values

class ParamNode final : public ValueNode {
public:
    bool b = false;
    float f = 0.0f;
    uint32_t id = 0;
    void link(GraphInstance&) override
    {
        b = (def->flags & 1) != 0;
        f = def->f[0];
        id = def->id;
    }
    bool getBool(GraphContext& ctx) override { return markActive(ctx), b; }
    float getFloat(GraphContext& ctx) override { return markActive(ctx), f; }
    uint32_t getId(GraphContext& ctx) override { return markActive(ctx), id; }
};

class ConstNode final : public ValueNode {
public:
    bool getBool(GraphContext&) override { return (def->flags & 1) != 0; }
    float getFloat(GraphContext&) override { return def->f[0]; }
    uint32_t getId(GraphContext&) override { return def->id; }
};

// Value nodes with inputs: init/shutdown them with the node, evaluate once per update.
class InputsNode : public ValueNode {
public:
    void link(GraphInstance& g) override
    {
        for (int i = 0; i < 2; ++i)
            in[i] = def->type == fmt::NodeType::StateCompleted || def->type == fmt::NodeType::TimeCondition
                        ? (i == 1 ? g.node<ValueNode>(def->n[1]) : nullptr)
                        : g.node<ValueNode>(def->n[i]);
        if (def->type == fmt::NodeType::And || def->type == fmt::NodeType::Or)
            for (uint32_t i = 0; i < def->listCount; ++i)
                list.push_back(g.node<ValueNode>(int32_t(g.pool(def->list)[i])));
    }

protected:
    void initializeInternal(GraphContext& ctx) override
    {
        ValueNode::initializeInternal(ctx);
        for (ValueNode* v : in)
            if (v)
                v->initialize(ctx);
        for (ValueNode* v : list)
            v->initialize(ctx);
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        for (ValueNode* v : list)
            v->shutdown(ctx);
        for (ValueNode* v : in)
            if (v)
                v->shutdown(ctx);
        ValueNode::shutdownInternal(ctx);
    }
    ValueNode* in[2] = {};
    std::vector<ValueNode*> list;
    bool bValue = false;
    float fValue = 0.0f;
};

class BoolLogicNode final : public InputsNode {
public:
    bool getBool(GraphContext& ctx) override
    {
        if (!wasUpdated(ctx)) {
            markActive(ctx);
            switch (def->type) {
            case fmt::NodeType::And:
                bValue = true;
                for (ValueNode* v : list)
                    if (!v->getBool(ctx)) {
                        bValue = false;
                        break;
                    }
                break;
            case fmt::NodeType::Or:
                bValue = false;
                for (ValueNode* v : list)
                    if (v->getBool(ctx)) {
                        bValue = true;
                        break;
                    }
                break;
            default:  // Not
                bValue = in[0] && !in[0]->getBool(ctx);
                break;
            }
        }
        return bValue;
    }
};

class FloatLogicNode final : public InputsNode {
public:
    bool getBool(GraphContext& ctx) override
    {
        if (!wasUpdated(ctx)) {
            markActive(ctx);
            const float v = in[0] ? in[0]->getFloat(ctx) : 0.0f;
            if (def->type == fmt::NodeType::FloatRange) {
                bValue = (def->flags & 1) ? (v >= def->f[0] && v <= def->f[1]) : (v > def->f[0] && v < def->f[1]);
            } else {  // FloatCompare
                const float c = in[1] ? in[1]->getFloat(ctx) : def->f[0];
                switch (def->flags) {
                case 0: bValue = v >= c; break;
                case 1: bValue = v <= c; break;
                case 2: bValue = std::fabs(v - c) <= def->f[1]; break;
                case 3: bValue = v > c; break;
                default: bValue = v < c; break;
                }
            }
        }
        return bValue;
    }
    float getFloat(GraphContext& ctx) override
    {
        if (!wasUpdated(ctx)) {
            markActive(ctx);
            const float a = in[0] ? in[0]->getFloat(ctx) : 0.0f;
            switch (def->type) {
            case fmt::NodeType::FloatRemap: {
                const float len = def->f[1] - def->f[0];
                const float t = len != 0.0f ? (a - def->f[0]) / len : 0.0f;
                fValue = def->f[2] + t * (def->f[3] - def->f[2]);
                break;
            }
            case fmt::NodeType::FloatClamp: fValue = std::clamp(a, def->f[0], def->f[1]); break;
            default: {  // FloatMath
                const float b = in[1] ? in[1]->getFloat(ctx) : def->f[0];
                float whole;
                switch (def->flags & 0xff) {
                case 0: fValue = a + b; break;
                case 1: fValue = a - b; break;
                case 2: fValue = a * b; break;
                case 3: fValue = nearZero(b) ? 0.0f : a / b; break;
                case 4: fValue = nearZero(b) ? 0.0f : std::fmod(a, b); break;
                case 5: fValue = std::fabs(a); break;
                case 6: fValue = -a; break;
                case 7: fValue = std::floor(a); break;
                case 8: fValue = std::ceil(a); break;
                case 9: std::modf(a, &whole), fValue = whole; break;
                case 10: fValue = std::modf(a, &whole); break;
                default: fValue = 1.0f - std::modf(a, &whole); break;
                }
                if (def->flags & 0x100)
                    fValue = std::fabs(fValue);
                if (def->flags & 0x200)
                    fValue = -fValue;
                break;
            }
            }
        }
        return fValue;
    }
};

class IdCompareNode final : public InputsNode {
public:
    void link(GraphInstance& g) override
    {
        InputsNode::link(g);
        ids = g.pool(def->list);
    }
    bool getBool(GraphContext& ctx) override
    {
        if (!wasUpdated(ctx)) {
            markActive(ctx);
            const uint32_t v = in[0] ? in[0]->getId(ctx) : 0;
            bool found = false;
            for (uint32_t i = 0; i < def->listCount; ++i)
                found |= ids[i] == v;
            if (def->flags & 1)
                bValue = !found;
            else
                bValue = def->listCount == 0 ? v == 0 : found;
        }
        return bValue;
    }
    const uint32_t* ids = nullptr;
};

// Event conditions (Esoterica's ID event condition and event percentage through): this update's
// sampled events so far (in a transition's condition: the source state's update is done).
//   EventCondition: list ids; flags bit 0: all of them (else any), bit 1: from inactive branches too
//   EventPercent:   id; how far through the strongest such event (-1: none), flags bit 1 as above
class EventNode final : public ValueNode {
public:
    void link(GraphInstance& g) override { ids = g.pool(def->list); }
    bool getBool(GraphContext& ctx) override
    {
        evaluate(ctx);
        return bValue;
    }
    float getFloat(GraphContext& ctx) override
    {
        evaluate(ctx);
        return fValue;
    }

private:
    bool counts(const GraphContext& ctx, const SampledEvent& e) const
    {
        return e.weight > 0.0f && (e.active || (def->flags & 2)) && ctx.events;
    }
    void evaluate(GraphContext& ctx)
    {
        if (wasUpdated(ctx))
            return;
        markActive(ctx);
        bValue = false;
        fValue = -1.0f;
        if (!ctx.events)
            return;
        if (def->type == fmt::NodeType::EventPercent) {
            float best = 0.0f;
            for (const SampledEvent& e : *ctx.events)
                if (e.id == def->id && counts(ctx, e) && e.weight > best) {
                    best = e.weight;
                    fValue = e.percent;
                }
            return;
        }
        const bool all = (def->flags & 1) != 0;
        bValue = all;
        for (uint32_t i = 0; i < def->listCount; ++i) {
            bool found = false;
            for (const SampledEvent& e : *ctx.events)
                found = found || (e.id == ids[i] && counts(ctx, e));
            if (all)
                bValue = bValue && found;
            else
                bValue = bValue || found;
        }
    }
    const uint32_t* ids = nullptr;
    bool bValue = false;
    float fValue = -1.0f;
};

class StateNodeBase;

class StateConditionNode final : public InputsNode {
public:
    void link(GraphInstance& g) override
    {
        InputsNode::link(g);
        state = g.node<PoseNode>(def->n[0]);
    }
    bool getBool(GraphContext& ctx) override;
    PoseNode* state = nullptr;
};

// ---- poses

PoseResult emptyResult() { return {}; }

class ClipNode final : public PoseNode {
public:
    void link(GraphInstance& g) override
    {
        clip = g.clip(def->n[0]);
        reverseNode = g.node<ValueNode>(def->n[1]);
        resetNode = g.node<ValueNode>(def->n[2]);
        if (clip && clip->skeleton() != g.skeleton())
            clip = nullptr;
        // Its sync track: the clip's sync events (none: one event over the whole clip).
        SyncTrack::Event markers[SyncTrack::kMaxEvents];
        int32_t count = 0;
        if (clip)
            for (const ClipSyncEvent& e : clip->syncEvents())
                if (count < SyncTrack::kMaxEvents)
                    markers[count++] = {e.id, e.start, 0.0f};
        track = SyncTrack(markers, count, def->n[3]);
    }
    bool isValid() const override { return clip != nullptr; }
    const SyncTrack& syncTrack() const override { return track; }

    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        if (!isValid())
            return emptyResult();
        markActive(ctx);
        bool looped = false;
        const bool singleFrame = clip->frameCount() <= 1;
        if (range) {
            if (singleFrame) {
                previous_ = current_ = 1.0f;
            } else {
                previous_ = track.percentageThrough(range->start);
                current_ = track.percentageThrough(range->end);
                looped = current_ < previous_;
            }
        } else {
            if (reverseNode && reverse != reverseNode->getBool(ctx)) {
                reverse = !reverse;
                current_ = 1.0f - current_;
                if (current_ == 1.0f)
                    current_ = 0.0f;
            }
            if (resetNode && resetNode->getBool(ctx))
                previous_ = current_ = 0.0f;
            if (singleFrame) {
                previous_ = firstUpdate ? 0.0f : 1.0f;
                current_ = 1.0f;
            } else {
                const float delta = duration_ > 0.0f ? ctx.deltaTime / duration_ : 0.0f;
                previous_ = current_;
                if (!(def->flags & 2)) {
                    current_ = clampPercentage(previous_ + delta, false);
                } else {
                    current_ += delta;
                    if (current_ > 1.0f) {
                        current_ = normalizedTime(current_);
                        looped = true;
                    }
                }
            }
        }
        sampleEvents(ctx, looped);

        PoseResult r;
        if (def->flags & 1) {
            if (reverse) {
                const float start = 1.0f - previous_, end = 1.0f - current_;
                // Esoterica: NoLooping(1, end) * NoLooping(start, 0), i.e. start->0 first.
                r.rootMotion = previous_ <= current_
                                   ? clip->rootMotion().deltaNoLooping(start, end)
                                   : combine(clip->rootMotion().deltaNoLooping(1.0f, end),
                                             clip->rootMotion().deltaNoLooping(start, 0.0f));
            } else {
                r.rootMotion = clip->rootMotion().deltaBetween(previous_, current_);
            }
        }
        r.task = ctx.tasks->sample(clip, reverse ? 1.0f - current_ : current_);
        firstUpdate = false;
        return r;
    }

protected:
    // The clip's events between its last and current time (the range played this update; wrapped
    // when it looped, mirrored when it plays in reverse): markers whose time is in it, events with
    // a duration that overlap it (how far through them it ends).
    void sampleEvents(GraphContext& ctx, bool looped)
    {
        if (!ctx.events || clip->events().empty() || current_ == previous_)
            return;
        const auto range = [&](float a, float b) {
            for (const ClipEvent& e : clip->events()) {
                float percent = 1.0f;
                if (e.duration <= 0.0f) {
                    if (e.start < a || e.start >= b)
                        continue;
                } else {
                    if (e.start >= b || e.start + e.duration <= a)
                        continue;
                    percent = std::clamp((b - e.start) / e.duration, 0.0f, 1.0f);
                }
                SampledEvent s;
                s.id = e.id;
                s.percent = percent;
                s.kind = e.kind == ClipEvent::Foot ? SampledEvent::Foot : SampledEvent::Marker;
                s.flags = e.foot == ClipEvent::Left ? SampledEvent::Left : e.foot == ClipEvent::Right ? SampledEvent::Right : 0;
                s.active = ctx.activeBranch;
                ctx.events->push_back(s);
            }
        };
        const float a = reverse ? 1.0f - current_ : previous_, b = reverse ? 1.0f - previous_ : current_;
        if (!looped)
            range(std::min(a, b), std::max(a, b));
        else if (!reverse) {
            range(previous_, 1.0f + 1e-6f);
            range(0.0f, current_);
        } else {
            range(0.0f, 1.0f - previous_);
            range(1.0f - current_, 1.0f + 1e-6f);
        }
    }

    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        if (reverseNode)
            reverseNode->initialize(ctx);
        if (resetNode)
            resetNode->initialize(ctx);
        if (clip) {
            duration_ = clip->duration() / std::max(def->f[0], 1e-4f);
            previous_ = current_ = track.percentageThrough(t);
        }
        reverse = false;
        firstUpdate = true;
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        if (resetNode)
            resetNode->shutdown(ctx);
        if (reverseNode)
            reverseNode->shutdown(ctx);
        PoseNode::shutdownInternal(ctx);
    }

    const Clip* clip = nullptr;
    ValueNode *reverseNode = nullptr, *resetNode = nullptr;
    SyncTrack track;
    bool reverse = false, firstUpdate = true;
};

class Blend1DNode final : public PoseNode {
public:
    void link(GraphInstance& g) override
    {
        input = g.node<ValueNode>(def->n[0]);
        const uint32_t n = def->listCount / 2;
        const uint32_t* p = g.pool(def->list);
        struct Point {
            uint32_t index;
            float value;
        };
        std::vector<Point> points;
        for (uint32_t i = 0; i < n; ++i) {
            sources.push_back(g.node<PoseNode>(int32_t(p[i])));
            points.push_back({i, asFloat(p[n + i])});
        }
        std::stable_sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.value < b.value; });
        for (size_t i = 0; i + 1 < points.size(); ++i)
            ranges.push_back({points[i].index, points[i + 1].index, points[i].value, points[i + 1].value});
        if (!points.empty())
            rangeLo = points.front().value, rangeHi = points.back().value;
    }
    bool isValid() const override { return input && sources.size() >= 1; }
    const SyncTrack& syncTrack() const override { return blended; }

    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        if (!isValid())
            return emptyResult();
        markActive(ctx);
        evaluate(ctx);
        SyncTrackTimeRange r;
        if (range) {
            r = *range;
        } else {
            const float delta = duration_ > 0.0f ? ctx.deltaTime / duration_ : 0.0f;
            const float to = clampPercentage(current_ + delta, (def->flags & 1) != 0);
            r.start = blended.time(current_);
            r.end = blended.time(to);
        }
        PoseResult result;
        if (!s1) {
            if (s0->isValid())
                result = s0->update(ctx, &r);
        } else {
            PoseResult r0, r1;
            const size_t e0 = ctx.events ? ctx.events->size() : 0;
            if (s0->isValid())
                r0 = s0->update(ctx, &r);
            const size_t e1 = ctx.events ? ctx.events->size() : 0;
            if (s1->isValid())
                r1 = s1->update(ctx, &r);
            if (ctx.events) {  // each input's events by its share
                const size_t e2 = ctx.events->size();
                for (size_t i = e0; i < e2; ++i)
                    (*ctx.events)[i].weight *= i < e1 ? 1.0f - weight : weight;
            }
            if (!r0.hasTask())
                r0.task = ctx.tasks->reference();
            if (!r1.hasTask())
                r1.task = ctx.tasks->reference();
            result.task = ctx.tasks->blend(r0.task, r1.task, weight);
            result.rootMotion = blendRootMotion(r0.rootMotion, r1.rootMotion, weight, BlendMode::Normal);
        }
        previous_ = blended.percentageThrough(r.start);
        current_ = blended.percentageThrough(r.end);
        return result;
    }

protected:
    void evaluate(GraphContext& ctx)
    {
        if (evaluatedUpdate == ctx.updateId)
            return;
        evaluatedUpdate = ctx.updateId;
        const float v = std::clamp(input->getFloat(ctx), rangeLo, rangeHi);
        if (ranges.empty()) {
            s0 = sources[0];
            s1 = nullptr;
            weight = 0.0f;
        } else {
            const Range* chosen = &ranges.back();
            for (const Range& rg : ranges)
                if (v >= rg.lo && v <= rg.hi) {
                    chosen = &rg;
                    break;
                }
            weight = chosen->hi > chosen->lo ? (v - chosen->lo) / (chosen->hi - chosen->lo) : 0.0f;
            if (nearZero(weight)) {
                s0 = sources[chosen->i0];
                s1 = nullptr;
            } else if (std::fabs(weight - 1.0f) <= 1e-4f) {
                s0 = sources[chosen->i1];
                s1 = nullptr;
            } else {
                s0 = sources[chosen->i0];
                s1 = sources[chosen->i1];
            }
        }
        if (!s1) {
            blended = SyncTrack(s0->syncTrack(), s0->syncTrack(), 0.0f);
            duration_ = s0->duration();
        } else {
            const SyncTrack &t0 = s0->syncTrack(), &t1 = s1->syncTrack();
            blended = SyncTrack(t0, t1, weight);
            duration_ = SyncTrack::durationSynchronized(s0->duration(), s1->duration(), t0.eventCount(), t1.eventCount(),
                                                        blended.eventCount(), weight);
        }
    }
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        input->initialize(ctx);
        for (PoseNode* s : sources)
            s->initialize(ctx, t);
        evaluatedUpdate = 0xffffffffu;
        if (isValid()) {
            evaluate(ctx);
            previous_ = current_ = blended.percentageThrough(t);
        }
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        for (PoseNode* s : sources)
            s->shutdown(ctx);
        input->shutdown(ctx);
        s0 = s1 = nullptr;
        PoseNode::shutdownInternal(ctx);
    }

    struct Range {
        uint32_t i0, i1;
        float lo, hi;
    };
    ValueNode* input = nullptr;
    std::vector<PoseNode*> sources;
    std::vector<Range> ranges;
    float rangeLo = 0.0f, rangeHi = 0.0f, weight = 0.0f;
    PoseNode *s0 = nullptr, *s1 = nullptr;
    SyncTrack blended;
    uint32_t evaluatedUpdate = 0xffffffffu;
};

// Blend2D (Esoterica Blend2DNode): the input point's triangle of the blend space (or, outside it,
// the nearest edge of its hull) gives up to three sources, blended as (s0 -> s1) -> s2, their
// sync tracks and durations alike; their events weighted by their shares.
class Blend2DNode final : public PoseNode {
public:
    void link(GraphInstance& g) override
    {
        inX = g.node<ValueNode>(def->n[0]);
        inY = g.node<ValueNode>(def->n[1]);
        const uint32_t* p = g.pool(def->list);
        const uint32_t n = def->listCount ? p[0] : 0;
        size_t i = 1;
        for (uint32_t k = 0; k < n; ++k)
            sources.push_back(g.node<PoseNode>(int32_t(p[i + k])));
        i += n;
        points.resize(n);
        for (uint32_t k = 0; k < n; ++k)
            points[k].x = asFloat(p[i + k]);
        i += n;
        for (uint32_t k = 0; k < n; ++k)
            points[k].y = asFloat(p[i + k]);
        i += n;
        const uint32_t triangles = p[i++];
        tris.assign(p + i, p + i + 3 * triangles);
        i += 3 * triangles;
        const uint32_t hullCount = p[i++];
        hull.assign(p + i, p + i + hullCount);
    }
    bool isValid() const override
    {
        if (!inX || !inY || sources.size() < 3)
            return false;
        for (const PoseNode* p : sources)
            if (!p)
                return false;
        return true;
    }
    const SyncTrack& syncTrack() const override { return blended; }

    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        if (!isValid())
            return emptyResult();
        markActive(ctx);
        evaluate(ctx);
        SyncTrackTimeRange r;
        if (range) {
            r = *range;
        } else {
            const float delta = duration_ > 0.0f ? ctx.deltaTime / duration_ : 0.0f;
            const float to = clampPercentage(current_ + delta, (def->flags & 1) != 0);
            r.start = blended.time(current_);
            r.end = blended.time(to);
        }
        PoseResult results[3];
        size_t marks[4] = {};
        marks[0] = ctx.events ? ctx.events->size() : 0;
        for (int k = 0; k < 3; ++k) {
            if (s[k] && s[k]->isValid())
                results[k] = s[k]->update(ctx, &r);
            if (s[k] && !results[k].hasTask())
                results[k].task = ctx.tasks->reference();
            marks[k + 1] = ctx.events ? ctx.events->size() : 0;
        }
        const float share[3] = {(1.0f - w01) * (1.0f - w12), w01 * (1.0f - w12), w12};
        if (ctx.events)
            for (int k = 0; k < 3; ++k)
                for (size_t e = marks[k]; e < marks[k + 1]; ++e)
                    (*ctx.events)[e].weight *= share[k];
        PoseResult result = results[0];
        if (s[1]) {
            result.task = ctx.tasks->blend(results[0].task, results[1].task, w01);
            result.rootMotion = blendRootMotion(results[0].rootMotion, results[1].rootMotion, w01, BlendMode::Normal);
        }
        if (s[2]) {
            result.task = ctx.tasks->blend(result.task, results[2].task, w12);
            result.rootMotion = blendRootMotion(result.rootMotion, results[2].rootMotion, w12, BlendMode::Normal);
        }
        previous_ = blended.percentageThrough(r.start);
        current_ = blended.percentageThrough(r.end);
        return result;
    }

protected:
    struct Point {
        float x = 0.0f, y = 0.0f;
    };
    // Esoterica's CalculateBlendSpaceWeights: s[0..2] (null: unused) and the weights between them.
    void evaluate(GraphContext& ctx)
    {
        if (evaluatedUpdate == ctx.updateId)
            return;
        evaluatedUpdate = ctx.updateId;
        const Point q = {inX->getFloat(ctx), inY->getFloat(ctx)};
        int idx[3] = {-1, -1, -1};
        w01 = w12 = 0.0f;
        bool inside = false;
        for (size_t t = 0; t + 2 < tris.size() && !inside; t += 3) {
            const Point &a = points[tris[t]], &b = points[tris[t + 1]], &c = points[tris[t + 2]];
            const float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::fabs(det) < 1e-12f)
                continue;
            const float l0 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / det;
            const float l1 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / det;
            const float l2 = 1.0f - l0 - l1;
            if (l0 < -1e-5f || l1 < -1e-5f || l2 < -1e-5f)
                continue;
            inside = true;
            struct IW {
                int i;
                float w;
            } iw[3] = {{int(tris[t]), std::max(l0, 0.0f)}, {int(tris[t + 1]), std::max(l1, 0.0f)}, {int(tris[t + 2]), std::max(l2, 0.0f)}};
            std::sort(iw, iw + 3, [](const IW& x, const IW& y) { return x.w < y.w; });
            if (std::fabs(iw[2].w - 1.0f) <= 1e-4f) {
                idx[0] = iw[2].i;
            } else {
                idx[0] = iw[0].i, idx[1] = iw[1].i, idx[2] = iw[2].i;
                w01 = iw[0].w + iw[1].w > 0.0f ? iw[1].w / (iw[0].w + iw[1].w) : 0.0f;
                w12 = iw[2].w;
            }
        }
        if (!inside && hull.size() >= 2) {  // the nearest point of the hull's edges
            float best = 1e30f, bestT = 0.0f;
            size_t edge = 0;
            for (size_t i = 1; i < hull.size(); ++i) {
                const Point &a = points[hull[i - 1]], &b = points[hull[i]];
                const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
                const float t = l2 > 0.0f ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
                const float ex = a.x + dx * t - q.x, ey = a.y + dy * t - q.y, d = ex * ex + ey * ey;
                if (d < best) {
                    best = d;
                    bestT = t;
                    edge = i - 1;
                }
            }
            idx[0] = int(hull[edge]);
            idx[1] = int(hull[edge + 1]);
            w01 = bestT;
        }
        // Simplified: a weight of 1 or 0 drops a source.
        if (idx[1] >= 0 && std::fabs(w01 - 1.0f) <= 1e-4f) {
            idx[0] = idx[1], idx[1] = idx[2], idx[2] = -1;
            w01 = w12, w12 = 0.0f;
        } else if (idx[1] >= 0 && nearZero(w01) && idx[2] < 0) {
            idx[1] = -1;
        }
        for (int k = 0; k < 3; ++k)
            s[k] = idx[k] >= 0 ? sources[size_t(idx[k])] : nullptr;
        if (!s[0]) {  // (no triangle and no hull: the first source)
            s[0] = sources[0];
            s[1] = s[2] = nullptr;
            w01 = w12 = 0.0f;
        }
        if (!s[1]) {
            blended = SyncTrack(s[0]->syncTrack(), s[0]->syncTrack(), 0.0f);
            duration_ = s[0]->duration();
            w01 = w12 = 0.0f;
            s[2] = nullptr;
            return;
        }
        const SyncTrack &t0 = s[0]->syncTrack(), &t1 = s[1]->syncTrack();
        const SyncTrack t01(t0, t1, w01);
        float d = SyncTrack::durationSynchronized(s[0]->duration(), s[1]->duration(), t0.eventCount(), t1.eventCount(), t01.eventCount(), w01);
        if (s[2]) {
            const SyncTrack& t2 = s[2]->syncTrack();
            blended = SyncTrack(t01, t2, w12);
            d = SyncTrack::durationSynchronized(d, s[2]->duration(), t01.eventCount(), t2.eventCount(), blended.eventCount(), w12);
        } else {
            blended = t01;
            w12 = 0.0f;
        }
        duration_ = d;
    }
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        inX->initialize(ctx);
        inY->initialize(ctx);
        for (PoseNode* p : sources)
            p->initialize(ctx, t);
        evaluatedUpdate = 0xffffffffu;
        if (isValid()) {
            evaluate(ctx);
            previous_ = current_ = blended.percentageThrough(t);
        }
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        for (PoseNode* p : sources)
            p->shutdown(ctx);
        inX->shutdown(ctx);
        inY->shutdown(ctx);
        s[0] = s[1] = s[2] = nullptr;
        PoseNode::shutdownInternal(ctx);
    }

    ValueNode *inX = nullptr, *inY = nullptr;
    std::vector<PoseNode*> sources;
    std::vector<Point> points;
    std::vector<uint32_t> tris, hull;
    PoseNode* s[3] = {};
    float w01 = 0.0f, w12 = 0.0f;
    SyncTrack blended;
    uint32_t evaluatedUpdate = 0xffffffffu;
};

// Layers (Esoterica's layer blend, local space): a base pose and layers over it, each blended in
// by its weight (and bone mask), overlaid or added; their events weighted alike (or left out).
// The base's time, sync track and root motion are the node's (a layer's root motion only when
// asked).
class LayersNode final : public PoseNode {
public:
    void link(GraphInstance& g) override
    {
        base = g.node<PoseNode>(def->n[0]);
        const uint32_t* p = g.pool(def->list);
        for (uint32_t i = 0; i + 6 <= def->listCount; i += 6)
            layers.push_back({g.node<PoseNode>(int32_t(p[i])), g.node<ValueNode>(int32_t(p[i + 1])), g.node<ValueNode>(int32_t(p[i + 2])),
                              p[i + 3], p[i + 4], asFloat(p[i + 5])});
    }
    bool isValid() const override { return base && base->isValid(); }
    const SyncTrack& syncTrack() const override { return isValid() ? base->syncTrack() : defaultTrack(); }

    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        if (!isValid())
            return emptyResult();
        markActive(ctx);
        PoseResult result = base->update(ctx, range);
        duration_ = base->duration();
        previous_ = base->previousTime();
        current_ = base->currentTime();
        for (const Layer& l : layers) {
            if (!l.pose || !l.pose->isValid())
                continue;
            const float w = std::clamp(l.weight ? l.weight->getFloat(ctx) : l.constant, 0.0f, 1.0f);
            // Synchronized: the base's range this update.
            SyncTrackTimeRange baseRange;
            const SyncTrackTimeRange* lr = nullptr;
            if (l.flags & 1) {
                baseRange = range ? *range
                                  : SyncTrackTimeRange{base->syncTrack().time(previous_), base->syncTrack().time(current_)};
                lr = &baseRange;
            }
            const size_t e0 = ctx.events ? ctx.events->size() : 0;
            const int8_t marker = ctx.tasks->marker();
            const PoseResult r = l.pose->update(ctx, lr);
            if (ctx.events)
                ctx.weightEvents(e0, (l.flags & 2) ? 0.0f : w);
            if (w <= 0.0f || !r.hasTask()) {
                ctx.tasks->rollback(marker);  // (its time moved on; its pose is not needed)
                continue;
            }
            const float* weights = l.mask ? l.mask->getMask(ctx) : nullptr;
            const int16_t mask = weights ? ctx.tasks->mask(weights) : int16_t(-1);
            if (!result.hasTask())
                result.task = ctx.tasks->reference();
            result.task = ctx.tasks->blend(result.task, r.task, w, l.mode == 1 ? BlendMode::Additive : BlendMode::Normal, mask);
            if (l.flags & 4)
                result.rootMotion = blendRootMotion(result.rootMotion, r.rootMotion, w, BlendMode::Normal);
        }
        return result;
    }

protected:
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        if (base)
            base->initialize(ctx, t);
        for (const Layer& l : layers) {
            if (l.pose)
                l.pose->initialize(ctx, t);
            if (l.weight)
                l.weight->initialize(ctx);
            if (l.mask)
                l.mask->initialize(ctx);
        }
        if (isValid()) {
            duration_ = base->duration();
            previous_ = base->previousTime();
            current_ = base->currentTime();
        }
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        for (const Layer& l : layers) {
            if (l.pose)
                l.pose->shutdown(ctx);
            if (l.weight)
                l.weight->shutdown(ctx);
            if (l.mask)
                l.mask->shutdown(ctx);
        }
        if (base)
            base->shutdown(ctx);
        PoseNode::shutdownInternal(ctx);
    }

    struct Layer {
        PoseNode* pose;
        ValueNode* weight;
        ValueNode* mask;
        uint32_t mode, flags;
        float constant;
    };
    PoseNode* base = nullptr;
    std::vector<Layer> layers;
};

// Bone masks: a weight per bone, as compiled (BoneMask), or two of them blended (BoneMaskBlend).
class BoneMaskNode final : public ValueNode {
public:
    void link(GraphInstance& g) override
    {
        if (def->type == fmt::NodeType::BoneMask) {
            if (g.skeleton() && def->listCount == g.skeleton()->boneCount())
                fixed = reinterpret_cast<const float*>(g.pool(def->list));
        } else {
            a = g.node<ValueNode>(def->n[0]);
            b = g.node<ValueNode>(def->n[1]);
            w = g.node<ValueNode>(def->n[2]);
            blended.assign(g.skeleton() ? g.skeleton()->boneCount() : 0, 0.0f);
        }
    }
    const float* getMask(GraphContext& ctx) override
    {
        if (def->type == fmt::NodeType::BoneMask)
            return fixed;
        if (!wasUpdated(ctx)) {
            markActive(ctx);
            const float* ma = a ? a->getMask(ctx) : nullptr;
            const float* mb = b ? b->getMask(ctx) : nullptr;
            const float t = std::clamp(w ? w->getFloat(ctx) : def->f[0], 0.0f, 1.0f);
            valid = ma && mb;
            for (size_t i = 0; valid && i < blended.size(); ++i)
                blended[i] = ma[i] + (mb[i] - ma[i]) * t;
        }
        return valid ? blended.data() : nullptr;
    }

protected:
    void initializeInternal(GraphContext& ctx) override
    {
        ValueNode::initializeInternal(ctx);
        for (ValueNode* v : {a, b, w})
            if (v)
                v->initialize(ctx);
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        for (ValueNode* v : {a, b, w})
            if (v)
                v->shutdown(ctx);
        ValueNode::shutdownInternal(ctx);
    }

    const float* fixed = nullptr;
    ValueNode *a = nullptr, *b = nullptr, *w = nullptr;
    std::vector<float> blended;
    bool valid = false;
};

class PassthroughNode : public PoseNode {
public:
    bool isValid() const override { return child && child->isValid(); }
    const SyncTrack& syncTrack() const override { return isValid() ? child->syncTrack() : defaultTrack(); }
    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        markActive(ctx);
        PoseResult r;
        if (isValid()) {
            r = child->update(ctx, range);
            copyTiming();
        }
        return r;
    }

protected:
    void copyTiming()
    {
        duration_ = child->duration();
        previous_ = child->previousTime();
        current_ = child->currentTime();
    }
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        if (child) {
            child->initialize(ctx, t);
            if (child->isValid())
                copyTiming();
        }
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        if (child)
            child->shutdown(ctx);
        PoseNode::shutdownInternal(ctx);
    }
    PoseNode* child = nullptr;
};

class SpeedScaleNode final : public PassthroughNode {
public:
    void link(GraphInstance& g) override
    {
        child = g.node<PoseNode>(def->n[0]);
        input = g.node<ValueNode>(def->n[1]);
    }
    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        markActive(ctx);
        const float scale = std::max(input ? input->getFloat(ctx) : def->f[0], 0.0f);
        PoseResult r;
        if (range) {  // time is dictated by the parent: only the reported duration changes
            r = PassthroughNode::update(ctx, range);
            if (isValid())
                duration_ = nearZero(scale) ? 0.0f : child->duration() / scale;
        } else {
            const float dt = ctx.deltaTime;
            ctx.deltaTime = nearZero(scale) ? 0.0f : dt * scale;
            r = PassthroughNode::update(ctx, nullptr);
            if (isValid())
                duration_ = nearZero(scale) ? 0.0f : child->duration() / scale;
            ctx.deltaTime = dt;
        }
        return r;
    }

protected:
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PassthroughNode::initializePose(ctx, t);
        if (input)
            input->initialize(ctx);
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        if (input)
            input->shutdown(ctx);
        PassthroughNode::shutdownInternal(ctx);
    }
    ValueNode* input = nullptr;
};

// Picks its option at initialization (first true condition), then passes through.
class SelectorNode final : public PassthroughNode {
public:
    void link(GraphInstance& g) override
    {
        const uint32_t* p = g.pool(def->list);
        for (uint32_t i = 0; i + 1 < def->listCount; i += 2) {
            options.push_back(g.node<PoseNode>(int32_t(p[i])));
            conditions.push_back(g.node<ValueNode>(int32_t(p[i + 1])));
        }
    }

protected:
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        child = nullptr;
        for (ValueNode* c : conditions)
            if (c)
                c->initialize(ctx);
        for (size_t i = 0; i < options.size() && !child; ++i)
            if (conditions[i] && conditions[i]->getBool(ctx))
                child = options[i];
        for (ValueNode* c : conditions)
            if (c)
                c->shutdown(ctx);
        if (child) {
            child->initialize(ctx, t);
            if (child->isValid())
                copyTiming();
        }
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        if (child)
            child->shutdown(ctx);
        child = nullptr;
        PoseNode::shutdownInternal(ctx);
    }
    std::vector<PoseNode*> options;
    std::vector<ValueNode*> conditions;
};

class ReferencePoseNode final : public PoseNode {
public:
    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange*) override
    {
        markActive(ctx);
        PoseResult r;
        r.task = ctx.tasks->reference();
        return r;
    }
};

// ---- state machine

class StateNode final : public PoseNode {
public:
    enum class Transitioning : uint8_t { None, In, Out };

    void link(GraphInstance& g) override
    {
        child = g.node<PoseNode>(def->n[0]);
        // Its events: [entry count, ids..., exit count, ids..., fully-in count, ids...].
        const uint32_t* p = g.pool(def->list);
        for (uint32_t i = 0, k = 0; k < 3 && i < def->listCount; ++k) {
            lists[k] = {p + i + 1, p[i]};
            i += 1 + p[i];
        }
    }
    bool isOffState() const { return child == nullptr; }
    const SyncTrack& syncTrack() const override { return child && child->isValid() ? child->syncTrack() : defaultTrack(); }

    void setTransitioning(Transitioning s)
    {
        transitioning = s;
        if (flagWord) {
            if (s != Transitioning::None)
                *flagWord |= flagBit;
            else
                *flagWord &= ~flagBit;
        }
    }
    void startTransitionOut()
    {
        setTransitioning(Transitioning::Out);
        exitPending = true;  // (its exit events: the next update, which has the context)
    }
    void startTransitionIn() { setTransitioning(Transitioning::In); }
    float elapsed() const { return elapsedTime; }

    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        markActive(ctx);
        elapsedTime += ctx.deltaTime;
        if (exitPending) {
            emit(ctx, 1, SampledEvent::Exit);
            exitPending = false;
        }
        if (transitioning == Transitioning::None)
            emit(ctx, 2, SampledEvent::FullyIn);
        PoseResult r;
        if (child && child->isValid()) {
            r = child->update(ctx, range);
            duration_ = child->duration();
            previous_ = child->previousTime();
            current_ = child->currentTime();
        }
        return r;
    }

    uint64_t* flagWord = nullptr;  // the state machine's "transitioning" bit set
    uint64_t flagBit = 0;

protected:
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        setTransitioning(Transitioning::None);
        elapsedTime = 0.0f;
        exitPending = false;
        emit(ctx, 0, SampledEvent::Entry);
        if (child) {
            child->initialize(ctx, t);
            if (child->isValid()) {
                duration_ = child->duration();
                previous_ = child->previousTime();
                current_ = child->currentTime();
            }
        }
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        if (child)
            child->shutdown(ctx);
        setTransitioning(Transitioning::None);
        PoseNode::shutdownInternal(ctx);
    }

    void emit(GraphContext& ctx, int list, uint8_t flag) const
    {
        if (!ctx.events)
            return;
        for (uint32_t i = 0; i < lists[list].count; ++i) {
            SampledEvent e;
            e.id = lists[list].ids[i];
            e.kind = SampledEvent::State;
            e.flags = flag;
            e.active = ctx.activeBranch;
            ctx.events->push_back(e);
        }
    }

    PoseNode* child = nullptr;
    Transitioning transitioning = Transitioning::None;
    float elapsedTime = 0.0f;
    bool exitPending = false;
    struct {
        const uint32_t* ids = nullptr;
        uint32_t count = 0;
    } lists[3];
};

bool StateConditionNode::getBool(GraphContext& ctx)
{
    if (!wasUpdated(ctx)) {
        markActive(ctx);
        if (def->type == fmt::NodeType::StateCompleted) {
            const float td = in[1] ? in[1]->getFloat(ctx) : def->f[0];
            if (!state || state->duration() == 0.0f)
                bValue = true;
            else if (nearZero(td))
                bValue = std::fabs(state->currentTime() - 1.0f) <= 1e-4f || state->currentTime() < state->previousTime();
            else
                bValue = state->currentTime() >= 1.0f - td / state->duration();
        } else {  // TimeCondition
            const float comparand = in[1] ? in[1]->getFloat(ctx) : def->f[0];
            float v = 0.0f;
            if (state) {
                switch (def->flags & 0xf) {
                case 0: v = state->currentTime(); break;
                case 1: v = state->syncTrack().time(state->currentTime()).percentageThrough; break;
                default: v = state->duration() * state->currentTime(); break;
                }
            }
            switch (def->flags >> 4) {
            case 0: bValue = v < comparand; break;
            case 1: bValue = v <= comparand; break;
            case 2: bValue = v > comparand; break;
            default: bValue = v >= comparand; break;
            }
        }
    }
    return bValue;
}

enum TransitionOption : uint16_t {
    kClampDuration = 1u << 0,
    kSynchronized = 1u << 1,
    kMatchSourceTime = 1u << 2,
    kMatchSyncEventIndex = 1u << 3,
    kMatchSyncEventId = 1u << 4,
    kMatchSyncEventPercentage = 1u << 5,
    kPreferClosestSyncEventId = 1u << 6,
    kMatchTimeInSeconds = 1u << 7,
    kOffsetTimeInSeconds = 1u << 8,
};

class TransitionNode final : public PoseNode {
public:
    struct StartOptions {
        PoseResult sourceResult;
        const SyncTrackTimeRange* updateRange = nullptr;
        PoseNode* source = nullptr;
        bool sourceIsTransition = false;
        int8_t sourceTasksMarker = 0;
    };

    void link(GraphInstance& g) override
    {
        target = g.node<StateNode>(def->n[0]);
        durationOverride = g.node<ValueNode>(def->n[1]);
        offsetOverride = g.node<ValueNode>(def->n[2]);
        syncIdNode = g.node<ValueNode>(def->n[3]);
    }
    bool has(uint16_t option) const { return (def->flags & option) != 0; }
    const SyncTrack& syncTrack() const override { return track; }
    StateNode* targetState() const { return target; }

    // Predictive: will this frame's update finish it?
    bool isComplete(const GraphContext& ctx) const
    {
        return transitionDuration <= 0.0f || progress + ctx.deltaTime / transitionDuration >= 1.0f;
    }

    PoseResult start(GraphContext& ctx, const StartOptions& o)
    {
        source = o.source;
        sourceIsTransition = o.sourceIsTransition;
        markActive(ctx);
        PoseResult sourceResult = o.sourceResult;

        auto startTransitionOutForSource = [&]() {
            const bool instant = transitionDuration == 0.0f;
            sourceState()->startTransitionOut();
            if (instant) {
                if (sourceIsTransition)
                    endSourceTransition(ctx);
                source->shutdown(ctx);
                source = nullptr;
                ctx.tasks->rollback(o.sourceTasksMarker);  // the source's pose is not needed
                sourceResult = {};
            }
        };

        SyncTrackTimeRange targetRange;
        PoseResult targetResult;
        const bool synchronized = has(kSynchronized) || o.updateRange;
        if (synchronized) {
            const SyncTrack& st = source->syncTrack();
            const SyncTrackTimeRange sourceRange{st.time(source->previousTime()), st.time(source->currentTime())};
            if (has(kClampDuration)) {
                const float realEnd = st.percentageThrough(st.endTime());
                const float cur = st.percentageThrough(sourceRange.start);
                const float d = realEnd > cur ? realEnd - cur : 1.0f - (cur - realEnd);
                if (source->duration() > 0.0f)
                    transitionDuration = std::min(transitionDuration, source->duration() * d);
            }
            syncEventOffset = o.updateRange ? 0.0f : std::floor(readOnce(ctx, offsetOverride, def->f[1]));
            targetRange = sourceRange;
            targetRange.start.eventIndex += int32_t(syncEventOffset);
            targetRange.end.eventIndex += int32_t(syncEventOffset);
            startTransitionOutForSource();
            target->initialize(ctx, targetRange.start);
            target->startTransitionIn();
            targetResult = target->update(ctx, &targetRange);
        } else {
            syncEventOffset = readOnce(ctx, offsetOverride, def->f[1]);
            const float sourceDuration = source->duration();
            if (has(kClampDuration) && sourceDuration > 0.0f)
                transitionDuration = std::min(transitionDuration, (1.0f - source->currentTime()) * sourceDuration);
            if (has(kMatchSourceTime) || !nearZero(syncEventOffset)) {
                SyncTrackTime targetStart;
                if (has(kMatchTimeInSeconds) || has(kOffsetTimeInSeconds)) {
                    const float sourceSeconds = has(kMatchTimeInSeconds) ? sourceDuration * source->currentTime() : 0.0f;
                    const float desired = sourceSeconds + syncEventOffset;
                    syncEventOffset = 0.0f;
                    target->initialize(ctx, SyncTrackTime{});
                    const float t = target->duration() == 0.0f ? 0.0f : std::clamp(desired / target->duration(), 0.0f, 1.0f);
                    targetStart = target->syncTrack().time(t);
                    target->shutdown(ctx);
                } else {
                    if (has(kMatchSourceTime)) {
                        const SyncTrack& st = source->syncTrack();
                        const SyncTrackTime from = st.time(source->currentTime());
                        if (has(kMatchSyncEventIndex)) {
                            targetStart.eventIndex = from.eventIndex;
                        } else if (has(kMatchSyncEventId)) {
                            uint32_t id = st.eventId(from.eventIndex);
                            if (syncIdNode) {
                                syncIdNode->initialize(ctx);
                                id = syncIdNode->getId(ctx);
                                syncIdNode->shutdown(ctx);
                            }
                            if (id != 0) {
                                target->initialize(ctx, targetStart);
                                const SyncTrack& tt = target->syncTrack();
                                targetStart.eventIndex = has(kPreferClosestSyncEventId) ? tt.closestEventIndexForId(from, id)
                                                                                         : tt.eventIndexForId(id);
                                target->shutdown(ctx);
                            }
                        }
                        if (has(kMatchSyncEventPercentage))
                            targetStart.percentageThrough = from.percentageThrough;
                    }
                    float index = 0.0f;
                    const float pct = std::fabs(std::modf(targetStart.toFloat() + syncEventOffset, &index));
                    targetStart = {int32_t(index), pct};
                }
                startTransitionOutForSource();
                target->initialize(ctx, targetStart);
                target->startTransitionIn();
                const float dt = ctx.deltaTime;
                ctx.deltaTime = 0.0f;  // build the pose at the start time without advancing
                targetResult = target->update(ctx, nullptr);
                ctx.deltaTime = dt;
            } else {
                startTransitionOutForSource();
                target->initialize(ctx, SyncTrackTime{});
                target->startTransitionIn();
                targetResult = target->update(ctx, nullptr);
            }
        }
        calculateBlendWeight();
        PoseResult result = blendResults(ctx, sourceResult, targetResult);
        if (!source) {
            previous_ = current_ = 0.0f;
            blendedDuration = target->duration();
            track = target->syncTrack();
        } else if (synchronized) {
            rebuildBlendedTrack();
            previous_ = track.percentageThrough(targetRange.start);
            current_ = track.percentageThrough(targetRange.end);
        } else {
            previous_ = current_ = 0.0f;
            blendedDuration = source->duration() + (target->duration() - source->duration()) * weight;
            track = target->syncTrack();
        }
        duration_ = target->duration();
        return result;
    }

    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* external) override
    {
        markActive(ctx);
        bool syncUpdate = false;
        SyncTrackTimeRange range;
        if (external) {
            syncUpdate = true;
            range = *external;
        } else if (source && has(kSynchronized)) {
            syncUpdate = true;
            const float delta = blendedDuration > 0.0f ? ctx.deltaTime / blendedDuration : 0.0f;
            range.start = track.time(current_);
            range.end = track.time(clampPercentage(current_ + delta, true));
        }
        if (sourceIsTransition && static_cast<TransitionNode*>(source)->isComplete(ctx))
            endSourceTransition(ctx);

        if (syncUpdate && has(kClampDuration))
            progress += track.percentageCovered(range) / std::max(transitionDuration, 1e-6f);
        else
            progress += transitionDuration > 0.0f ? ctx.deltaTime / transitionDuration : 1.0f;
        progress = std::clamp(progress, 0.0f, 1.0f);
        calculateBlendWeight();

        PoseResult sourceResult;
        const size_t eventsFrom = ctx.events ? ctx.events->size() : 0;
        if (source) {
            const bool wasActive = ctx.activeBranch;
            ctx.activeBranch = false;  // the source is the inactive branch
            if (syncUpdate) {
                const int32_t off = int32_t(syncEventOffset);
                SyncTrackTimeRange sourceRange{{range.start.eventIndex - off, range.start.percentageThrough},
                                               {range.end.eventIndex - off, range.end.percentageThrough}};
                if (has(kClampDuration) && progress == 1.0f) {
                    sourceRange.end.eventIndex = sourceRange.start.eventIndex;
                    sourceRange.end.percentageThrough = 1.0f;
                }
                sourceResult = source->update(ctx, &sourceRange);
            } else {
                sourceResult = source->update(ctx, nullptr);
            }
            ctx.activeBranch = wasActive;
        }
        const size_t eventsTarget = ctx.events ? ctx.events->size() : 0;
        const PoseResult targetResult = target->update(ctx, syncUpdate ? &range : nullptr);
        if (ctx.events) {
            const size_t end = ctx.events->size();
            for (size_t i = eventsFrom; i < end; ++i)
                (*ctx.events)[i].weight *= i < eventsTarget ? 1.0f - weight : weight;
        }
        PoseResult result = blendResults(ctx, sourceResult, targetResult);
        if (syncUpdate && source) {
            rebuildBlendedTrack();
            previous_ = track.percentageThrough(range.start);
            current_ = track.percentageThrough(range.end);
        } else {
            blendedDuration = (source ? source->duration() : 0.0f) +
                              (target->duration() - (source ? source->duration() : 0.0f)) * weight;
            if (blendedDuration > 0.0f) {
                previous_ = current_;
                current_ = clampPercentage(current_ + ctx.deltaTime / blendedDuration, true);
            } else {
                previous_ = current_ = 1.0f;
            }
        }
        duration_ = target->duration();
        return result;
    }

protected:
    float readOnce(GraphContext& ctx, ValueNode* node, float fallback)
    {
        if (!node)
            return fallback;
        node->initialize(ctx);
        const float v = node->getFloat(ctx);
        node->shutdown(ctx);
        return v;
    }
    StateNode* sourceState() const
    {
        return sourceIsTransition ? static_cast<TransitionNode*>(source)->target : static_cast<StateNode*>(source);
    }
    // Collapses a finished source transition into its target state.
    void endSourceTransition(GraphContext& ctx)
    {
        StateNode* state = static_cast<TransitionNode*>(source)->target;
        source->shutdown(ctx);
        source = state;
        sourceIsTransition = false;
        state->startTransitionOut();
    }
    void calculateBlendWeight()
    {
        if (transitionDuration == 0.0f) {
            weight = 1.0f;
            return;
        }
        const float t = progress;
        weight = std::clamp((def->extra & 0xff) == 1 ? t * t * (3.0f - 2.0f * t) : t, 0.0f, 1.0f);
    }
    void rebuildBlendedTrack()
    {
        const SyncTrack &st = source->syncTrack(), &tt = target->syncTrack();
        track = SyncTrack(st, tt, weight);
        blendedDuration = SyncTrack::durationSynchronized(source->duration(), target->duration(), st.eventCount(),
                                                          tt.eventCount(), track.eventCount(), weight);
    }
    PoseResult blendResults(GraphContext& ctx, const PoseResult& s, const PoseResult& t)
    {
        PoseResult r;
        if (!source) {
            return t;
        }
        if (s.hasTask() && t.hasTask()) {
            const uint32_t mode = def->extra >> 8;  // 0 blend, 1 additive, 2 ignore source, 3 ignore target
            r.rootMotion = mode == 2   ? t.rootMotion
                           : mode == 3 ? s.rootMotion
                                       : blendRootMotion(s.rootMotion, t.rootMotion, weight,
                                                         mode == 1 ? BlendMode::Additive : BlendMode::Normal);
            r.task = ctx.tasks->blend(s.task, t.task, weight);
        } else {
            r = s.hasTask() ? s : t;
        }
        return r;
    }
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        syncEventOffset = 0.0f;
        transitionDuration = def->f[0];
        if (durationOverride) {
            durationOverride->initialize(ctx);
            transitionDuration = std::clamp(durationOverride->getFloat(ctx), 0.0f, 10.0f);
            durationOverride->shutdown(ctx);
        }
        progress = 0.0f;
        weight = 0.0f;
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        target->setTransitioning(StateNode::Transitioning::None);  // the state machine owns the target
        current_ = 1.0f;
        if (source) {
            if (sourceIsTransition)
                endSourceTransition(ctx);
            source->shutdown(ctx);
            source = nullptr;
        }
        PoseNode::shutdownInternal(ctx);
    }

    StateNode* target = nullptr;
    PoseNode* source = nullptr;
    bool sourceIsTransition = false;
    ValueNode *durationOverride = nullptr, *offsetOverride = nullptr, *syncIdNode = nullptr;
    SyncTrack track;
    float progress = 0.0f, transitionDuration = 0.0f, syncEventOffset = 0.0f, weight = 0.0f, blendedDuration = 0.0f;
    float debugProgress() const override { return isInitialized() ? progress : -1.0f; }
};

class StateMachineNode final : public PoseNode {
public:
    struct TransitionInfo {
        int32_t targetState;
        TransitionNode* node;
        ValueNode* condition;
        bool forceable;
    };
    struct StateInfo {
        StateNode* node;
        ValueNode* entryCondition;
        std::vector<TransitionInfo> transitions;
    };

    void link(GraphInstance& g) override
    {
        const uint32_t* p = g.pool(def->list);
        for (uint32_t i = 0; i < def->listCount;) {
            StateInfo s{g.node<StateNode>(int32_t(p[i])), g.node<ValueNode>(int32_t(p[i + 1])), {}};
            const uint32_t count = p[i + 2];
            i += 3;
            for (uint32_t t = 0; t < count; ++t, i += 4)
                s.transitions.push_back({int32_t(p[i]), g.node<TransitionNode>(int32_t(p[i + 1])),
                                         g.node<ValueNode>(int32_t(p[i + 2])), p[i + 3] != 0});
            states.push_back(std::move(s));
        }
        for (size_t s = 0; s < states.size() && s < 64; ++s) {
            states[s].node->flagWord = &transitioning;
            states[s].node->flagBit = 1ull << s;
        }
    }
    bool isValid() const override { return active >= 0 && active < int32_t(states.size()); }
    const fmt::GraphNode* debugActive() const override { return isInitialized() && isValid() ? states[active].node->def : nullptr; }
    const SyncTrack& syncTrack() const override
    {
        return activeTransition ? activeTransition->syncTrack() : states[active].node->syncTrack();
    }

    PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range) override
    {
        markActive(ctx);
        const int8_t marker = ctx.tasks->marker();
        if (pendingConditionShutdown >= 0) {
            shutdownConditions(ctx, pendingConditionShutdown);
            pendingConditionShutdown = -1;
        }
        if (activeTransition && activeTransition->isComplete(ctx)) {
            activeTransition->shutdown(ctx);
            activeTransition = nullptr;
        }
        PoseResult result;
        PoseNode* current = activeTransition ? static_cast<PoseNode*>(activeTransition) : states[active].node;
        result = current->update(ctx, range);
        copyTiming(current);
        if (ctx.activeBranch)
            evaluateTransitions(ctx, range, result, marker);
        return result;
    }

protected:
    void copyTiming(PoseNode* n)
    {
        duration_ = n->duration();
        previous_ = n->previousTime();
        current_ = n->currentTime();
    }
    void initializeConditions(GraphContext& ctx, int32_t s)
    {
        for (TransitionInfo& t : states[s].transitions)
            if (t.condition)
                t.condition->initialize(ctx);
    }
    void shutdownConditions(GraphContext& ctx, int32_t s)
    {
        for (TransitionInfo& t : states[s].transitions)
            if (t.condition)
                t.condition->shutdown(ctx);
    }
    int32_t selectDefaultState(GraphContext& ctx)
    {
        int32_t selected = def->n[0];
        for (StateInfo& s : states)
            if (s.entryCondition)
                s.entryCondition->initialize(ctx);
        for (size_t i = 0; i < states.size(); ++i)
            if (states[i].entryCondition && states[i].entryCondition->getBool(ctx)) {
                selected = int32_t(i);
                break;
            }
        for (StateInfo& s : states)
            if (s.entryCondition)
                s.entryCondition->shutdown(ctx);
        return selected;
    }
    void evaluateTransitions(GraphContext& ctx, const SyncTrackTimeRange* range, PoseResult& result, int8_t marker)
    {
        StateInfo& cur = states[active];
        const TransitionInfo* chosen = nullptr;
        for (const TransitionInfo& t : cur.transitions) {
            // Forced transitions (into a state still blending) need cached poses: not ported.
            if (transitioning & (1ull << t.targetState))
                continue;
            if (t.condition && t.condition->getBool(ctx)) {
                chosen = &t;
                break;
            }
        }
        if (!chosen)
            return;
        chosen->node->initialize(ctx, SyncTrackTime{});
        TransitionNode::StartOptions o;
        o.sourceResult = result;
        o.updateRange = range;
        o.sourceIsTransition = activeTransition != nullptr;
        o.source = activeTransition ? static_cast<PoseNode*>(activeTransition) : cur.node;
        o.sourceTasksMarker = marker;
        result = chosen->node->start(ctx, o);
        activeTransition = chosen->node;
        pendingConditionShutdown = active;  // their values were just read: shut down next update
        active = chosen->targetState;
        initializeConditions(ctx, active);
        copyTiming(states[active].node);
    }
    void initializePose(GraphContext& ctx, const SyncTrackTime& t) override
    {
        PoseNode::initializePose(ctx, t);
        transitioning = 0;
        active = selectDefaultState(ctx);
        states[active].node->initialize(ctx, t);
        copyTiming(states[active].node);
        initializeConditions(ctx, active);
    }
    void shutdownInternal(GraphContext& ctx) override
    {
        if (activeTransition)
            activeTransition->shutdown(ctx);
        if (pendingConditionShutdown >= 0) {
            shutdownConditions(ctx, pendingConditionShutdown);
            pendingConditionShutdown = -1;
        }
        shutdownConditions(ctx, active);
        states[active].node->shutdown(ctx);
        activeTransition = nullptr;
        transitioning = 0;
        PoseNode::shutdownInternal(ctx);
    }

    std::vector<StateInfo> states;
    uint64_t transitioning = 0;
    TransitionNode* activeTransition = nullptr;
    int32_t active = 0, pendingConditionShutdown = -1;
};

// ---- factory

template <typename T>
constexpr size_t slot()
{
    return (sizeof(T) + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
}

size_t nodeSize(fmt::NodeType t)
{
    using N = fmt::NodeType;
    switch (t) {
    case N::ParamBool: case N::ParamFloat: case N::ParamId: return slot<ParamNode>();
    case N::ConstBool: case N::ConstFloat: case N::ConstId: return slot<ConstNode>();
    case N::And: case N::Or: case N::Not: return slot<BoolLogicNode>();
    case N::FloatCompare: case N::FloatRange: case N::FloatMath: case N::FloatRemap: case N::FloatClamp:
        return slot<FloatLogicNode>();
    case N::IdCompare: return slot<IdCompareNode>();
    case N::StateCompleted: case N::TimeCondition: return slot<StateConditionNode>();
    case N::Clip: return slot<ClipNode>();
    case N::Blend1D: return slot<Blend1DNode>();
    case N::SpeedScale: return slot<SpeedScaleNode>();
    case N::Selector: return slot<SelectorNode>();
    case N::ReferencePose: return slot<ReferencePoseNode>();
    case N::State: return slot<StateNode>();
    case N::StateMachine: return slot<StateMachineNode>();
    case N::Transition: return slot<TransitionNode>();
    case N::EventCondition: case N::EventPercent: return slot<EventNode>();
    case N::Blend2D: return slot<Blend2DNode>();
    case N::Layers: return slot<LayersNode>();
    case N::BoneMask: case N::BoneMaskBlend: return slot<BoneMaskNode>();
    default: return 0;
    }
}

Node* construct(fmt::NodeType t, void* m)
{
    using N = fmt::NodeType;
    switch (t) {
    case N::ParamBool: case N::ParamFloat: case N::ParamId: return new (m) ParamNode();
    case N::ConstBool: case N::ConstFloat: case N::ConstId: return new (m) ConstNode();
    case N::And: case N::Or: case N::Not: return new (m) BoolLogicNode();
    case N::FloatCompare: case N::FloatRange: case N::FloatMath: case N::FloatRemap: case N::FloatClamp:
        return new (m) FloatLogicNode();
    case N::IdCompare: return new (m) IdCompareNode();
    case N::StateCompleted: case N::TimeCondition: return new (m) StateConditionNode();
    case N::Clip: return new (m) ClipNode();
    case N::Blend1D: return new (m) Blend1DNode();
    case N::SpeedScale: return new (m) SpeedScaleNode();
    case N::Selector: return new (m) SelectorNode();
    case N::ReferencePose: return new (m) ReferencePoseNode();
    case N::State: return new (m) StateNode();
    case N::StateMachine: return new (m) StateMachineNode();
    case N::Transition: return new (m) TransitionNode();
    case N::EventCondition: case N::EventPercent: return new (m) EventNode();
    case N::Blend2D: return new (m) Blend2DNode();
    case N::Layers: return new (m) LayersNode();
    case N::BoneMask: case N::BoneMaskBlend: return new (m) BoneMaskNode();
    default: return nullptr;
    }
}

}  // namespace

// ---- instance

GraphInstance::~GraphInstance()
{
    if (root_ && root_->isInitialized())
        root_->shutdown(ctx_);
    for (Node* n : nodes_)
        if (n)
            n->~Node();
}

std::shared_ptr<const GraphDefinition> GraphDefinition::fromPak(const AnimData& data, uint32_t graph)
{
    const fmt::Graph& g = data.graph(graph);
    auto d = std::make_shared<GraphDefinition>();
    d->name = data.name(g.name);
    d->skeleton = g.skeleton;
    d->nodes = data.at<fmt::GraphNode>(g.nodes);
    d->nodeCount = g.nodeCount;
    d->pool = data.at<uint32_t>(g.pool);
    d->root = g.root;
    const uint32_t* names = data.at<uint32_t>(g.parameterNames);
    for (uint32_t i = 0; i < g.parameterCount; ++i)
        d->parameters.push_back(data.name(names[i]));
    return d;
}

std::shared_ptr<const GraphDefinition> GraphDefinition::fromCompiled(std::shared_ptr<const CompiledGraph> graph)
{
    auto d = std::make_shared<GraphDefinition>();
    d->name = graph->name;
    d->skeleton = graph->skeleton;
    d->nodes = graph->nodes.data();
    d->nodeCount = uint32_t(graph->nodes.size());
    d->pool = graph->pool.data();
    d->root = graph->root;
    d->parameters = graph->parameterNames;
    d->entityParameters = graph->entityParameters;
    d->ik = graph->ik;
    d->compiled = std::move(graph);
    return d;
}

bool GraphInstance::init(std::shared_ptr<const GraphDefinition> definition, const std::vector<Clip>& clips, const Skeleton* skeleton)
{
    def_ = std::move(definition);
    clips_ = &clips;
    skeleton_ = skeleton;
    pool_ = def_->pool;
    tasks_.init(skeleton);
    ctx_.tasks = &tasks_;
    ctx_.events = &events_;

    const fmt::GraphNode* defs = def_->nodes;
    size_t total = 0;
    for (uint32_t i = 0; i < def_->nodeCount; ++i) {
        const size_t s = nodeSize(defs[i].type);
        if (s == 0)
            return false;
        total += s;
    }
    memory_.reset(new uint8_t[total + alignof(std::max_align_t)]);
    uint8_t* p = reinterpret_cast<uint8_t*>((uintptr_t(memory_.get()) + alignof(std::max_align_t) - 1) &
                                            ~uintptr_t(alignof(std::max_align_t) - 1));
    nodes_.resize(def_->nodeCount);
    for (uint32_t i = 0; i < def_->nodeCount; ++i) {
        nodes_[i] = construct(defs[i].type, p);
        nodes_[i]->def = &defs[i];
        p += nodeSize(defs[i].type);
    }
    for (Node* n : nodes_)
        n->link(*this);
    root_ = node<PoseNode>(def_->root);
    for (const std::string& name : def_->parameters)
        parameterHashes_.push_back(fmt::hashId(name.c_str()));

    // Parameters are persistent; everything else initializes lazily from the root.
    ++ctx_.updateId;
    for (uint32_t i = 0; i < parameterCount(); ++i)
        nodes_[i]->initialize(ctx_);
    return root_ != nullptr;
}

const Clip* GraphInstance::clip(int32_t index) const
{
    return index >= 0 && size_t(index) < clips_->size() ? &(*clips_)[index] : nullptr;
}

int32_t GraphInstance::parameterIndex(uint32_t nameHash) const
{
    for (size_t i = 0; i < parameterHashes_.size(); ++i)
        if (parameterHashes_[i] == nameHash)
            return int32_t(i);
    return -1;
}

void GraphInstance::setBool(int32_t index, bool v)
{
    if (index >= 0 && uint32_t(index) < parameterCount())
        static_cast<ParamNode*>(nodes_[index])->b = v;
}

void GraphInstance::setFloat(int32_t index, float v)
{
    if (index >= 0 && uint32_t(index) < parameterCount() && std::isfinite(v))
        static_cast<ParamNode*>(nodes_[index])->f = v;
}

void GraphInstance::setId(int32_t index, uint32_t v)
{
    if (index >= 0 && uint32_t(index) < parameterCount())
        static_cast<ParamNode*>(nodes_[index])->id = v;
}

float GraphInstance::getFloat(int32_t index)
{
    return index >= 0 && uint32_t(index) < parameterCount() ? static_cast<ParamNode*>(nodes_[index])->f : 0.0f;
}

bool GraphInstance::getBool(int32_t index)
{
    return index >= 0 && uint32_t(index) < parameterCount() && static_cast<ParamNode*>(nodes_[index])->b;
}

uint32_t GraphInstance::getId(int32_t index)
{
    return index >= 0 && uint32_t(index) < parameterCount() ? static_cast<ParamNode*>(nodes_[index])->id : 0;
}

int32_t GraphInstance::activeNode(int32_t index) const
{
    if (index < 0 || size_t(index) >= nodes_.size())
        return -1;
    const fmt::GraphNode* d = nodes_[index]->debugActive();
    return d ? int32_t(d - def_->nodes) : -1;
}

bool GraphInstance::inState(uint32_t nameHash) const
{
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (def_->nodes[i].type != fmt::NodeType::StateMachine || !nodes_[i]->debugUpdated(ctx_.updateId))
            continue;
        const fmt::GraphNode* a = nodes_[i]->debugActive();
        if (!a)
            continue;
        if (a->id == nameHash)
            return true;
        if (!a->id && def_->compiled) {  // (an older compile: the name from its node id)
            const std::string& id = def_->compiled->nodeIds[size_t(a - def_->nodes)];
            const size_t colon = id.rfind(':');
            if (fmt::hashId(id.c_str() + (colon == std::string::npos ? 0 : colon + 1)) == nameHash)
                return true;
        }
    }
    return false;
}

std::string GraphInstance::stateName(uint32_t n) const
{
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (def_->nodes[i].type != fmt::NodeType::StateMachine || !nodes_[i]->debugUpdated(ctx_.updateId))
            continue;
        const fmt::GraphNode* a = nodes_[i]->debugActive();
        if (!a || n-- > 0)
            continue;
        if (!def_->compiled)
            return {};
        const std::string& id = def_->compiled->nodeIds[size_t(a - def_->nodes)];
        const size_t colon = id.rfind(':');
        return colon == std::string::npos ? id : id.substr(colon + 1);
    }
    return {};
}

void GraphInstance::debug(std::vector<NodeDebug>& out) const
{
    out.assign(nodes_.size(), {});
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Node* n = nodes_[i];
        NodeDebug& d = out[i];
        d.flags = uint8_t((n->isInitialized() ? 1 : 0) | (n->debugUpdated(ctx_.updateId) ? 2 : 0));
        if (const fmt::GraphNode* a = n->debugActive())
            d.active = int32_t(a - def_->nodes);
        d.progress = n->debugProgress();
        const fmt::NodeType t = def_->nodes[i].type;
        if ((t >= fmt::NodeType::Clip && t <= fmt::NodeType::Transition) || t == fmt::NodeType::Blend2D || t == fmt::NodeType::Layers) {
            const PoseNode* p = static_cast<const PoseNode*>(n);
            d.time = p->currentTime();
            d.duration = p->duration();
        }
    }
}

Xform GraphInstance::evaluate(float dt)
{
    tasks_.reset();
    events_.clear();
    ctx_.deltaTime = dt;
    ++ctx_.updateId;
    ctx_.activeBranch = true;
    if (!root_->isInitialized()) {
        root_->initialize(ctx_, SyncTrackTime{});
        ++ctx_.updateId;  // values read during initialization are re-evaluated
    }
    const PoseResult r = root_->update(ctx_, nullptr);
    rootTask_ = r.task;
    return r.rootMotion;
}

}  // namespace fx::anim
