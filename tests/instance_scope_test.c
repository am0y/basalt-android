#include "instance_scope.h"
#include <assert.h>
#include <stdio.h>

static uint8_t script[128], folder[128], actor[128];
static uint8_t descriptor[3][24], names[3][24];
static int read_test(uintptr_t address, void* output, size_t size)
{
    const struct { void* data; size_t size; } ranges[] = {
        {script,sizeof(script)},{folder,sizeof(folder)},{actor,sizeof(actor)},
        {descriptor,sizeof(descriptor)},{names,sizeof(names)},
    };
    for (unsigned i=0;i<sizeof(ranges)/sizeof(*ranges);++i)
    {
        uintptr_t begin=(uintptr_t)ranges[i].data;
        if (address>=begin && address-begin<=ranges[i].size && size<=ranges[i].size-(address-begin))
        { memcpy(output,(void*)address,size);return 1; }
    }
    return 0;
}
static void pointer(void* object,size_t offset,void* value) { memcpy((uint8_t*)object+offset,&value,8); }
int main(void)
{
    uint8_t* objects[]={script,folder,actor};
    const char* classes[]={"LocalScript","Folder","Actor"};
    for (unsigned i=0;i<3;++i)
    {
        names[i][0]=(uint8_t)(strlen(classes[i])*2);
        memcpy(names[i]+1,classes[i],strlen(classes[i]));
        pointer(descriptor[i],8,names[i]);
        pointer(objects[i],0x18,descriptor[i]);
    }
    pointer(script,0x68,folder);
    assert(instance_actor_scope((uintptr_t)script,read_test)==0);
    pointer(folder,0x68,actor);
    assert(instance_actor_scope((uintptr_t)script,read_test)==1);
    pointer(folder,0x68,folder);
    assert(instance_actor_scope((uintptr_t)script,read_test)==-1);
    assert(instance_actor_scope(0,read_test)==-1);
    assert(instance_actor_scope(1,read_test)==-1);
    puts("Actor VM filtering: main script, Actor descendant, cycle and unreadable cases OK");
    return 0;
}
