// Smoke test for the embedded slang.neural standard module.
//
// slang.neural is a pre-compiled .slang-module that a static build embeds in
// the library (SLANG_EMBED_STANDARD_MODULES). A build that lost the embedded
// bytes fails here with a module-not-found diagnostic. The module is marked
// experimental, so the compile needs -experimental-feature.

import slang.neural;

RWStructuredBuffer<float> result;

[shader("compute")]
[numthreads(4, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    result[tid.x] = float(tid.x);
}
