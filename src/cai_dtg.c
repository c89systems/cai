#include <cai/dtg.h>
#include <string.h>
typedef struct dtg_zone {
  long seconds;
  const char *name;
} dtg_zone;
/* HM Nautical Almanac Office, Standard Time Zones, November 2021,
 * https://aa.usno.navy.mil/downloads/TimeZoneMap_2021.pdf */
static const dtg_zone zones[] = {
    {-43200L, "Y"}, {-39600L, "X"},  {-36000L, "W"}, {-34200L, "V*"},
    {-32400L, "V"}, {-28800L, "U"},  {-25200L, "T"}, {-21600L, "S"},
    {-18000L, "R"}, {-16200L, "Q*"}, {-14400L, "Q"}, {-12600L, "P*"},
    {-10800L, "P"}, {-7200L, "O"},   {-3600L, "N"},  {0L, "Z"},
    {3600L, "A"},   {7200L, "B"},    {10800L, "C"},  {12600L, "C*"},
    {14400L, "D"},  {16200L, "D*"},  {18000L, "E"},  {19800L, "E*"},
    {21600L, "F"},  {23400L, "F*"},  {25200L, "G"},  {28800L, "H"},
    {30600L, "H*"}, {32400L, "I"},   {34200L, "I*"}, {36000L, "K"},
    {37800L, "K*"}, {39600L, "L"},   {41400L, "L*"}, {43200L, "M"},
    {46800L, "M*"}};
static const char months[12][4] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
static char upper(char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }
static int copy_text(const char *text, char *out, size_t capacity) {
  size_t n = strlen(text);
  if (out == NULL)
    return CAI_DTG_INVALID;
  if (n >= capacity)
    return CAI_DTG_SPACE;
  memcpy(out, text, n + 1U);
  return CAI_DTG_OK;
}
int cai_dtg_zone(long offset, char *out, size_t capacity) {
  size_t i;
  if (offset <= -86400L || offset >= 86400L)
    return CAI_DTG_RANGE;
  for (i = 0; i < sizeof(zones) / sizeof(zones[0]); i++)
    if (zones[i].seconds == offset)
      return copy_text(zones[i].name, out, capacity);
  return copy_text("J", out, capacity);
}
static int zone_parse(cai_dtg *value) {
  size_t i;
  value->zone[0] = upper(value->zone[0]);
  if (strcmp(value->zone, "J") == 0) {
    value->offset_known = 0;
    value->offset_seconds = 0;
    return CAI_DTG_OK;
  }
  for (i = 0; i < sizeof(zones) / sizeof(zones[0]); i++)
    if (strcmp(value->zone, zones[i].name) == 0) {
      value->offset_known = 1;
      value->offset_seconds = zones[i].seconds;
      return CAI_DTG_OK;
    }
  return CAI_DTG_INVALID;
}
static int valid(const cai_dtg *v) {
  static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max = 31;
  if (v->hour < 0 || v->hour > 23 || v->minute < 0 || v->minute > 59)
    return 0;
  if (v->has_month_year) {
    if (!v->has_zone || v->month < 1 || v->month > 12 || v->year < 0 ||
        v->year > 99)
      return 0;
    max = days[v->month - 1] + (v->month == 2 && v->year % 4 == 0);
  }
  return v->day > 0 && v->day <= max;
}
int cai_dtg_parse(const char *text, cai_dtg *out) {
  cai_dtg value;
  size_t n, z, i;
  int digits[6], month;
  if (text == NULL || out == NULL)
    return CAI_DTG_INVALID;
  for (n = 0; n < CAI_DTG_CAPACITY && text[n] != '\0'; n++) {
  }
  if (n < 6 || n >= CAI_DTG_CAPACITY)
    return CAI_DTG_INVALID;
  memset(&value, 0, sizeof(value));
  for (i = 0; i < 6; i++) {
    if (text[i] < '0' || text[i] > '9')
      return CAI_DTG_INVALID;
    digits[i] = text[i] - '0';
  }
  value.day = digits[0] * 10 + digits[1];
  value.hour = digits[2] * 10 + digits[3];
  value.minute = digits[4] * 10 + digits[5];
  z = n - 6;
  if (n >= 12) {
    value.has_month_year = 1;
    z -= 5;
    if (text[n - 2] < '0' || text[n - 2] > '9' || text[n - 1] < '0' ||
        text[n - 1] > '9')
      return CAI_DTG_INVALID;
    value.year = (text[n - 2] - '0') * 10 + text[n - 1] - '0';
    for (month = 0; month < 12; month++)
      if (upper(text[n - 5]) == months[month][0] &&
          upper(text[n - 4]) == months[month][1] &&
          upper(text[n - 3]) == months[month][2])
        break;
    if (month == 12)
      return CAI_DTG_INVALID;
    value.month = month + 1;
  }
  if (z >= sizeof(value.zone))
    return CAI_DTG_INVALID;
  if (z != 0) {
    memcpy(value.zone, text + 6, z);
    value.zone[z] = '\0';
    value.has_zone = 1;
    if (zone_parse(&value) != CAI_DTG_OK)
      return CAI_DTG_INVALID;
  }
  if (!valid(&value))
    return CAI_DTG_INVALID;
  *out = value;
  return CAI_DTG_OK;
}
static void pair(char *out, int value) {
  out[0] = (char)('0' + value / 10);
  out[1] = (char)('0' + value % 10);
}
int cai_dtg_format(const cai_dtg *value, char *out, size_t capacity) {
  cai_dtg v;
  char text[CAI_DTG_CAPACITY];
  size_t n = 6, z = 0;
  if (value == NULL || out == NULL || !valid(value))
    return CAI_DTG_INVALID;
  v = *value;
  if (v.has_zone) {
    for (z = 0; z < sizeof(v.zone) && v.zone[z] != '\0'; z++) {
    }
    if (z == sizeof(v.zone) || zone_parse(&v) != CAI_DTG_OK)
      return CAI_DTG_INVALID;
  }
  pair(text, v.day);
  pair(text + 2, v.hour);
  pair(text + 4, v.minute);
  if (z) {
    memcpy(text + n, v.zone, z);
    n += z;
  }
  if (v.has_month_year) {
    memcpy(text + n, months[v.month - 1], 3U);
    n += 3;
    pair(text + n, v.year);
    n += 2;
  }
  text[n] = '\0';
  return copy_text(text, out, capacity);
}
