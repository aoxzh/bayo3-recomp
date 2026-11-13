// libb3python.so: the python command for the in-app setup, as an executable shipped as a lib*.so (Android only runs
// files from the APK's native library folder). CPython's official Android build is embed-only (libpython3.14.so);
// this is its plain main. The launcher sets PYTHONHOME to the standard library it unpacked into the app's files.
#include <Python.h>

int main(int argc, char** argv) {
    return Py_BytesMain(argc, argv);
}
