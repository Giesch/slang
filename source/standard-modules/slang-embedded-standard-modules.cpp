#include "slang-embedded-standard-modules.h"

#include "../core/slang-blob.h"

#include <string.h>

#ifdef SLANG_EMBED_STANDARD_MODULES

static const uint8_t g_neuralModule[] = {
#include "slang-neural-module-generated.h"
};

static const uint8_t g_workgraphModule[] = {
#include "slang-workgraph-module-generated.h"
};

static Slang::StaticBlob g_neuralModuleBlob((const void*)g_neuralModule, sizeof(g_neuralModule));
static Slang::StaticBlob g_workgraphModuleBlob(
    (const void*)g_workgraphModule,
    sizeof(g_workgraphModule));

ISlangBlob* slang_getEmbeddedStandardModule(const char* modulePath)
{
    if (strcmp(modulePath, "slang/neural") == 0)
        return &g_neuralModuleBlob;
    if (strcmp(modulePath, "experimental/workgraph") == 0)
        return &g_workgraphModuleBlob;
    return nullptr;
}

#else

ISlangBlob* slang_getEmbeddedStandardModule(const char* modulePath)
{
    SLANG_UNUSED(modulePath);
    return nullptr;
}

#endif
