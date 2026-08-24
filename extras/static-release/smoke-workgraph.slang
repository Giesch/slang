// Smoke test for the embedded experimental.workgraph standard module.
//
// The module is marked experimental, so the compile needs -experimental-feature.
// Work graphs exist only on HLSL, so this file is compiled with -target hlsl.

import experimental.workgraph;

struct MyRecord
{
    int value;
};

[shader("node")]
[NodeLaunch("coalescing")]
[MaxRecords(4)]
[numthreads(4, 1, 1)]
void main(GroupNodeInputRecords<MyRecord> input)
{
    uint count = input.Count();
}
