#ifndef DNF_NATIVE_TIMEZONE_H
#define DNF_NATIVE_TIMEZONE_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

/* POSIX TZ offsets, like Windows Bias, count minutes west of UTC.
   Linux libc can use these rules without a separate zoneinfo installation. */
static int nr_timezone_text(const TIME_ZONE_INFORMATION *zone, char text[160]) {
    long standard = zone->Bias + zone->StandardBias;
    long daylight = zone->Bias + zone->DaylightBias;
    int used = snprintf(text, 160, "TZ=WST%s%ld:%02ld", standard < 0 ? "-" : "",
                        labs(standard) / 60, labs(standard) % 60);
    if (!zone->DaylightDate.wMonth && !zone->StandardDate.wMonth) return 1;
    const SYSTEMTIME *dates[] = {&zone->DaylightDate, &zone->StandardDate};
    for (unsigned i = 0; i < 2; ++i) {
        const SYSTEMTIME *date = dates[i];
        /* Windows' recurring transition (week 5 means last) matches POSIX M.
           An absolute-year transition needs a transition table instead. */
        if (date->wYear || !date->wMonth || date->wMonth > 12 ||
            !date->wDay || date->wDay > 5 || date->wDayOfWeek > 6 ||
            date->wHour > 23 || date->wMinute > 59 || date->wSecond > 59 || date->wMilliseconds)
            return 0;
    }
    used += snprintf(text + used, 160 - (size_t)used, "WDT%s%ld:%02ld",
                     daylight < 0 ? "-" : "", labs(daylight) / 60, labs(daylight) % 60);
    for (unsigned i = 0; i < 2; ++i) {
        const SYSTEMTIME *date = dates[i];
        used += snprintf(text + used, 160 - (size_t)used, ",M%u.%u.%u/%u:%02u:%02u",
                         date->wMonth, date->wDay, date->wDayOfWeek,
                         date->wHour, date->wMinute, date->wSecond);
    }
    return 1;
}

#endif
