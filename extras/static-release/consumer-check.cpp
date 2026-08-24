// Link check for the bundled static archive produced by SLANG_BUNDLE_STATIC_LIB.
//
// This is compiled against nothing but the staged release tree -- its include/
// directory and the single merged archive in its lib/ directory. No CMake, no
// slang::slang target, no list of internal archives. That is the whole point of
// the bundle: a consumer that has never seen Slang's build system links one
// file.
//
// Creating and releasing a global session is enough to prove the archive is
// well formed. It resolves symbols across core, compiler-core and the core
// module, which live in different members of the merged archive, and it runs
// the static initializers of every bundled library. A malformed merge on any
// of the three per-platform paths -- GNU ar's MRI mode, macOS libtool,
// Windows lib.exe -- fails here at link time rather than silently shipping.
//
// Loading a module that imports slang.neural proves that the embedded standard
// modules (SLANG_EMBED_STANDARD_MODULES) are members of the archive as well.
// The check runs with no slang-standard-module-<version>/ directory next to
// the executable, so the import can only resolve from the embedded bytes.
//
// The embedded glslang/SPIRV-Tools path is covered separately, by the slangc
// invocations in release-static.yml.

#include <cstdio>
#include <slang.h>

static int checkEmbeddedStandardModule(slang::IGlobalSession* globalSession)
{
    slang::CompilerOptionEntry experimental = {};
    experimental.name = slang::CompilerOptionName::ExperimentalFeature;
    experimental.value.kind = slang::CompilerOptionValueKind::Int;
    experimental.value.intValue0 = 1;

    slang::TargetDesc target = {};
    target.format = SLANG_SPIRV;

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    sessionDesc.compilerOptionEntries = &experimental;
    sessionDesc.compilerOptionEntryCount = 1;

    slang::ISession* session = nullptr;
    const SlangResult result = globalSession->createSession(sessionDesc, &session);
    if (SLANG_FAILED(result) || session == nullptr)
    {
        std::fprintf(stderr, "createSession failed: 0x%08x\n", static_cast<unsigned int>(result));
        return 1;
    }

    ISlangBlob* diagnostics = nullptr;
    slang::IModule* module = session->loadModuleFromSourceString(
        "consumer-check",
        "consumer-check.slang",
        "import slang.neural;\n",
        &diagnostics);
    if (diagnostics != nullptr)
    {
        std::fprintf(stderr, "%s", static_cast<const char*>(diagnostics->getBufferPointer()));
        diagnostics->release();
    }
    if (module == nullptr)
    {
        std::fprintf(stderr, "import slang.neural did not resolve from the embedded module\n");
        session->release();
        return 1;
    }

    // The session owns the module; the caller holds no reference to release.
    session->release();
    return 0;
}

int main()
{
    slang::IGlobalSession* globalSession = nullptr;

    const SlangResult result = slang_createGlobalSession(SLANG_API_VERSION, &globalSession);
    if (SLANG_FAILED(result))
    {
        std::fprintf(
            stderr,
            "slang_createGlobalSession failed: 0x%08x\n",
            static_cast<unsigned int>(result));
        return 1;
    }
    if (globalSession == nullptr)
    {
        std::fprintf(stderr, "slang_createGlobalSession succeeded but returned null\n");
        return 1;
    }

    if (checkEmbeddedStandardModule(globalSession) != 0)
    {
        globalSession->release();
        return 1;
    }

    globalSession->release();

    std::printf("static consumer link check OK\n");
    return 0;
}
