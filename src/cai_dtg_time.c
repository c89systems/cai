#define _POSIX_C_SOURCE 200809L
#include <cai/dtg.h>
#include <string.h>
static int calendar(time_t when, int utc, struct tm *out) {
  return (utc ? gmtime_r(&when, out) : localtime_r(&when, out)) != NULL;
}
static int parts(time_t when, int utc, const struct tm *date, cai_dtg *out) {
  cai_dtg value;
  struct tm greenwich;
  int days;
  memset(&value, 0, sizeof(value));
  value.day = date->tm_mday;
  value.hour = date->tm_hour;
  value.minute = date->tm_min;
  value.month = date->tm_mon + 1;
  value.year = (date->tm_year % 100 + 100) % 100;
  value.has_month_year = value.has_zone = value.offset_known = 1;
  if (!utc) {
    if (!calendar(when, 1, &greenwich))
      return CAI_DTG_RANGE;
    days = date->tm_yday - greenwich.tm_yday;
    if (date->tm_year > greenwich.tm_year)
      days = 1;
    else if (date->tm_year < greenwich.tm_year)
      days = -1;
    value.offset_seconds = (long)days * 86400L +
                           (long)(date->tm_hour - greenwich.tm_hour) * 3600L +
                           (long)(date->tm_min - greenwich.tm_min) * 60L +
                           date->tm_sec - greenwich.tm_sec;
  }
  if (cai_dtg_zone(value.offset_seconds, value.zone, sizeof(value.zone)) !=
      CAI_DTG_OK)
    return CAI_DTG_RANGE;
  *out = value;
  return CAI_DTG_OK;
}
int cai_dtg_time_parts(time_t when, int utc, cai_dtg *out) {
  struct tm date;
  if (out == NULL)
    return CAI_DTG_INVALID;
  if (!calendar(when, utc, &date))
    return CAI_DTG_RANGE;
  return parts(when, utc, &date, out);
}
int cai_dtg_format_time(time_t when, time_t reference, int utc, char *out,
                        size_t capacity) {
  struct tm date, now;
  cai_dtg value;
  int rc;
  if (out == NULL)
    return CAI_DTG_INVALID;
  if (!calendar(when, utc, &date) || !calendar(reference, utc, &now))
    return CAI_DTG_RANGE;
  rc = parts(when, utc, &date, &value);
  if (rc != CAI_DTG_OK)
    return rc;
  if (date.tm_year == now.tm_year && date.tm_mon == now.tm_mon) {
    value.has_month_year = 0;
    value.has_zone = utc != 0;
  }
  return cai_dtg_format(&value, out, capacity);
}
