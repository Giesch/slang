//DIAGNOSTIC_TEST:SIMPLE(filecheck=CHECK):

// Test 1: Struct with explicit constructor used in a nested initializer list context.
// The explicit constructor path is skipped for non-C-style types in nested contexts,
// which triggers error 30504.
struct ExplicitCtorStruct
{
    int value;
    __init(int v) { value = v; }
};

struct ContainsNonCStyle
{
    ExplicitCtorStruct inner;
};

[numthreads(1, 1, 1)]
void computeMain(uint3 dispatchThreadID: SV_DispatchThreadID)
{
    //CHECK: error 30504: cannot use initializer list for type 'ExplicitCtorStruct'
    //CHECK: note: 'ExplicitCtorStruct' has an explicit constructor
    ContainsNonCStyle cns = {{10}};
}
