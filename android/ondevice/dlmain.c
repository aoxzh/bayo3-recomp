// libb3dlmain.so: an executable shipped as a lib*.so, so Android lets the app run it (only the APK's native library
// folder is executable since API 29; files the app writes are not). It runs a program the setup compiled on the phone
// as a shared library, which may be loaded from the app's files:
//     libb3dlmain.so <program.so> [args...]   calls the library's b3_main(argc, argv) with argv[0] = <program.so>
// The setup builds the Kraken decoder this way (its license keeps it out of the APK; tools/b3_win.py build_kraken).
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <program.so> [args...]\n", argv[0]);
        return 2;
    }
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "dlmain: %s\n", dlerror());
        return 127;
    }
    // Built with -Dmain=b3_main: a C program exports b3_main, a C++ one its mangled name, int main(int, char**) or
    // int main() (called with the arguments all the same: the AArch64 calling convention makes that harmless).
    int (*entry)(int, char**) = (int (*)(int, char**))dlsym(lib, "b3_main");
    if (!entry) entry = (int (*)(int, char**))dlsym(lib, "_Z7b3_mainiPPc");
    if (!entry) entry = (int (*)(int, char**))dlsym(lib, "_Z7b3_mainv");
    if (!entry) {
        fprintf(stderr, "dlmain: %s has no b3_main\n", argv[1]);
        return 127;
    }
    return entry(argc - 1, argv + 1);
}
