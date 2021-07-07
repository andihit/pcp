#include <stdio.h>
#include <sys/resource.h>

typedef struct MemStats
{
    long maps;
    long annotate;
    long helptexts;
} MemStats;

MemStats* memstats();
void print_memstats();
void print_memstats_info(const char*);
