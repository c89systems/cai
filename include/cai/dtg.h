/** @file cai/dtg.h
 * Allocation-free date-time groups. The syntax/zone unit has no CAI or
 * platform dependencies; the time adapter uses the host's local timezone.
 */
#ifndef CAI_DTG_H
#define CAI_DTG_H
#include <stddef.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CAI_DTG_ZONE_CAPACITY 8
#define CAI_DTG_CAPACITY 24
enum cai_dtg_status {
  CAI_DTG_OK = 0,
  CAI_DTG_INVALID = 1,
  CAI_DTG_SPACE = 2,
  CAI_DTG_RANGE = 3
};
typedef struct cai_dtg {
  int day, hour, minute;
  /** Month 1..12 and year 00..99, meaningful only with has_month_year.
   * Parsing never guesses a century or turns incomplete dates into epochs. */
  int month, year, has_month_year;
  int has_zone;
  char zone[CAI_DTG_ZONE_CAPACITY];
  /** Seconds east of UTC. Parsed J has offset_known=0; time-derived J retains
   * the exact host offset even though the chart has no specific designator. */
  long offset_seconds;
  int offset_known;
} cai_dtg;
/** ASCII USNO/HMNAO zone designator, including star suffixes. Offsets with
 * Unicode designators or no listed code use J (local), never rounded time. */
int cai_dtg_zone(long offset_seconds, char *out, size_t capacity);
/** Parse compact DDHHMM[ZONE] or DDHHMMZONEMMMYY. ASCII letters are case
 * insensitive. Invalid input leaves out unchanged. Leap validation treats
 * year 00 as potentially leap; century-specific validation belongs to caller.
 */
int cai_dtg_parse(const char *text, cai_dtg *out);
/** Format parsed/calendar fields canonically, without changing out on error. */
int cai_dtg_format(const cai_dtg *value, char *out, size_t capacity);
/** Convert an instant to full local/UTC fields without changing process TZ.
 * utc=0 selects local time, utc!=0 UTC. Seconds are omitted, not rounded. */
int cai_dtg_time_parts(time_t when, int utc, cai_dtg *out);
/** Smart display: same calendar month AND year as reference -> DDHHMM
 * locally, DDHHMMZ in UTC. Otherwise DDHHMMZONEMMMYY. */
int cai_dtg_format_time(time_t when, time_t reference, int utc, char *out,
                        size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
