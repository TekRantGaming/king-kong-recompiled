#include "kkshaders/compiler.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>  // IUnknown (WIN32_LEAN_AND_MEAN leaves out the OLE headers)
#else
#include <dlfcn.h>
#endif

#include <dxcapi.h>

namespace kkshaders {

namespace {

std::mutex gLoadMutex;
DxcCreateInstanceProc gCreateInstance = nullptr;
std::string gVersion;

template <typename T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

}  // namespace

bool loadDxc(const std::string& pathIn, std::string* error) {
    std::lock_guard<std::mutex> lock(gLoadMutex);
    if (gCreateInstance) return true;
    namespace fs = std::filesystem;
#ifdef _WIN32
    const char* libName = "dxcompiler.dll";
#else
    const char* libName = "libdxcompiler.so";
#endif
    std::string path = pathIn;
    if (!path.empty() && fs::is_directory(path)) {
        fs::path dir(path);
        if (fs::exists(dir / libName)) path = (dir / libName).string();
        else if (fs::exists(dir / "lib" / libName)) path = (dir / "lib" / libName).string();
        else if (fs::exists(dir / "bin" / libName)) path = (dir / "bin" / libName).string();
        else if (fs::exists(dir / "bin" / "x64" / libName)) path = (dir / "bin" / "x64" / libName).string();
    }
    if (path.empty()) path = libName;

#ifdef _WIN32
    HMODULE module = LoadLibraryExA(path.c_str(), nullptr, path.find_first_of("/\\") != std::string::npos ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
    if (!module) {
        if (error) *error = "cannot load " + path;
        return false;
    }
    gCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
#else
    // The validator library is loaded by name by the compiler: load it first from the same folder.
    fs::path dxil = fs::path(path).parent_path() / "libdxil.so";
    if (fs::exists(dxil)) dlopen(dxil.c_str(), RTLD_NOW | RTLD_GLOBAL);
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        if (error) *error = "cannot load " + path + ": " + dlerror();
        return false;
    }
    gCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(dlsym(module, "DxcCreateInstance"));
#endif
    if (!gCreateInstance) {
        if (error) *error = path + " has no DxcCreateInstance";
        return false;
    }

    Ref<IDxcVersionInfo> info;
    Ref<IDxcCompiler3> compiler;
    if (SUCCEEDED(gCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())))) {
        if (SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(info.put())))) {
            UINT32 major = 0, minor = 0;
            info->GetVersion(&major, &minor);
            gVersion = std::to_string(major) + "." + std::to_string(minor);
        }
    }
    Ref<IDxcValidator> validator;
    if (FAILED(gCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(validator.put())))) {
        gVersion += " (no validator: DXIL will not be signed)";
    } else {
        Ref<IDxcVersionInfo> vinfo;
        if (SUCCEEDED(validator->QueryInterface(IID_PPV_ARGS(vinfo.put())))) {
            UINT32 major = 0, minor = 0;
            vinfo->GetVersion(&major, &minor);
            gVersion += ", validator " + std::to_string(major) + "." + std::to_string(minor);
        }
    }
    return true;
}

bool dxcLoaded() { return gCreateInstance != nullptr; }

std::string dxcVersion() { return gVersion; }

struct Compiler::Impl {
    Ref<IDxcCompiler3> compiler;
    Ref<IDxcUtils> utils;
    Ref<IDxcValidator> validator;
};

Compiler::Compiler() : impl_(std::make_unique<Impl>()) {
    if (!gCreateInstance) return;
    gCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(impl_->compiler.put()));
    gCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(impl_->utils.put()));
    gCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(impl_->validator.put()));
}

Compiler::~Compiler() = default;

bool Compiler::ok() const { return bool(impl_->compiler); }

CompileOutput Compiler::compile(const std::string& hlsl, ShaderKind kind, CompileTarget target) {
    CompileOutput out;
    if (!impl_->compiler) {
        out.messages = "DXC is not loaded";
        return out;
    }
    std::vector<std::wstring> args = {L"-T", kind == ShaderKind::Vertex ? L"vs_6_0" : L"ps_6_0", L"-E", L"main",
                                      L"-HV", L"2021", L"-O3", L"-Qstrip_debug"};
    if (target == CompileTarget::Spirv) {
        args.insert(args.end(), {L"-spirv", L"-fspv-target-env=vulkan1.2", L"-fvk-use-dx-layout"});
        if (kind == ShaderKind::Vertex) args.push_back(L"-fvk-invert-y");
    } else {
        args.push_back(L"-Qstrip_reflect");
    }
    std::vector<LPCWSTR> argv;
    for (const auto& a : args) argv.push_back(a.c_str());

    DxcBuffer source{};
    source.Ptr = hlsl.data();
    source.Size = hlsl.size();
    source.Encoding = DXC_CP_UTF8;

    Ref<IDxcResult> result;
    HRESULT hr = impl_->compiler->Compile(&source, argv.data(), UINT32(argv.size()), nullptr, IID_PPV_ARGS(result.put()));
    if (FAILED(hr) || !result) {
        out.messages = "DXC Compile call failed";
        return out;
    }
    Ref<IDxcBlobUtf8> errors;
    if (result->HasOutput(DXC_OUT_ERRORS) &&
        SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.put()), nullptr)) && errors &&
        errors->GetStringLength() > 0) {
        out.messages.assign(errors->GetStringPointer(), errors->GetStringLength());
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) return out;
    Ref<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.put()), nullptr)) || !object) {
        out.messages += "no object produced";
        return out;
    }
    const uint8_t* p = static_cast<const uint8_t*>(object->GetBufferPointer());
    out.blob.assign(p, p + object->GetBufferSize());
    out.ok = !out.blob.empty();
    return out;
}

bool Compiler::validateDxil(const std::vector<uint8_t>& blob, std::string* messages) {
    if (!impl_->validator || !impl_->utils) {
        if (messages) *messages = "no DXIL validator";
        return false;
    }
    Ref<IDxcBlobEncoding> source;
    if (FAILED(impl_->utils->CreateBlob(blob.data(), UINT32(blob.size()), DXC_CP_ACP, source.put()))) {
        if (messages) *messages = "cannot make a blob";
        return false;
    }
    Ref<IDxcOperationResult> result;
    if (FAILED(impl_->validator->Validate(source.p, DxcValidatorFlags_Default, result.put())) || !result) {
        if (messages) *messages = "validator call failed";
        return false;
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status) && messages) {
        Ref<IDxcBlobEncoding> errors;
        if (SUCCEEDED(result->GetErrorBuffer(errors.put())) && errors)
            messages->assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    }
    return SUCCEEDED(status);
}

bool isDxilSigned(const std::vector<uint8_t>& blob) {
    // DxilContainerHeader: 'DXBC', then a 16-byte digest the validator fills in.
    if (blob.size() < 20 || std::memcmp(blob.data(), "DXBC", 4) != 0) return false;
    for (size_t i = 4; i < 20; i++)
        if (blob[i] != 0) return true;
    return false;
}

bool validateSpirvExternal(const std::string& spirvVal, const std::vector<uint8_t>& blob, std::string* messages) {
    namespace fs = std::filesystem;
    static std::atomic<uint32_t> counter{0};
    std::ostringstream name;
    name << "kkshaders-" << std::hash<std::thread::id>{}(std::this_thread::get_id()) << "-" << counter++;
    fs::path base = fs::temp_directory_path() / name.str();
    fs::path spv = base;
    spv += ".spv";
    fs::path log = base;
    log += ".txt";
    {
        std::ofstream f(spv, std::ios::binary);
        f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    }
    std::string command = "\"" + spirvVal + "\" --target-env vulkan1.2 \"" + spv.string() + "\" > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    int rc = std::system(command.c_str());
    if (messages) {
        std::ifstream f(log);
        std::stringstream ss;
        ss << f.rdbuf();
        *messages = ss.str();
    }
    std::error_code ec;
    fs::remove(spv, ec);
    fs::remove(log, ec);
    return rc == 0;
}

}  // namespace kkshaders
