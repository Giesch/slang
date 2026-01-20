//DIAGNOSTIC_TEST:SIMPLE(filecheck=CHECK):

struct DefaultStruct_base
{
    int data0;
    __init()
    {
        data0 = 2;
    }
};

struct DefaultStruct1 : DefaultStruct_base
{
    int data1 = 1;
};

struct DefaultStruct2 : DefaultStruct_base
{

};

[numthreads(1, 1, 1)]
void computeMain(uint3 dispatchThreadID: SV_DispatchThreadID)
{
    //CHECK: error 30504: cannot use initializer list for type 'DefaultStruct1'
    //CHECK: note: 'DefaultStruct1' inherits from non-interface type 'DefaultStruct_base'
    DefaultStruct1 s1 = {};

    //CHECK: error 30504: cannot use initializer list for type 'DefaultStruct1'
    //CHECK: note: 'DefaultStruct1' inherits from non-interface type 'DefaultStruct_base'
    DefaultStruct1 s2 = {1};

    //CHECK: error 30504: cannot use initializer list for type 'DefaultStruct1'
    //CHECK: note: 'DefaultStruct1' inherits from non-interface type 'DefaultStruct_base'
    DefaultStruct1 s3 = {1, 2};

    //CHECK: error 30504: cannot use initializer list for type 'DefaultStruct2'
    //CHECK: note: 'DefaultStruct2' inherits from non-interface type 'DefaultStruct_base'
    DefaultStruct2 s4 = {};

    //CHECK: error 30504: cannot use initializer list for type 'DefaultStruct2'
    //CHECK: note: 'DefaultStruct2' inherits from non-interface type 'DefaultStruct_base'
    DefaultStruct2 s5 = {1};
}
