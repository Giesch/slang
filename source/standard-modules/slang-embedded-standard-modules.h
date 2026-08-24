#ifndef SLANG_EMBEDDED_STANDARD_MODULES_H_INCLUDED
#define SLANG_EMBEDDED_STANDARD_MODULES_H_INCLUDED

#include "slang.h"

/// Return the serialized standard module embedded for `modulePath`, or nullptr.
///
/// `modulePath` is the hierarchical module name with `/` separators, as
/// `import slang.neural;` produces "slang/neural". The blob holds the same bytes
/// as the `.slang-module` file that the build writes for that module. Every
/// build defines this function; a build without SLANG_EMBED_STANDARD_MODULES
/// returns nullptr for every path.
ISlangBlob* slang_getEmbeddedStandardModule(const char* modulePath);

#endif // SLANG_EMBEDDED_STANDARD_MODULES_H_INCLUDED
