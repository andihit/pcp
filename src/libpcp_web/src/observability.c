#include <time.h>
#include "observability.h"

static MemStats memStats;

MemStats *memstats()
{
        return &memStats;
}

static long total()
{
        return memStats.maps + memStats.annotate + memStats.helptexts;
}

void print_memstats()
{
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        fprintf(stderr, "RSS: %li KB, count: %li KB\n", usage.ru_maxrss, total() / 1024);
}

static long last_rss = 0;

void print_memstats_info(const char *reason)
{
        time_t timer;
        char timebuf[10];
        struct tm *tm_info;

        timer = time(NULL);
        tm_info = localtime(&timer);
        strftime(timebuf, 10, "%H:%M:%S", tm_info);

        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);

        if (usage.ru_maxrss != last_rss)
        {
                fprintf(stderr, "[!!] %s RSS: %li KB, count: %li KB (%s), diff: %li KB\n", timebuf, usage.ru_maxrss, total() / 1024, reason, usage.ru_maxrss - last_rss);
        }
        else
        {
                //fprintf(stderr, "[--] %s RSS: %li KB, count: %li KB (%s)\n", timebuf, usage.ru_maxrss, total() / 1024, reason);
        }

        last_rss = usage.ru_maxrss;
}
