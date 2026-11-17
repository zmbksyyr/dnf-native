/* Compare actual packaged glibc calendar conversion and Linux clocks. */
typedef __SIZE_TYPE__ size_t;
struct pair { long seconds, fraction; };
struct calendar {
    int second, minute, hour, day, month, year, weekday, yearday, daylight;
    long offset;
    const char *zone;
};
extern long time(long *);
extern int gettimeofday(struct pair *, void *);
extern int clock_gettime(int, struct pair *);
extern struct calendar *localtime_r(const long *, struct calendar *);
extern long mktime(struct calendar *);
extern size_t strftime(char *, size_t, const char *, const struct calendar *);
extern char *getenv(const char *);
extern long strtol(const char *, char **, int);
extern int printf(const char *, ...);
extern int __libc_start_main(int (*)(void), int, char **, void *, void *, void *, void *);
extern void _exit(int) __attribute__((noreturn));
#define CHECK(c) do { if (!(c)) _exit(__LINE__); } while (0)

static void report(const char *label, long value) {
    struct calendar local;
    char text[80];
    CHECK(localtime_r(&value, &local));
    CHECK(strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S%z", &local));
    CHECK(mktime(&local) == value);
    printf("%s %ld %s\n", label, value, text);
}

static int main_test(void) {
    long now = time(0);
    struct pair wall, realtime, monotonic, later;
    CHECK(gettimeofday(&wall, 0) == 0 && clock_gettime(0, &realtime) == 0);
    CHECK(now <= wall.seconds && wall.seconds <= now + 2);
    CHECK(now <= realtime.seconds && realtime.seconds <= now + 2);
    CHECK(wall.fraction >= 0 && wall.fraction < 1000000);
    CHECK(realtime.fraction >= 0 && realtime.fraction < 1000000000);
    CHECK(clock_gettime(1, &monotonic) == 0 && clock_gettime(1, &later) == 0);
    CHECK(later.seconds > monotonic.seconds ||
          (later.seconds == monotonic.seconds && later.fraction >= monotonic.fraction));
    report("NOW", now);
    char *samples = getenv("DNF_TIME_SAMPLES");
    CHECK(samples);
    while (*samples) {
        char *end;
        long value = strtol(samples, &end, 10);
        CHECK(end != samples);
        report("SAMPLE", value);
        samples = *end == ',' ? end + 1 : end;
    }
    return 42;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    __libc_start_main(main_test, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
