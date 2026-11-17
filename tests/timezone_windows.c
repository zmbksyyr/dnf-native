/* Use Windows' own conversion as the oracle without changing system settings. */
#include "../src/runtime/timezone.h"
#include <stdint.h>
#include <string.h>

int main(int argc, char **argv) {
    TIME_ZONE_INFORMATION zone;
    if (argc < 2) return 1;
    if (!strcmp(argv[1], "system")) {
        if (GetTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) return 2;
    } else {
        DYNAMIC_TIME_ZONE_INFORMATION dynamic;
        DWORD index = 0;
        int found = 0;
        while (EnumDynamicTimeZoneInformation(index++, &dynamic) == ERROR_SUCCESS) {
            char key[160];
            if (!WideCharToMultiByte(CP_UTF8, 0, dynamic.TimeZoneKeyName, -1, key, sizeof(key), NULL, NULL)) return 3;
            const char *requested = !strcmp(argv[1], "disabled-Pacific") ? "Pacific Standard Time" : argv[1];
            if (!strcmp(key, requested)) { found = 1; break; }
        }
        SYSTEMTIME now; GetSystemTime(&now);
        if (!found || !GetTimeZoneInformationForYear(now.wYear, &dynamic, &zone)) return 4;
        if (!strcmp(argv[1], "disabled-Pacific")) {
            memset(&zone.DaylightDate, 0, sizeof(zone.DaylightDate));
            memset(&zone.StandardDate, 0, sizeof(zone.StandardDate));
        }
    }
    char text[160];
    if (!nr_timezone_text(&zone, text)) return 5;
    puts(text);
    for (int i = 2; i < argc; ++i) {
        int64_t epoch = strtoll(argv[i], NULL, 10);
        ULARGE_INTEGER value;
        value.QuadPart = (uint64_t)epoch * 10000000ULL + 116444736000000000ULL;
        FILETIME file = {value.LowPart, value.HighPart};
        SYSTEMTIME utc, local;
        if (!FileTimeToSystemTime(&file, &utc) || !SystemTimeToTzSpecificLocalTime(&zone, &utc, &local)) return 6;
        FILETIME local_file;
        if (!SystemTimeToFileTime(&local, &local_file)) return 7;
        ULARGE_INTEGER wall; wall.LowPart = local_file.dwLowDateTime; wall.HighPart = local_file.dwHighDateTime;
        long minutes = (long)(((int64_t)wall.QuadPart - (int64_t)value.QuadPart) / 600000000LL);
        printf("SAMPLE %lld %04u-%02u-%02uT%02u:%02u:%02u%c%02ld%02ld\n", (long long)epoch,
               local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond,
               minutes < 0 ? '-' : '+', labs(minutes) / 60, labs(minutes) % 60);
    }
    return 0;
}
