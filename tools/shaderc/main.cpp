// falcon_shaderc: compiles one Slang source to SPIR-V + DXIL for every entry
// point, validates SDL_GPU's binding layout, turns the SPIR-V into Metal Shading Language
// (SPIRV-Cross, each resource at the index SDL's Metal backend binds it to), and emits an
// embeddable .cpp/.h (the engine's shaders) or, with --blob, one .fshader file (a project's: R2;
// engine/render/shader_file.h):
//   "FXSH", u32 version (1), u32 json size, the JSON (entries: name, stage, resource counts in
//   the order samplers, storage textures, storage buffers, rw storage textures, rw storage
//   buffers, uniform buffers; threads, where their SPIR-V, DXIL and MSL are, the MSL entry
//   point's name, the uniform block's fields), then the code.
// --include DIR (repeatable) adds a search path for imports. --msl-dir DIR also writes each
// entry's MSL as <DIR>/<file>.<entry>.metal (CI compiles them with Apple's compiler).
// --no-dxil 1: no DXIL (Linux and macOS have no DXC, nor D3D12 to run it); the register checks
// against DXIL are then skipped (Windows builds keep them).
//
// SDL_GPU binding model (HLSL space == SPIR-V set):
//   vertex   space0: [samplers][storage textures][storage buffers]   space1: uniforms
//   fragment space2: [samplers][storage textures][storage buffers]   space3: uniforms
//   compute  space0: [samplers][ro storage textures][ro storage buffers]
//            space1: [rw storage textures][rw storage buffers]       space2: uniforms
// Slots are counted from 0 in that order (unused slots below a used one count toward
// the next used resource's class); the shader must annotate both
// [[vk::binding(i, set)]] and register(xi, spaceN) with the same i.
//
// SDL_GPU's Metal order (SDL_gpu.h), which the MSL is given:
//   [[texture]] sampled textures, read-only storage textures, read-write storage textures
//   [[sampler]] one per sampled texture, at its index
//   [[buffer]]  uniform buffers, read-only storage buffers, read-write storage buffers
//   (vertex buffers from [[buffer(14)]]: the vertex stage's [[stage_in]])

#include <slang-com-ptr.h>
#include <slang.h>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Slang::ComPtr;

namespace {

struct Args {
    std::string root, source, outDir, dxc, depfile, blob, mslDir;
    bool dxil = true;
    std::vector<std::string> includes;
};

enum class Cls { Sampler, StorageTex, StorageBuf, RwStorageTex, RwStorageBuf, Uniform };

const char* clsName(Cls c)
{
    switch (c) {
    case Cls::Sampler: return "sampler";
    case Cls::StorageTex: return "storage texture";
    case Cls::StorageBuf: return "storage buffer";
    case Cls::RwStorageTex: return "rw storage texture";
    case Cls::RwStorageBuf: return "rw storage buffer";
    case Cls::Uniform: return "uniform buffer";
    }
    return "?";
}

struct Param {
    std::string name;
    Cls cls;
    unsigned set, index;
    slang::VariableLayoutReflection* spirvVar;
    slang::VariableLayoutReflection* dxilVar;
};

// A uniform block's field (--blob: parameters set by name).
struct UniformField {
    std::string name, type;  // type: float, int, uint, other
    unsigned offset = 0, size = 0, components = 0;
};

struct Entry {
    std::string name, symbol;
    SlangStage stage;
    std::vector<uint8_t> spirv, dxil;
    std::string msl, mslEntry;
    unsigned counts[6] = {};
    SlangUInt threads[3] = {0, 0, 0};
    std::vector<UniformField> uniforms;
    unsigned uniformSize = 0;
};

void reflectUniforms(slang::VariableLayoutReflection* var, Entry& entry)
{
    slang::TypeLayoutReflection* el = var->getTypeLayout()->getElementTypeLayout();
    if (!el)
        return;
    entry.uniformSize = (unsigned)el->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM);
    for (unsigned i = 0; i < el->getFieldCount(); ++i) {
        slang::VariableLayoutReflection* f = el->getFieldByIndex(i);
        slang::TypeLayoutReflection* t = f->getTypeLayout();
        slang::TypeReflection* type = t->getType();
        UniformField u;
        u.name = f->getName() ? f->getName() : "";
        u.offset = (unsigned)f->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM);
        u.size = (unsigned)t->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM);
        const auto scalar = type->getScalarType();
        u.type = scalar == slang::TypeReflection::ScalarType::Float32 ? "float"
                 : scalar == slang::TypeReflection::ScalarType::Int32 ? "int"
                 : scalar == slang::TypeReflection::ScalarType::UInt32 ? "uint"
                                                                      : "other";
        switch (type->getKind()) {
        case slang::TypeReflection::Kind::Scalar: u.components = 1; break;
        case slang::TypeReflection::Kind::Vector: u.components = (unsigned)type->getElementCount(); break;
        case slang::TypeReflection::Kind::Matrix: u.components = type->getRowCount() * type->getColumnCount(); break;
        default: u.type = "other"; break;
        }
        entry.uniforms.push_back(u);
    }
}

bool writeBlob(const fs::path& path, const std::string& source, const std::vector<Entry>& entries)
{
    std::ostringstream j;
    std::vector<uint8_t> code;
    j << "{\"source\":\"" << source << "\",\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        const char* stage = e.stage == SLANG_STAGE_VERTEX ? "vertex" : e.stage == SLANG_STAGE_FRAGMENT ? "fragment" : "compute";
        j << (i ? "," : "") << "{\"name\":\"" << e.name << "\",\"stage\":\"" << stage << "\",\"counts\":[";
        for (int c = 0; c < 6; ++c)
            j << (c ? "," : "") << e.counts[c];
        j << "],\"threads\":[" << e.threads[0] << "," << e.threads[1] << "," << e.threads[2] << "]";
        j << ",\"spirv\":[" << code.size() << "," << e.spirv.size() << "]";
        code.insert(code.end(), e.spirv.begin(), e.spirv.end());
        j << ",\"dxil\":[" << code.size() << "," << e.dxil.size() << "]";
        code.insert(code.end(), e.dxil.begin(), e.dxil.end());
        j << ",\"msl\":[" << code.size() << "," << e.msl.size() << "],\"mslEntry\":\"" << e.mslEntry << "\"";
        code.insert(code.end(), e.msl.begin(), e.msl.end());
        j << ",\"uniformSize\":" << e.uniformSize << ",\"uniforms\":[";
        for (size_t u = 0; u < e.uniforms.size(); ++u) {
            const UniformField& f = e.uniforms[u];
            j << (u ? "," : "") << "{\"name\":\"" << f.name << "\",\"type\":\"" << f.type << "\",\"offset\":" << f.offset
              << ",\"size\":" << f.size << ",\"components\":" << f.components << "}";
        }
        j << "]}";
        while (code.size() % 4)  // (the next entry's code aligned)
            code.push_back(0);
    }
    j << "]}";
    const std::string json = j.str();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const uint32_t version = 1, jsonSize = uint32_t(json.size());
    out.write("FXSH", 4);
    out.write(reinterpret_cast<const char*>(&version), 4);
    out.write(reinterpret_cast<const char*>(&jsonSize), 4);
    out.write(json.data(), std::streamsize(json.size()));
    out.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size()));
    return bool(out);
}

int g_errors = 0;

void error(const std::string& source, const std::string& msg)
{
    std::fprintf(stderr, "%s: error: %s\n", source.c_str(), msg.c_str());
    ++g_errors;
}

void printDiag(slang::IBlob* diag)
{
    if (diag && diag->getBufferSize())
        std::fprintf(stderr, "%s", (const char*)diag->getBufferPointer());
}

bool classify(slang::VariableLayoutReflection* var, Cls& out, std::string& why)
{
    auto* tl = var->getTypeLayout();
    switch (tl->getKind()) {
    case slang::TypeReflection::Kind::ConstantBuffer:
        out = Cls::Uniform;
        return true;
    case slang::TypeReflection::Kind::Resource: {
        const SlangResourceShape shape = tl->getResourceShape();
        const unsigned base = shape & SLANG_RESOURCE_BASE_SHAPE_MASK;
        const bool rw = tl->getResourceAccess() != SLANG_RESOURCE_ACCESS_READ;
        if (shape & SLANG_TEXTURE_COMBINED_FLAG) {
            out = Cls::Sampler;
            return true;
        }
        if (base == SLANG_STRUCTURED_BUFFER) {
            why = "SDL_GPU binds raw buffer views on D3D12; use (RW)ByteAddressBuffer with Load<T>/Store<T>";
            return false;
        }
        if (base == SLANG_BYTE_ADDRESS_BUFFER) {
            out = rw ? Cls::RwStorageBuf : Cls::StorageBuf;
            return true;
        }
        if (base == SLANG_TEXTURE_BUFFER) {
            why = "texel buffers are not supported by SDL_GPU";
            return false;
        }
        out = rw ? Cls::RwStorageTex : Cls::StorageTex;
        return true;
    }
    case slang::TypeReflection::Kind::SamplerState:
        why = "separate SamplerState is not supported by SDL_GPU; use combined Sampler2D etc.";
        return false;
    case slang::TypeReflection::Kind::Array:
        why = "resource arrays are not supported by SDL_GPU's binding model";
        return false;
    default:
        why = "unsupported global parameter (wrap loose uniforms in a ConstantBuffer<T>)";
        return false;
    }
}

// Which sets belong to which stage, and the class order within each set.
struct StageSets {
    unsigned resourceSet, rwSet, uniformSet;
};

StageSets stageSets(SlangStage stage)
{
    switch (stage) {
    case SLANG_STAGE_VERTEX: return {0, ~0u, 1};
    case SLANG_STAGE_FRAGMENT: return {2, ~0u, 3};
    default: return {0, 1, 2};
    }
}

slang::ParameterCategory dxilCategory(Cls c)
{
    switch (c) {
    case Cls::Uniform: return slang::ParameterCategory::ConstantBuffer;
    case Cls::RwStorageTex:
    case Cls::RwStorageBuf: return slang::ParameterCategory::UnorderedAccess;
    default: return slang::ParameterCategory::ShaderResource;
    }
}

// Buffer element layouts must be byte-identical across backends: std430/std140
// on Vulkan versus D3D packing can silently diverge (float3, arrays in cbuffers).
void compareLayouts(const std::string& src, const std::string& path, slang::TypeLayoutReflection* a,
                    slang::TypeLayoutReflection* b)
{
    if (!a || !b)
        return;
    if (a->getSize() != b->getSize()) {
        error(src, path + ": size differs between SPIR-V (" + std::to_string(a->getSize()) + ") and DXIL (" +
                       std::to_string(b->getSize()) + "); pad to 16-byte friendly layouts (avoid float3 members)");
        return;
    }
    if (a->getKind() == slang::TypeReflection::Kind::Array) {
        if (a->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM) != b->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM))
            error(src, path + ": array stride differs between SPIR-V and DXIL");
        compareLayouts(src, path + "[]", a->getElementTypeLayout(), b->getElementTypeLayout());
        return;
    }
    if (a->getKind() != slang::TypeReflection::Kind::Struct)
        return;
    for (unsigned i = 0; i < a->getFieldCount(); ++i) {
        auto* fa = a->getFieldByIndex(i);
        auto* fb = b->getFieldByIndex(i);
        const std::string fpath = path + "." + fa->getName();
        if (fa->getOffset() != fb->getOffset()) {
            error(src, fpath + ": offset differs between SPIR-V (" + std::to_string(fa->getOffset()) + ") and DXIL (" +
                           std::to_string(fb->getOffset()) + ")");
            continue;
        }
        compareLayouts(src, fpath, fa->getTypeLayout(), fb->getTypeLayout());
    }
}

slang::TypeLayoutReflection* bufferElement(slang::VariableLayoutReflection* var)
{
    auto* tl = var->getTypeLayout();
    return tl->getElementTypeLayout();
}

std::string sanitize(std::string s)
{
    for (char& c : s)
        if (!std::isalnum((unsigned char)c))
            c = '_';
    return s;
}

void emitBytes(std::ostringstream& o, const std::vector<uint8_t>& bytes)
{
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i % 24 == 0)
            o << "\n    ";
        o << (unsigned)bytes[i] << ',';
    }
}

std::string depEscape(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string out;
    for (char c : p) {
        if (c == ' ' || c == '#')
            out += '\\';
        out += c;
    }
    return out;
}

// The entry's SPIR-V as MSL for SDL's Metal backend: each resource SDL_GPU binds at slot `index`
// of `set` gets the Metal index of SDL's order (top of file). `counts` are the slot ranges SDL
// binds per class (gaps included), as the host gets them.
bool toMsl(const std::string& src, Entry& entry, const std::vector<Param>& params)
{
    using spirv_cross::CompilerMSL;
    const unsigned S = entry.counts[(int)Cls::Sampler], T = entry.counts[(int)Cls::StorageTex];
    const unsigned B = entry.counts[(int)Cls::StorageBuf], R = entry.counts[(int)Cls::RwStorageTex];
    const unsigned U = entry.counts[(int)Cls::Uniform];
    const spv::ExecutionModel model = entry.stage == SLANG_STAGE_VERTEX     ? spv::ExecutionModelVertex
                                      : entry.stage == SLANG_STAGE_FRAGMENT ? spv::ExecutionModelFragment
                                                                            : spv::ExecutionModelGLCompute;
    try {
        std::vector<uint32_t> words(entry.spirv.size() / 4);
        std::memcpy(words.data(), entry.spirv.data(), words.size() * 4);
        CompilerMSL msl(std::move(words));
        CompilerMSL::Options options = msl.get_msl_options();
        options.platform = CompilerMSL::Options::macOS;
        options.set_msl_version(3, 0);  // macOS 14+ on Apple silicon
        options.enable_decoration_binding = false;
        msl.set_msl_options(options);

        std::vector<std::pair<unsigned, unsigned>> bound;
        for (const Param& p : params) {
            spirv_cross::MSLResourceBinding b;
            b.stage = model;
            b.desc_set = p.set;
            b.binding = p.index;
            b.count = 1;
            switch (p.cls) {
            case Cls::Sampler: b.msl_texture = b.msl_sampler = p.index; break;
            case Cls::StorageTex: b.msl_texture = p.index; break;  // after the S sampled textures already
            case Cls::StorageBuf: b.msl_buffer = U + (p.index - S - T); break;
            case Cls::RwStorageTex: b.msl_texture = S + T + p.index; break;
            case Cls::RwStorageBuf: b.msl_buffer = U + B + (p.index - R); break;
            case Cls::Uniform: b.msl_buffer = p.index; break;
            }
            msl.add_msl_resource_binding(b);
            bound.emplace_back(p.set, p.index);
        }

        // Every resource the SPIR-V declares must be one of them, or SPIRV-Cross would pick an index.
        const spirv_cross::ShaderResources res = msl.get_shader_resources();
        for (const auto* list : {&res.uniform_buffers, &res.storage_buffers, &res.storage_images, &res.sampled_images,
                                 &res.separate_images, &res.separate_samplers}) {
            for (const spirv_cross::Resource& r : *list) {
                const unsigned set = msl.get_decoration(r.id, spv::DecorationDescriptorSet);
                const unsigned binding = msl.get_decoration(r.id, spv::DecorationBinding);
                if (std::find(bound.begin(), bound.end(), std::make_pair(set, binding)) == bound.end())
                    error(src, entry.name + ": '" + r.name + "' (binding " + std::to_string(binding) + ", set " +
                                   std::to_string(set) + ") has no Metal index");
            }
        }

        // Names a shader may use that are types or keywords in Metal and that SPIRV-Cross passes
        // through as they are (a local called half: vsm_pages, water_fft). They get a trailing _.
        static const char* const kMetalNames[] = {
            "half", "half2", "half3", "half4", "char", "uchar", "short", "ushort", "long", "ulong",
            "size_t", "ptrdiff_t", "thread", "threadgroup", "threadgroup_imageblock", "device",
            "constant", "ray_data", "object_data", "sampler", "texture", "access", "metal", "vec",
            "packed_float2", "packed_float3", "packed_float4", "bfloat", "atomic_uint", "atomic_int"};
        for (uint32_t id = 1; id < msl.get_current_id_bound(); ++id) {
            const std::string& name = msl.get_name(id);
            for (const char* reserved : kMetalNames)
                if (name == reserved) {
                    msl.set_name(id, name + "_");
                    break;
                }
        }

        entry.msl = msl.compile();
        entry.mslEntry = msl.get_cleansed_entry_point_name("main", model);
        return true;
    } catch (const std::exception& e) {
        error(src, entry.name + ": MSL: " + e.what());
        return false;
    }
}

bool writeIfChanged(const fs::path& path, const std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (old == text)
            return true;
    }
    in.close();
    std::ofstream out(path, std::ios::binary);
    out << text;
    return (bool)out;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--root") args.root = v;
        else if (k == "--source") args.source = v;
        else if (k == "--out-dir") args.outDir = v;
        else if (k == "--dxc") args.dxc = v;
        else if (k == "--depfile") args.depfile = v;
        else if (k == "--blob") args.blob = v;
        else if (k == "--msl-dir") args.mslDir = v;
        else if (k == "--no-dxil") args.dxil = std::strcmp(v, "0") == 0;
        else if (k == "--include") args.includes.push_back(v);
        else {
            std::fprintf(stderr, "falcon_shaderc: unknown option %s\n", k.c_str());
            return 2;
        }
    }
    if (args.root.empty() || args.source.empty() || (args.outDir.empty() && args.blob.empty())) {
        std::fprintf(stderr, "usage: falcon_shaderc --root DIR --source REL.slang (--out-dir DIR | --blob F.fshader) [--dxc DIR] "
                             "[--include DIR]... [--depfile F] [--msl-dir DIR] [--no-dxil 1]\n");
        return 2;
    }

    const fs::path srcPath = fs::path(args.root) / args.source;
    std::ifstream srcFile(srcPath, std::ios::binary);
    if (!srcFile) {
        error(srcPath.string(), "cannot open");
        return 1;
    }
    const std::string sourceText((std::istreambuf_iterator<char>(srcFile)), std::istreambuf_iterator<char>());
    const std::string stem = srcPath.stem().string();
    const std::string src = srcPath.generic_string();

    ComPtr<slang::IGlobalSession> global;
    if (SLANG_FAILED(slang::createGlobalSession(global.writeRef()))) {
        error(src, "failed to create Slang global session");
        return 1;
    }
    if (args.dxil && !args.dxc.empty())
        global->setDownstreamCompilerPath(SLANG_PASS_THROUGH_DXC, args.dxc.c_str());

    enum { kSpirv = 0, kDxil = 1 };
    slang::TargetDesc targets[2] = {};
    targets[kSpirv].format = SLANG_SPIRV;
    targets[kSpirv].profile = global->findProfile("spirv_1_5");
    targets[kDxil].format = SLANG_DXIL;
    targets[kDxil].profile = global->findProfile("sm_6_6");

    slang::CompilerOptionEntry options[1] = {};
    options[0].name = slang::CompilerOptionName::Optimization;
    options[0].value.intValue0 = SLANG_OPTIMIZATION_LEVEL_HIGH;

    const std::string rootStr = fs::path(args.root).generic_string();
    std::vector<const char*> searchPaths = {rootStr.c_str()};
    for (const std::string& dir : args.includes)
        searchPaths.push_back(dir.c_str());

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets = targets;
    sessionDesc.targetCount = args.dxil ? 2 : 1;
    sessionDesc.searchPaths = searchPaths.data();
    sessionDesc.searchPathCount = SlangInt(searchPaths.size());
    sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    sessionDesc.compilerOptionEntries = options;
    sessionDesc.compilerOptionEntryCount = 1;

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(sessionDesc, session.writeRef()))) {
        error(src, "failed to create Slang session");
        return 1;
    }

    ComPtr<slang::IBlob> diag;
    slang::IModule* module =
        session->loadModuleFromSourceString(stem.c_str(), srcPath.string().c_str(), sourceText.c_str(), diag.writeRef());
    printDiag(diag);
    if (!module)
        return 1;

    std::vector<Entry> entries;
    for (SlangInt32 e = 0; e < module->getDefinedEntryPointCount(); ++e) {
        ComPtr<slang::IEntryPoint> ep;
        module->getDefinedEntryPoint(e, ep.writeRef());
        slang::IComponentType* parts[] = {module, ep.get()};
        ComPtr<slang::IComponentType> composite, linked;
        diag = nullptr;
        if (SLANG_FAILED(session->createCompositeComponentType(parts, 2, composite.writeRef(), diag.writeRef()))) {
            printDiag(diag);
            return 1;
        }
        diag = nullptr;
        if (SLANG_FAILED(composite->link(linked.writeRef(), diag.writeRef()))) {
            printDiag(diag);
            return 1;
        }

        Entry entry;
        // (without DXIL, its layout is SPIR-V's: the cross-target checks below compare it with itself)
        slang::ProgramLayout* layouts[2] = {linked->getLayout(kSpirv), linked->getLayout(args.dxil ? kDxil : kSpirv)};
        slang::EntryPointReflection* epr = layouts[kSpirv]->getEntryPointByIndex(0);
        entry.name = epr->getName();
        entry.symbol = sanitize(stem + "_" + entry.name);
        entry.stage = epr->getStage();
        if (entry.stage != SLANG_STAGE_VERTEX && entry.stage != SLANG_STAGE_FRAGMENT &&
            entry.stage != SLANG_STAGE_COMPUTE) {
            error(src, entry.name + ": only vertex, fragment and compute stages exist in SDL_GPU");
            continue;
        }
        if (entry.stage == SLANG_STAGE_COMPUTE)
            epr->getComputeThreadGroupSize(3, entry.threads);
        if (epr->getParameterCount() > 0) {
            for (unsigned p = 0; p < epr->getParameterCount(); ++p) {
                auto* v = epr->getParameterByIndex(p);
                if (v->getCategory() == slang::ParameterCategory::Uniform ||
                    v->getCategory() == slang::ParameterCategory::ConstantBuffer)
                    error(src, entry.name + ": entry-point uniform '" + std::string(v->getName()) +
                                   "' is not allowed; use a global ConstantBuffer<T>");
            }
        }

        for (int t = 0; t < (args.dxil ? 2 : 1); ++t) {
            ComPtr<slang::IBlob> code;
            diag = nullptr;
            if (SLANG_FAILED(linked->getEntryPointCode(0, t, code.writeRef(), diag.writeRef()))) {
                printDiag(diag);
                error(src, entry.name + (t == kSpirv ? ": SPIR-V" : ": DXIL") + " compilation failed");
                continue;
            }
            printDiag(diag);
            auto* p = (const uint8_t*)code->getBufferPointer();
            (t == kSpirv ? entry.spirv : entry.dxil).assign(p, p + code->getBufferSize());
        }

        ComPtr<slang::IMetadata> metadata;
        linked->getEntryPointMetadata(0, kSpirv, metadata.writeRef(), nullptr);

        // Gather and validate global parameters relevant to this stage.
        const StageSets sets = stageSets(entry.stage);
        std::vector<Param> candidates;
        for (unsigned p = 0; p < layouts[kSpirv]->getParameterCount(); ++p) {
            auto* sv = layouts[kSpirv]->getParameterByIndex(p);
            auto* dv = layouts[kDxil]->getParameterByIndex(p);
            Param param{sv->getName(), Cls::Uniform, 0, 0, sv, dv};
            std::string why;
            if (!classify(sv, param.cls, why)) {
                error(src, std::string("'") + param.name + "': " + why);
                continue;
            }
            param.set = (unsigned)sv->getBindingSpace(slang::ParameterCategory::DescriptorTableSlot);
            param.index = (unsigned)sv->getOffset(slang::ParameterCategory::DescriptorTableSlot);

            const auto cat = dxilCategory(param.cls);
            const unsigned dSpace = args.dxil ? (unsigned)dv->getBindingSpace(cat) : param.set;
            const unsigned dIndex = args.dxil ? (unsigned)dv->getOffset(cat) : param.index;
            if (dSpace != param.set || dIndex != param.index)
                error(src, "'" + param.name + "': vk::binding(" + std::to_string(param.index) + ", " +
                               std::to_string(param.set) + ") does not match register index " + std::to_string(dIndex) +
                               " space " + std::to_string(dSpace));
            if (param.cls == Cls::Sampler && args.dxil) {
                const auto sIndex = dv->getOffset(slang::ParameterCategory::SamplerState);
                const auto sSpace = dv->getBindingSpace(slang::ParameterCategory::SamplerState);
                if (sIndex != param.index || sSpace != param.set)
                    error(src, "'" + param.name + "': sampler register must equal its texture register (s" +
                                   std::to_string(param.index) + ", space" + std::to_string(param.set) + ")");
            }

            // Only resources this entry point actually references form its layout, so one
            // file can hold compute and graphics entries whose sets overlap numerically.
            bool used = !metadata;
            if (metadata)
                metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT, param.set, param.index,
                                                  used);
            if (used)
                candidates.push_back(param);
        }

        // Usage is reported per location, so parameters of other stages that alias a used
        // location also look used. Keep the one whose class fits this stage's set.
        auto fits = [&](const Param& p) {
            if (p.set == sets.uniformSet)
                return p.cls == Cls::Uniform;
            if (p.set == sets.rwSet)
                return p.cls == Cls::RwStorageTex || p.cls == Cls::RwStorageBuf;
            if (p.set == sets.resourceSet)
                return p.cls == Cls::Sampler || p.cls == Cls::StorageTex || p.cls == Cls::StorageBuf;
            return false;
        };
        std::vector<Param> params;
        for (const Param& param : candidates) {
            bool aliasFits = false;
            for (const Param& other : candidates)
                if (&other != &param && other.set == param.set && other.index == param.index && fits(other))
                    aliasFits = true;
            if (aliasFits && !fits(param))
                continue;

            const bool ours = param.set == sets.resourceSet || param.set == sets.rwSet || param.set == sets.uniformSet;
            if (!ours) {
                error(src, entry.name + " uses '" + param.name + "' in set " + std::to_string(param.set) +
                               ", which belongs to another stage");
                continue;
            }
            auto* sv = param.spirvVar;
            auto* dv = param.dxilVar;
            if (param.set == sets.uniformSet && param.cls != Cls::Uniform)
                error(src, "'" + param.name + "': only uniform buffers may live in set " + std::to_string(param.set));
            if (param.set == sets.rwSet && param.cls != Cls::RwStorageTex && param.cls != Cls::RwStorageBuf)
                error(src, "'" + param.name + "': only rw storage resources may live in set " + std::to_string(param.set));
            if (param.set == sets.resourceSet &&
                (param.cls == Cls::Uniform || param.cls == Cls::RwStorageTex || param.cls == Cls::RwStorageBuf)) {
                if (entry.stage != SLANG_STAGE_COMPUTE && (param.cls == Cls::RwStorageTex || param.cls == Cls::RwStorageBuf))
                    error(src, "'" + param.name + "': graphics stages cannot write storage resources in SDL_GPU");
                else
                    error(src, "'" + param.name + "': wrong set for a " + std::string(clsName(param.cls)));
            }
            if (param.cls != Cls::Sampler)
                compareLayouts(src, param.name, bufferElement(sv), bufferElement(dv));
            if (param.cls == Cls::Uniform && param.index == 0 && entry.uniforms.empty())
                reflectUniforms(sv, entry);  // (the first uniform block: parameters by name)
            params.push_back(param);
        }

        // Each set must be contiguous from 0 and ordered by class.
        for (unsigned set : {sets.resourceSet, sets.rwSet, sets.uniformSet}) {
            if (set == ~0u)
                continue;
            std::vector<Param*> inSet;
            for (auto& p : params)
                if (p.set == set)
                    inSet.push_back(&p);
            std::sort(inSet.begin(), inSet.end(), [](Param* a, Param* b) { return a->index < b->index; });
            // SDL binds slots 0..count-1 per class, so unused slots below a used one are counted
            // toward the class of the next used resource (the host binds the full range).
            unsigned assigned = 0;
            const Param* prev = nullptr;
            for (Param* p : inSet) {
                if (p->index < assigned)
                    continue;  // another resource aliases this slot; count it once
                if (prev && (int)p->cls < (int)prev->cls)
                    error(src, entry.name + ": set " + std::to_string(set) + " must order " + clsName(p->cls) + " '" +
                                   p->name + "' before " + clsName(prev->cls) + " '" + prev->name + "'");
                if (prev && p->cls != prev->cls && p->index != assigned)
                    error(src, entry.name + ": set " + std::to_string(set) + " has unused slots " +
                                   std::to_string(assigned) + ".." + std::to_string(p->index - 1) + " between " +
                                   clsName(prev->cls) + " '" + prev->name + "' and " + clsName(p->cls) + " '" + p->name +
                                   "'; their class is ambiguous");
                entry.counts[(int)p->cls] += p->index + 1 - assigned;
                assigned = p->index + 1;
                prev = p;
            }
        }
        if (!entry.spirv.empty())
            toMsl(src, entry, params);
        entries.push_back(std::move(entry));
    }

    if (g_errors)
        return 1;
    if (entries.empty()) {
        error(src, "no entry points (mark them with [shader(\"vertex\")] etc.)");
        return 1;
    }

    if (!args.mslDir.empty()) {
        fs::create_directories(args.mslDir);
        for (const Entry& e : entries)
            if (!writeIfChanged(fs::path(args.mslDir) / (stem + "." + e.name + ".metal"), e.msl))
                error(src, "failed to write " + e.name + ".metal");
    }

    if (!args.blob.empty()) {
        fs::create_directories(fs::path(args.blob).parent_path());
        if (!writeBlob(args.blob, args.source, entries)) {
            error(src, "failed to write " + args.blob);
            return 1;
        }
        return 0;
    }

    // Emit header + data.
    std::ostringstream h, c;
    h << "// Generated by falcon_shaderc from " << args.source << ". Do not edit.\n#pragma once\n"
      << "#include \"gpu/shader_blob.h\"\n\nnamespace fx::shaders {\n";
    c << "// Generated by falcon_shaderc from " << args.source << ". Do not edit.\n#include \"" << stem
      << ".shader.h\"\n\nnamespace fx::shaders {\nnamespace {\n";
    for (const Entry& e : entries) {
        h << "extern const ShaderBlob " << e.symbol << ";\n";
        c << "alignas(4) const uint8_t " << e.symbol << "_spirv[] = {";
        emitBytes(c, e.spirv);
        c << "\n};\n";
        if (!e.dxil.empty()) {
            c << "alignas(4) const uint8_t " << e.symbol << "_dxil[] = {";
            emitBytes(c, e.dxil);
            c << "\n};\n";
        }
        c << "const uint8_t " << e.symbol << "_msl[] = {";
        emitBytes(c, std::vector<uint8_t>(e.msl.begin(), e.msl.end()));
        c << "\n};\n";
    }
    c << "}  // namespace\n\n";
    for (const Entry& e : entries) {
        const char* stage = e.stage == SLANG_STAGE_VERTEX     ? "ShaderStage::Vertex"
                            : e.stage == SLANG_STAGE_FRAGMENT ? "ShaderStage::Fragment"
                                                              : "ShaderStage::Compute";
        c << "const ShaderBlob " << e.symbol << " = {\"" << stem << "." << e.name << "\", " << stage << ", " << e.symbol
          << "_spirv, " << e.spirv.size() << ", " << (e.dxil.empty() ? std::string("nullptr") : e.symbol + "_dxil") << ", "
          << e.dxil.size() << ", \"" << e.name << "\", "
          << e.symbol << "_msl, " << e.msl.size() << ", \"" << e.mslEntry << "\", "
          << e.counts[(int)Cls::Sampler] << ", " << e.counts[(int)Cls::StorageTex] << ", "
          << e.counts[(int)Cls::StorageBuf] << ", " << e.counts[(int)Cls::Uniform] << ", "
          << e.counts[(int)Cls::RwStorageTex] << ", " << e.counts[(int)Cls::RwStorageBuf] << ", " << e.threads[0] << ", "
          << e.threads[1] << ", " << e.threads[2] << "};\n";
    }
    h << "}  // namespace fx::shaders\n";
    c << "}  // namespace fx::shaders\n";

    const fs::path outDir(args.outDir);
    fs::create_directories(outDir);
    const fs::path cppPath = outDir / (stem + ".shader.cpp");
    if (!writeIfChanged(outDir / (stem + ".shader.h"), h.str()) || !writeIfChanged(cppPath, c.str())) {
        error(src, "failed to write outputs");
        return 1;
    }

    if (!args.depfile.empty()) {
        std::set<std::string> deps;
        deps.insert(fs::absolute(srcPath).generic_string());
        for (SlangInt32 i = 0; i < module->getDependencyFileCount(); ++i)
            if (const char* d = module->getDependencyFilePath(i))
                deps.insert(fs::absolute(d).generic_string());
        std::ostringstream d;
        d << depEscape(fs::absolute(cppPath).generic_string()) << ":";
        for (const auto& dep : deps)
            d << " \\\n  " << depEscape(dep);
        d << "\n";
        writeIfChanged(args.depfile, d.str());
    }
    return 0;
}
