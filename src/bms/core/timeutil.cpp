#include "timeutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// Howard Hinnant's days_from_civil - aantal dagen sinds 1970-01-01.
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long) doe - 719468;
}

time_t utc_epoch(int year, int month, int day, int hour, int minute, int second)
{
    return (time_t) days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

ems_date_t date_from_epoch_local(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

ems_date_t date_today(void)
{
    return date_from_epoch_local(time(nullptr));
}

ems_date_t date_add_days(ems_date_t d, int days)
{
    // Puur kalenderrekenen via UTC (geen DST-gevoeligheid).
    long n = days_from_civil(d.year, d.month, d.day) + days;
    time_t t = (time_t) n * 86400;
    struct tm tm;
    gmtime_r(&t, &tm);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

bool date_equal(ems_date_t a, ems_date_t b)
{
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

void date_iso(ems_date_t d, char * out)
{
    snprintf(out, 11, "%04d-%02d-%02d", d.year, d.month, d.day);
}

bool date_parse_iso(const char * s, ems_date_t * out)
{
    return sscanf(s, "%4d-%2d-%2d", &out->year, &out->month, &out->day) == 3;
}

int date_weekday(ems_date_t d)
{
    long n = days_from_civil(d.year, d.month, d.day);
    // 1970-01-01 was een donderdag (Python weekday 3).
    return (int) (((n % 7) + 7 + 3) % 7);
}

time_t local_time_of(ems_date_t d, int hour, int minute)
{
    struct tm tm = {};
    tm.tm_year = d.year - 1900;
    tm.tm_mon = d.month - 1;
    tm.tm_mday = d.day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

time_t local_midnight(ems_date_t d)
{
    return local_time_of(d, 0, 0);
}

void iso_utc(time_t t, char * out, int out_size)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%S+00:00", &tm);
}

void iso_utc_now(char * out, int out_size)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    char base[24];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out, out_size, "%s.%06ld+00:00", base, (long) tv.tv_usec);
}

bool parse_iso8601(const char * s, time_t * out)
{
    int y, mo, d, h, mi, sec;
    int consumed = 0;
    if(sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &consumed) != 6) {
        return false;
    }
    const char * p = s + consumed;
    if(*p == '.') {
        p++;
        while(*p >= '0' && *p <= '9') {
            p++;
        }
    }
    int offset_s = 0;
    if(*p == 'Z' || *p == 'z' || *p == '\0') {
        offset_s = 0;
    } else if(*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if(sscanf(p + 1, "%2d:%2d", &oh, &om) < 1) {
            return false;
        }
        offset_s = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
    } else {
        return false;
    }
    *out = utc_epoch(y, mo, d, h, mi, sec) - offset_s;
    return true;
}
