#include "audio/audio_bake.h"

#include "core/json.h"
#include "core/log.h"

#include <SDL3/SDL_timer.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#if FALCON_STEAM_AUDIO
#include <phonon.h>
#endif

namespace fx {

namespace fs = std::filesystem;

namespace {

constexpr char kPositionsMagic[4] = {'F', 'A', 'P', '1'};

bool readFile(const fs::path& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

// Written beside, then moved over the old one (a reader never sees half a file).
bool writeFile(const fs::path& path, const void* data, size_t size)
{
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(static_cast<const char*>(data), std::streamsize(size));
        if (!f)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

std::string hex(uint64_t v)
{
    char s[17];
    std::snprintf(s, sizeof(s), "%016llx", (unsigned long long)v);
    return s;
}
uint64_t unhex(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 16); }

void writeVec(JsonWriter& w, const char* key, float3 v)
{
    w.beginArray(key).value(nullptr, double(v.x)).value(nullptr, double(v.y)).value(nullptr, double(v.z)).endArray();
}
float3 readVec(const Json& j, const char* key, float3 fallback)
{
    const Json* a = j.get(key);
    if (!a || a->array.size() != 3)
        return fallback;
    return {float(a->array[0].number), float(a->array[1].number), float(a->array[2].number)};
}

uint64_t cellKey(int32_t x, int32_t y, int32_t z)
{
    return (uint64_t(uint32_t(x) & 0x1fffff) << 42) | (uint64_t(uint32_t(y) & 0x1fffff) << 21) | uint64_t(uint32_t(z) & 0x1fffff);
}

}  // namespace

std::string audioBakeDirFor(const std::string& scenePath)
{
    std::string p = scenePath;
    const size_t dot = p.find_last_of('.');
    const size_t slash = p.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        p.resize(dot);
    return p + ".audio";
}

bool readAudioBake(const std::string& dir, AudioBakeSettings& settings, AudioBakeInfo& info)
{
    info = {};
    std::string text;
    if (dir.empty() || !readFile(fs::path(dir) / "bake.json", text))
        return false;
    try {
        const Json root = Json::parse(text);
        if (const Json* s = root.get("settings"); s && s->isObject()) {
            settings.spacing = std::clamp(float(s->getNumber("spacing", settings.spacing)), 0.5f, 50.0f);
            settings.height = std::clamp(float(s->getNumber("height", settings.height)), 0.1f, 10.0f);
            settings.quality = std::clamp(uint32_t(s->getNumber("quality", settings.quality)), 1u, 3u);
            settings.pathing = s->getBool("pathing", settings.pathing);
            settings.autoRegion = s->getBool("autoRegion", settings.autoRegion);
            settings.center = readVec(*s, "center", settings.center);
            settings.size = readVec(*s, "size", settings.size);
        }
        if (const Json* b = root.get("baked"); b && b->isObject()) {
            info.valid = true;
            info.probes = uint32_t(b->getNumber("probes"));
            info.reverb = b->getBool("reverb");
            info.pathing = b->getBool("pathing");
            info.bytes = uint64_t(b->getNumber("bytes"));
            info.seconds = b->getNumber("seconds");
            info.bakedAt = int64_t(b->getNumber("bakedAt"));
            info.objectsHash = unhex(b->getString("objects"));
            info.terrainHash = unhex(b->getString("terrain"));
            info.objects = uint32_t(b->getNumber("objectCount"));
            info.spacing = float(b->getNumber("spacing"));
            info.height = float(b->getNumber("height"));
            info.visRange = float(b->getNumber("visRange"));
            info.quality = uint32_t(b->getNumber("quality"));
            info.order = uint32_t(b->getNumber("order", 1));
            info.lo = readVec(*b, "lo", {});
            info.hi = readVec(*b, "hi", {});
        }
    } catch (const std::exception& e) {
        FX_WARN("audio: %s/bake.json: %s", dir.c_str(), e.what());
        return false;
    }
    return true;
}

bool writeAudioBake(const std::string& dir, const AudioBakeSettings& s, const AudioBakeInfo& info)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    JsonWriter w;
    w.beginObject().value("format", uint32_t(1));
    w.beginObject("settings").value("spacing", double(s.spacing)).value("height", double(s.height)).value("quality", s.quality);
    w.value("pathing", s.pathing).value("autoRegion", s.autoRegion);
    writeVec(w, "center", s.center);
    writeVec(w, "size", s.size);
    w.endObject();
    if (info.valid) {
        w.beginObject("baked").value("probes", info.probes).value("reverb", info.reverb).value("pathing", info.pathing);
        w.value("bytes", info.bytes).value("seconds", info.seconds).value("bakedAt", info.bakedAt);
        w.value("objects", hex(info.objectsHash)).value("terrain", hex(info.terrainHash)).value("objectCount", info.objects);
        w.value("spacing", double(info.spacing)).value("height", double(info.height)).value("visRange", double(info.visRange));
        w.value("quality", info.quality).value("order", info.order);
        writeVec(w, "lo", info.lo);
        writeVec(w, "hi", info.hi);
        w.endObject();
    }
    w.endObject();
    return writeFile(fs::path(dir) / "bake.json", w.str().data(), w.str().size());
}

bool clearAudioBake(const std::string& dir, const AudioBakeSettings& settings)
{
    std::error_code ec;
    fs::remove(fs::path(dir) / "probes.bin", ec);
    fs::remove(fs::path(dir) / "probes.pos", ec);
    return writeAudioBake(dir, settings, {});
}

AudioBakedData::~AudioBakedData()
{
#if FALCON_STEAM_AUDIO
    if (batch) {
        IPLProbeBatch b = static_cast<IPLProbeBatch>(batch);
        iplProbeBatchRelease(&b);
    }
#endif
}

void AudioBakedData::index()
{
    float largest = 0.0f;
    for (const AudioProbe& p : probes)
        largest = std::max(largest, p.radius);
    cell_ = std::max(largest, 0.5f);
    grid_.clear();
    grid_.reserve(probes.size());
    for (uint32_t i = 0; i < probes.size(); ++i) {
        const float3 c = probes[i].position * (1.0f / cell_);
        grid_.emplace_back(cellKey(int32_t(std::floor(c.x)), int32_t(std::floor(c.y)), int32_t(std::floor(c.z))), i);
    }
    std::sort(grid_.begin(), grid_.end());
}

bool AudioBakedData::covers(float3 p) const
{
    const float3 c = p * (1.0f / cell_);
    const int32_t x = int32_t(std::floor(c.x)), y = int32_t(std::floor(c.y)), z = int32_t(std::floor(c.z));
    for (int32_t dz = -1; dz <= 1; ++dz)
        for (int32_t dy = -1; dy <= 1; ++dy)
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const uint64_t key = cellKey(x + dx, y + dy, z + dz);
                for (auto it = std::lower_bound(grid_.begin(), grid_.end(), std::pair<uint64_t, uint32_t>{key, 0u});
                     it != grid_.end() && it->first == key; ++it) {
                    const AudioProbe& probe = probes[it->second];
                    if (length(probe.position - p) <= probe.radius)
                        return true;
                }
            }
    return false;
}

#if FALCON_STEAM_AUDIO

bool loadAudioBake(void* context, const std::string& dir, std::unique_ptr<AudioBakedData>& out, std::string& error)
{
    out.reset();
    AudioBakeSettings settings;
    AudioBakeInfo info;
    if (!context || !readAudioBake(dir, settings, info) || !info.valid)
        return false;
    std::string data, positions;
    if (!readFile(fs::path(dir) / "probes.bin", data) || !readFile(fs::path(dir) / "probes.pos", positions)) {
        error = "probes.bin or probes.pos is missing";
        return false;
    }
    if (positions.size() < 8 || std::memcmp(positions.data(), kPositionsMagic, 4) != 0) {
        error = "probes.pos is not a probe list";
        return false;
    }
    uint32_t count = 0;
    std::memcpy(&count, positions.data() + 4, 4);
    if (positions.size() != 8 + size_t(count) * sizeof(AudioProbe)) {
        error = "probes.pos is cut short";
        return false;
    }
    auto baked = std::make_unique<AudioBakedData>();
    baked->info = info;
    baked->probes.resize(count);
    std::memcpy(baked->probes.data(), positions.data() + 8, size_t(count) * sizeof(AudioProbe));
    baked->index();
    IPLSerializedObjectSettings ss{reinterpret_cast<IPLbyte*>(data.data()), IPLsize(data.size())};
    IPLSerializedObject serialized = nullptr;
    IPLProbeBatch batch = nullptr;
    if (iplSerializedObjectCreate(static_cast<IPLContext>(context), &ss, &serialized) != IPL_STATUS_SUCCESS) {
        error = "out of memory";
        return false;
    }
    const IPLerror loaded = iplProbeBatchLoad(static_cast<IPLContext>(context), serialized, &batch);
    iplSerializedObjectRelease(&serialized);
    if (loaded != IPL_STATUS_SUCCESS || !batch) {
        error = "Steam Audio could not read probes.bin (baked by another version?)";
        return false;
    }
    iplProbeBatchCommit(batch);
    baked->batch = batch;
    out = std::move(baked);
    return true;
}

AudioBaker::~AudioBaker()
{
    cancel();
    if (thread_.joinable())
        thread_.join();
}

bool AudioBaker::start(Job job)
{
    if (thread_.joinable())
        return false;
    done_ = false;
    cancel_ = false;
    context_ = job.context;
    {
        std::lock_guard lock(lock_);
        progress_ = {"scene", 0.0f, 0};
        ok_ = false;
        error_.clear();
        info_ = {};
    }
    thread_ = std::thread([this, job = std::move(job)]() mutable { run(std::move(job)); });
    return true;
}

void AudioBaker::cancel()
{
    if (!running())
        return;
    cancel_ = true;
    if (void* c = context_.load()) {  // (whichever bake runs stops at its next check)
        iplReflectionsBakerCancelBake(static_cast<IPLContext>(c));
        iplPathBakerCancelBake(static_cast<IPLContext>(c));
    }
}

void AudioBaker::stop()
{
    cancel();
    if (thread_.joinable())
        thread_.join();
}

AudioBaker::Progress AudioBaker::progress() const
{
    std::lock_guard lock(lock_);
    return progress_;
}

bool AudioBaker::collect(bool& ok, std::string& error, AudioBakeInfo& info)
{
    if (!thread_.joinable() || !done_.load())
        return false;
    thread_.join();
    std::lock_guard lock(lock_);
    ok = ok_;
    error = error_;
    info = info_;
    return true;
}

void AudioBaker::stage(const char* name, float fraction)
{
    std::lock_guard lock(lock_);
    progress_.stage = name;
    progress_.fraction = fraction;
}

void AudioBaker::run(Job job)
{
    const uint64_t t0 = SDL_GetTicksNS();
    IPLContext context = static_cast<IPLContext>(job.context);
    // Embree for the bake's rays, its threads (one per core) only while it runs.
    IPLEmbreeDevice embree = nullptr;
    if (IPLEmbreeDeviceSettings es{}; job.embree && iplEmbreeDeviceCreate(context, &es, &embree) != IPL_STATUS_SUCCESS)
        embree = nullptr;
    const AudioBakeSettings& s = job.settings;
    const AudioBakeGeometry& g = job.geometry;
    IPLScene scene = nullptr;
    IPLStaticMesh mesh = nullptr;
    IPLProbeArray array = nullptr;
    IPLProbeBatch batch = nullptr;
    std::string error;
    AudioBakeInfo info;
    const auto fail = [&](std::string why) { error = cancel_.load() ? "Cancelled" : std::move(why); };
    const auto progress = [](IPLfloat32 fraction, void* user) {
        auto* self = static_cast<AudioBaker*>(user);
        std::lock_guard lock(self->lock_);
        self->progress_.fraction = float(fraction);
    };
    do {
        // The scene: one static mesh of everything.
        IPLSceneSettings ss{};
        ss.type = embree ? IPL_SCENETYPE_EMBREE : IPL_SCENETYPE_DEFAULT;
        ss.embreeDevice = embree;
        if (g.triangles.empty()) {
            fail("Nothing to bake: the scene has no static geometry with a collider, and no terrain, in the region");
            break;
        }
        if (iplSceneCreate(context, &ss, &scene) != IPL_STATUS_SUCCESS) {
            fail("Steam Audio's scene did not start");
            break;
        }
        std::vector<IPLVector3> vertices(g.vertices.size());
        for (size_t i = 0; i < g.vertices.size(); ++i)
            vertices[i] = {g.vertices[i].x, g.vertices[i].y, g.vertices[i].z};
        std::vector<IPLTriangle> triangles(g.triangles.size() / 3);
        for (size_t i = 0; i < triangles.size(); ++i)
            triangles[i] = {{int(g.triangles[i * 3]), int(g.triangles[i * 3 + 1]), int(g.triangles[i * 3 + 2])}};
        std::vector<IPLMaterial> materials(g.materials.size());
        for (size_t i = 0; i < materials.size(); ++i) {
            for (int b = 0; b < 3; ++b) {
                materials[i].absorption[b] = g.materials[i].absorption[b];
                materials[i].transmission[b] = g.materials[i].transmission[b];
            }
            materials[i].scattering = g.materials[i].scattering;
        }
        std::vector<IPLint32> indices(g.materialIndex.begin(), g.materialIndex.end());
        IPLStaticMeshSettings ms{};
        ms.numVertices = IPLint32(vertices.size());
        ms.numTriangles = IPLint32(triangles.size());
        ms.numMaterials = IPLint32(materials.size());
        ms.vertices = vertices.data();
        ms.triangles = triangles.data();
        ms.materialIndices = indices.data();
        ms.materials = materials.data();
        if (iplStaticMeshCreate(scene, &ms, &mesh) != IPL_STATUS_SUCCESS) {
            fail("Steam Audio did not take the scene's geometry");
            break;
        }
        iplStaticMeshAdd(mesh, scene);
        iplSceneCommit(scene);
        if (cancel_) {
            fail("");
            break;
        }

        // Probes: a uniform grid over the floor under the region (whatever is solid under it).
        stage("probes", 0.0f);
        if (iplProbeArrayCreate(context, &array) != IPL_STATUS_SUCCESS) {
            fail("out of memory");
            break;
        }
        IPLProbeGenerationParams pg{};
        pg.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
        pg.spacing = s.spacing;
        pg.height = s.height;
        // (Steam Audio 4.8.1 scales a unit cube centred on the origin, not the documented 0 .. 1 one,
        // and moves it by the last column.)
        const float3 extent = g.hi - g.lo, centre = (g.lo + g.hi) * 0.5f;
        pg.transform.elements[0][0] = extent.x;
        pg.transform.elements[1][1] = extent.y;
        pg.transform.elements[2][2] = extent.z;
        pg.transform.elements[0][3] = centre.x;
        pg.transform.elements[1][3] = centre.y;
        pg.transform.elements[2][3] = centre.z;
        pg.transform.elements[3][3] = 1.0f;
        iplProbeArrayGenerateProbes(array, scene, &pg);
        const IPLint32 count = iplProbeArrayGetNumProbes(array);
        {
            std::lock_guard lock(lock_);
            progress_.probes = uint32_t(std::max(count, 0));
        }
        if (count <= 0) {
            fail("No probes: nothing solid under the region to stand them on");
            break;
        }
        if (uint32_t(count) > kAudioBakeMaxProbes) {
            fail(std::to_string(count) + " probes is more than " + std::to_string(kAudioBakeMaxProbes) +
                 ": space them farther apart, or bake a smaller region");
            break;
        }
        std::vector<AudioProbe> probes(static_cast<size_t>(count));
        for (IPLint32 i = 0; i < count; ++i) {
            const IPLSphere p = iplProbeArrayGetProbe(array, i);
            probes[size_t(i)] = {{p.center.x, p.center.y, p.center.z}, p.radius};
        }
        if (iplProbeBatchCreate(context, &batch) != IPL_STATUS_SUCCESS) {
            fail("out of memory");
            break;
        }
        iplProbeBatchAddProbeArray(batch, array);
        iplProbeBatchCommit(batch);

        // The listener's reverb at each probe: impulse responses (convolution, hybrid) and reverb
        // times (parametric, hybrid), so any reverb type plays it.
        stage("reverb", 0.0f);
        const AudioBakeQuality q = audioBakeQuality(s.quality);
        const int threads = int(std::clamp(std::thread::hardware_concurrency(), 2u, 64u) - 1);
        IPLReflectionsBakeParams rp{};
        rp.scene = scene;
        rp.probeBatch = batch;
        rp.sceneType = ss.type;
        rp.identifier = {IPL_BAKEDDATATYPE_REFLECTIONS, IPL_BAKEDDATAVARIATION_REVERB, {}};
        rp.bakeFlags = IPLReflectionsBakeFlags(IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION | IPL_REFLECTIONSBAKEFLAGS_BAKEPARAMETRIC);
        rp.numRays = int(q.rays);
        rp.numDiffuseSamples = 32;
        rp.numBounces = int(q.bounces);
        rp.simulatedDuration = q.seconds;
        rp.savedDuration = std::min(q.seconds, job.savedSeconds);
        rp.order = int(job.order);
        rp.numThreads = threads;
        rp.irradianceMinDistance = 1.0f;
        rp.bakeBatchSize = 1;
        iplReflectionsBakerBake(context, &rp, progress, this);
        if (cancel_) {
            fail("");
            break;
        }
        info.reverb = true;

        // Paths between the probes (each pair that sees the other, within the range), so sound
        // finds its way around what blocks it.
        const float visRange = std::max(8.0f * s.spacing, 24.0f);
        if (s.pathing) {
            stage("paths", 0.0f);
            IPLPathBakeParams pp{};
            pp.scene = scene;
            pp.probeBatch = batch;
            pp.identifier = {IPL_BAKEDDATATYPE_PATHING, IPL_BAKEDDATAVARIATION_DYNAMIC, {}};
            pp.numSamples = 1;
            pp.radius = std::min(0.5f, 0.25f * s.spacing);
            pp.threshold = 0.1f;
            pp.visRange = visRange;
            pp.pathRange = std::max(visRange * 4.0f, 100.0f);
            pp.numThreads = threads;
            iplPathBakerBake(context, &pp, progress, this);
            if (cancel_) {
                fail("");
                break;
            }
            info.pathing = true;
        }

        // Saved: the batch, the probes, what it was baked from.
        stage("saving", 0.0f);
        IPLSerializedObjectSettings so{};
        IPLSerializedObject serialized = nullptr;
        if (iplSerializedObjectCreate(context, &so, &serialized) != IPL_STATUS_SUCCESS) {
            fail("out of memory");
            break;
        }
        iplProbeBatchSave(batch, serialized);
        const size_t bytes = size_t(iplSerializedObjectGetSize(serialized));
        std::error_code ec;
        fs::create_directories(job.dir, ec);
        const bool wrote = writeFile(fs::path(job.dir) / "probes.bin", iplSerializedObjectGetData(serialized), bytes);
        iplSerializedObjectRelease(&serialized);
        std::string pos(kPositionsMagic, 4);
        const uint32_t n = uint32_t(probes.size());
        pos.append(reinterpret_cast<const char*>(&n), 4);
        pos.append(reinterpret_cast<const char*>(probes.data()), probes.size() * sizeof(AudioProbe));
        info.valid = true;
        info.probes = n;
        info.bytes = bytes;
        info.seconds = double(SDL_GetTicksNS() - t0) * 1e-9;
        info.bakedAt = int64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        info.objectsHash = g.objectsHash;
        info.terrainHash = g.terrainHash;
        info.objects = g.objects;
        info.spacing = s.spacing;
        info.height = s.height;
        info.visRange = visRange;
        info.quality = s.quality;
        info.order = job.order;
        info.lo = g.lo;
        info.hi = g.hi;
        if (!wrote || !writeFile(fs::path(job.dir) / "probes.pos", pos.data(), pos.size()) || !writeAudioBake(job.dir, s, info)) {
            fail("Could not write " + job.dir);
            info = {};
            break;
        }
        stage("saving", 1.0f);
    } while (false);
    if (batch)
        iplProbeBatchRelease(&batch);
    if (array)
        iplProbeArrayRelease(&array);
    if (mesh) {
        iplStaticMeshRemove(mesh, scene);
        iplStaticMeshRelease(&mesh);
    }
    if (scene)
        iplSceneRelease(&scene);
    if (embree)
        iplEmbreeDeviceRelease(&embree);
    {
        std::lock_guard lock(lock_);
        ok_ = error.empty();
        error_ = error;
        info_ = info;
    }
    done_ = true;
}

#else  // no Steam Audio in this build: nothing bakes

bool loadAudioBake(void*, const std::string&, std::unique_ptr<AudioBakedData>&, std::string&) { return false; }
AudioBaker::~AudioBaker() = default;
bool AudioBaker::start(Job) { return false; }
void AudioBaker::cancel() {}
void AudioBaker::stop() {}
AudioBaker::Progress AudioBaker::progress() const { return {}; }
bool AudioBaker::collect(bool&, std::string&, AudioBakeInfo&) { return false; }
void AudioBaker::stage(const char*, float) {}
void AudioBaker::run(Job) {}

#endif

}  // namespace fx
