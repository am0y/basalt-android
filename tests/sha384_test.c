#include "sha384.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    Sha384 state;char output[97];
    sha384_init(&state);sha384_update(&state,"abc",3);sha384_finish(&state,output);
    assert(!strcmp(output,"cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"));
    sha384_init(&state);for(int i=0;i<1000000;++i)sha384_update(&state,"a",1);sha384_finish(&state,output);
    assert(!strcmp(output,"9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985"));
    puts("SHA-384 known-answer tests OK");
}
