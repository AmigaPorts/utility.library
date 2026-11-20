/* SPDX-License-Identifier: MPL-2.0 */
// Amiga2Date, Date2Amiga and CheckDate: seconds since 1978 and ClockData.

#include "internal/date.h"
#include "internal/math.h"

#include <utility/date.h>

/* ========================================================================
 * Amiga2Date / Date2Amiga / CheckDate -- seconds-since-01-Jan-1978
 * epoch <-> broken-down struct ClockData conversions. (V36)
 *
 * Autodoc FUNCTION/RESULTS text:
 *   Amiga2Date(seconds,result): "Fills in a ClockData structure with
 *     the date and time calculated from a ULONG containing the number
 *     of seconds from 01-Jan-1978 to the date."
 *   Date2Amiga(date): "Calculates the number of seconds from 01-Jan-1978
 *     to the date specified in the ClockData structure." WARNING: "This
 *     function does no sanity checking of the data in the ClockData
 *     structure."
 *   CheckDate(date): "Determines if the ClockData structure contains
 *     legal date information and returns the number of seconds from
 *     01-Jan-1978 to that date, or 0 if the ClockData structure contains
 *     illegal data." BUGS: "The wday field of the ClockData structure is
 *     not checked."
 *
 * struct ClockData field semantics (RKRM Libraries 3rd Ed., Chapter 37,
 * "Date Functions" -- the NDK header itself gives only bare UWORD
 * fields with no range comments): sec/min (0-59), hour (0-23), mday
 * (1-31), month (1-12), year (full year, "1978 -"), wday (0-6, 0 ==
 * Sunday). 01-Jan-1978 itself is a Sunday, so "days since epoch" and
 * "wday" fall out of the same value with no separate epoch-weekday
 * offset needed (day 0 -> wday 0).
 *
 * CheckDate's own BUGS text is the empirical question this work
 * package's task spec flagged for direct oracle verification per
 * the "oracle wins over the docs" rule rather than
 * blind trust -- see provenance/utility-library-datefilter.md's
 * "Oracle observations" for the real-V40 result (confirmed: the real
 * ROM genuinely does NOT check wday either, matching the documented
 * bug verbatim, with no later-version fix evident).
 *
 * IMPLEMENTATION NOTE -- same libgcc-helper-call hang this work
 * package's math-function predecessor found and fixed (see the
 * IMPLEMENTATION NOTE above SMult32/UMult32/SDivMod32/UDivMod32, and
 * provenance/utility-library-math.md's "Errors and fixes" for the full
 * empirical trail): a plain 32x32 `*`/`/`/`%` on this 68000-baseline,
 * -nostartfiles library build reliably reaches a GCC libgcc runtime
 * helper and hangs. Rather than re-derive that the hard way a second
 * time, every multiply/divide/modulo below reuses the already-fixed,
 * already-`-S`-confirmed-call-free static primitives UMult32Core()/
 * UDivMod32Core() (defined earlier in this file) instead of C's plain
 * operators -- re-confirmed via a fresh `-S` inspection of this new
 * region specifically (see provenance/utility-library-datefilter.md),
 * not merely assumed to inherit the earlier fix's safety.
 * ======================================================================== */

#define SECS_PER_MIN  60UL
#define SECS_PER_HOUR 3600UL
#define SECS_PER_DAY  86400UL
#define EPOCH_YEAR    1978UL
#define LAST_YEAR     2114UL  /* 0xFFFFFFFF seconds is 07-Feb-2114 */

static const UWORD DaysInMonthTable[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* umod32 -- remainder-only convenience wrapper over UDivMod32Core(),
 * same "no plain `%`" discipline as the rest of this section. */
static ULONG umod32(ULONG a, ULONG b)
{
    unsigned long long qr = UDivMod32Core(a, b);
    return (ULONG)qr;
}

/* isLeapYear -- the standard Gregorian rule (divisible by 4, except
 * centuries, except again every 4th century) -- year 2000 IS a leap
 * year (divisible by 400), years 1900/2100 are NOT (divisible by 100
 * but not 400). "& 3" for the divide-by-4 test is a plain bitwise AND
 * (a native, single-instruction op, not a disguised division), not a
 * disguised use of plain `%`; the 100/400 tests go through umod32()
 * like everything else in this section. */
static BOOL isLeapYear(ULONG year)
{
    if ((year & 3) != 0)
        return FALSE;
    if (umod32(year, 100) != 0)
        return TRUE;
    return (umod32(year, 400) == 0);
}

/* daysInMonth -- month is 1-based (1-12), matching ClockData's own
 * convention. */
static ULONG daysInMonth(ULONG month1based, ULONG year)
{
    ULONG d = DaysInMonthTable[month1based - 1];

    if (month1based == 2 && isLeapYear(year))
        d = 29;

    return d;
}

VOID Amiga2Date(ULONG seconds, struct ClockData *result)
{
    unsigned long long qr;
    ULONG days, secInDay, remAfterHour;
    ULONG year, month;

    if (!result)
        return;

    qr = UDivMod32Core(seconds, SECS_PER_DAY);
    days = (ULONG)(qr >> 32);
    secInDay = (ULONG)qr;

    qr = UDivMod32Core(secInDay, SECS_PER_HOUR);
    result->hour = (UWORD)(qr >> 32);
    remAfterHour = (ULONG)qr;

    qr = UDivMod32Core(remAfterHour, SECS_PER_MIN);
    result->min = (UWORD)(qr >> 32);
    result->sec = (UWORD)(ULONG)qr;

    /* 01-Jan-1978 (day 0) is itself a Sunday (wday 0), so "days since
     * epoch mod 7" is directly the wday value -- no separate offset. */
    result->wday = (UWORD)umod32(days, 7);

    year = EPOCH_YEAR;
    for (;;) {
        ULONG dinyear = isLeapYear(year) ? 366 : 365;
        if (days < dinyear)
            break;
        days -= dinyear;
        year++;
    }
    result->year = (UWORD)year;

    month = 1;
    for (;;) {
        ULONG dim = daysInMonth(month, year);
        if (days < dim)
            break;
        days -= dim;
        month++;
    }
    result->month = (UWORD)month;
    result->mday = (UWORD)(days + 1);
}

ULONG Date2Amiga(struct ClockData *date)
{
    ULONG year, month;
    ULONG days = 0;
    ULONG daysTimesSecsPerDay, hourTimesSecsPerHour, minTimesSecsPerMin;
    ULONG seconds;

    /* WARNING (autodoc): "This function does no sanity checking of the
     * data in the ClockData structure." -- deliberately no validation
     * here; CheckDate() is the function that validates, implemented
     * separately below (and reusing this function once a date is
     * confirmed legal). */

    for (year = EPOCH_YEAR; year < date->year; year++)
        days += isLeapYear(year) ? 366 : 365;

    for (month = 1; month < date->month; month++)
        days += daysInMonth(month, date->year);

    days += (date->mday - 1);

    daysTimesSecsPerDay = UMult32Core(days, SECS_PER_DAY);
    hourTimesSecsPerHour = UMult32Core(date->hour, SECS_PER_HOUR);
    minTimesSecsPerMin = UMult32Core(date->min, SECS_PER_MIN);

    seconds = daysTimesSecsPerDay + hourTimesSecsPerHour + minTimesSecsPerMin + date->sec;

    return seconds;
}

/* CheckDate -- validates sec/min/hour/mday/month/year (NOT wday, per
 * the autodoc's own documented BUG, empirically reconfirmed against
 * the real V40 oracle rather than merely trusted -- see this section's
 * header comment above). A year before the epoch has no representable
 * non-negative seconds value, so it's treated as invalid along with
 * out-of-range sec/min/hour/mday/month, exactly like every other
 * out-of-range field. Valid -> reuses Date2Amiga() (this file's own
 * reuse discipline) for the returned seconds value, so the two
 * functions can never disagree about what seconds a given date maps
 * to. */
ULONG CheckDate(struct ClockData *date)
{
    if (!date)
        return 0;

    if (date->year < EPOCH_YEAR)
        return 0;
    if (date->month < 1 || date->month > 12)
        return 0;
    if (date->mday < 1 || date->mday > daysInMonth(date->month, date->year))
        return 0;
    if (date->hour > 23)
        return 0;
    if (date->min > 59)
        return 0;
    if (date->sec > 59)
        return 0;

    /* A date past 07-Feb-2114 06:28:15 has no 32-bit seconds value.
     * Date2Amiga() wraps for those (as the original's does), but the
     * original's CheckDate() reports them as invalid -- confirmed
     * against Kickstart 3.1 for 01-Jan-2200 and 08-Feb-2114. Reject
     * far years outright, then let a round trip catch the wrap inside
     * 2114 itself. */
    if (date->year > LAST_YEAR)
        return 0;

    {
        ULONG seconds = Date2Amiga(date);
        struct ClockData check;

        Amiga2Date(seconds, &check);
        if (check.year != date->year || check.month != date->month || check.mday != date->mday)
            return 0;

        return seconds;
    }
}
