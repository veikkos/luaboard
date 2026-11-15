/* App and file names. See names.h. */
#include "names.h"

bool name_valid(const char *s, size_t n, size_t max)
{
    if (n == 0 || n >= max || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}
