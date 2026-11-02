#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/dtg.h>
#include <stdlib.h>
#include <string.h>
static time_t instant(int year, int month, int day, int hour, int minute) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  return mktime(&tm);
}
int main(void) {
  static const struct {
    long offset;
    const char *zone;
  } cases[] = {{-43200L, "Y"}, {-39600L, "X"},  {-36000L, "W"}, {-34200L, "V*"},
               {-32400L, "V"}, {-28800L, "U"},  {-25200L, "T"}, {-21600L, "S"},
               {-18000L, "R"}, {-16200L, "Q*"}, {-14400L, "Q"}, {-12600L, "P*"},
               {-10800L, "P"}, {-7200L, "O"},   {-3600L, "N"},  {0L, "Z"},
               {3600L, "A"},   {7200L, "B"},    {10800L, "C"},  {12600L, "C*"},
               {14400L, "D"},  {16200L, "D*"},  {18000L, "E"},  {19800L, "E*"},
               {21600L, "F"},  {23400L, "F*"},  {25200L, "G"},  {28800L, "H"},
               {30600L, "H*"}, {32400L, "I"},   {34200L, "I*"}, {36000L, "K"},
               {37800L, "K*"}, {39600L, "L"},   {41400L, "L*"}, {43200L, "M"},
               {46800L, "M*"}, {20700L, "J"},   {45900L, "J"},  {50400L, "J"},
               {31500L, "J"},  {49500L, "J"},   {1172L, "J"}};
  static const char *bad[] = {"",
                              "02123",
                              "001230",
                              "321230",
                              "022400",
                              "021260",
                              "021230JUNK",
                              "021230ANOV2026",
                              "021230AABC26",
                              "311230AAPR26",
                              "291230AFEB25",
                              "021230E+NOV26",
                              "021230+0530NOV26",
                              "021230ANOV26extra",
                              "021230A\nNOV26",
                              "021230E\342\200\240NOV26"};
  cai_dtg value, original;
  char text[CAI_DTG_CAPACITY], zone[CAI_DTG_ZONE_CAPACITY];
  time_t now, old, july, winter;
  size_t i;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    assert(cai_dtg_zone(cases[i].offset, zone, sizeof(zone)) == CAI_DTG_OK);
    assert(strcmp(zone, cases[i].zone) == 0);
    strcpy(text, "021230");
    strcat(text, zone);
    strcat(text, "NOV26");
    assert(cai_dtg_parse(text, &value) == CAI_DTG_OK);
    assert(value.year == 26 && value.month == 11 && value.day == 2);
    assert(value.hour == 12 && value.minute == 30 && value.has_month_year);
    assert(value.offset_known == (strcmp(zone, "J") != 0));
    if (value.offset_known)
      assert(value.offset_seconds == cases[i].offset);
    assert(cai_dtg_format(&value, text, sizeof(text)) == CAI_DTG_OK);
  }
  assert(cai_dtg_parse("021230", &value) == CAI_DTG_OK && !value.has_zone &&
         !value.has_month_year);
  assert(cai_dtg_parse("021230Z", &value) == CAI_DTG_OK && value.has_zone &&
         !value.has_month_year);
  assert(cai_dtg_parse("091630tjul11", &value) == CAI_DTG_OK &&
         value.year == 11 && value.offset_seconds == -25200L);
  assert(cai_dtg_format(&value, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "091630TJUL11") == 0);
  assert(cai_dtg_parse("291230ZFEB00", &value) == CAI_DTG_OK);
  original = value;
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    assert(cai_dtg_parse(bad[i], &value) == CAI_DTG_INVALID);
    assert(memcmp(&value, &original, sizeof(value)) == 0);
  }
  strcpy(text, "unchanged");
  assert(cai_dtg_format(&value, text, 3U) == CAI_DTG_SPACE &&
         strcmp(text, "unchanged") == 0);
  assert(cai_dtg_zone(86400L, text, sizeof(text)) == CAI_DTG_RANGE);
  assert(cai_dtg_parse(NULL, &value) == CAI_DTG_INVALID);
  assert(setenv("TZ", "UTC0", 1) == 0);
  tzset();
  now = instant(2026, 11, 2, 12, 30);
  old = instant(2026, 10, 2, 12, 30);
  july = instant(2026, 7, 2, 12, 30);
  winter = instant(2026, 1, 2, 12, 30);
  assert(cai_dtg_format_time(now, now, 0, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "021230") == 0);
  assert(cai_dtg_format_time(now, now, 1, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "021230Z") == 0);
  assert(cai_dtg_format_time(old, now, 1, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "021230ZOCT26") == 0);
  assert(cai_dtg_format_time(instant(2025, 11, 2, 12, 30), now, 1, text,
                             sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "021230ZNOV25") == 0);
  if (sizeof(time_t) > 4U)
    assert(cai_dtg_format_time(instant(2000, 11, 2, 12, 30),
                               instant(2100, 11, 2, 12, 30), 1, text,
                               sizeof(text)) == CAI_DTG_OK &&
           strcmp(text, "021230ZNOV00") == 0);
  assert(setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1) == 0);
  tzset();
  assert(cai_dtg_format_time(july, now, 0, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "021430BJUL26") == 0);
  assert(cai_dtg_format_time(winter, now, 0, text, sizeof(text)) ==
             CAI_DTG_OK &&
         strcmp(text, "021330AJAN26") == 0);
  assert(strcmp(getenv("TZ"), "CET-1CEST,M3.5.0/2,M10.5.0/3") == 0);
  assert(setenv("TZ", "NPT-5:45", 1) == 0);
  tzset();
  assert(cai_dtg_time_parts(now, 0, &value) == CAI_DTG_OK &&
         value.offset_known && value.offset_seconds == 20700L &&
         strcmp(value.zone, "J") == 0);
  assert(cai_dtg_format_time(july, now, 0, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "021815JJUL26") == 0);
  assert(setenv("TZ", "LHST-10:30", 1) == 0);
  tzset();
  assert(cai_dtg_time_parts(now, 0, &value) == CAI_DTG_OK &&
         strcmp(value.zone, "K*") == 0);
  assert(cai_dtg_format_time(old, now, 0, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "022300K*OCT26") == 0);
  assert(setenv("TZ", "LINT-14", 1) == 0);
  tzset();
  old = instant(2026, 1, 1, 0, 30);
  assert(cai_dtg_format_time(old, now, 0, text, sizeof(text)) == CAI_DTG_OK &&
         strcmp(text, "010030JJAN26") == 0);
  assert(cai_dtg_time_parts(old, 0, &value) == CAI_DTG_OK &&
         value.offset_seconds == 50400L);
  assert(setenv("TZ", "IDL12", 1) == 0);
  tzset();
  assert(cai_dtg_time_parts(old, 0, &value) == CAI_DTG_OK && value.year == 25 &&
         value.month == 12 && value.day == 30 && value.hour == 22 &&
         value.offset_seconds == -43200L);
  return 0;
}
