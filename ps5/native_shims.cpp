/*
 * PS5 port contributions and enhancements: Youss Riaal, 2026.
 * SPDX-License-Identifier: GPL-3.0-only
 * Applies to original port contributions only; see COPYRIGHT.md and NOTICE.md.
 * Preserve existing upstream notices. No relicensing of original game content.
 */
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
extern "C" int sceSystemServiceLoadExec(const char*,char**);
extern "C" void catchReturnFromMain(int status){
 fprintf(stderr,"Native title closing through system service: %d\n",status);fflush(nullptr);
 sceSystemServiceLoadExec("exit",nullptr);
 for(;;)usleep(100000);
}
extern "C" [[noreturn]] void __assert(const char*function,const char*file,int line,const char*condition){
 fprintf(stderr,"Assertion failed: %s (%s:%d %s)\n",condition,file,line,function);abort();
}
