/* Petites fonctions de la bibliothèque C que mingw attend et que msvcrt.dll fournit sous un autre nom. */
#include <stdarg.h>
#include <stddef.h>
int _vsnprintf(char*, size_t, const char*, va_list);
int _vscprintf(const char*, va_list);
int __ms_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    if (!buf || n == 0) return _vscprintf(fmt, ap);
    int r = _vsnprintf(buf, n, fmt, ap);
    if (r < 0 || (size_t)r >= n) { buf[n - 1] = 0; return _vscprintf(fmt, ap); }
    return r;
}
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) { return __ms_vsnprintf(buf, n, fmt, ap); }
