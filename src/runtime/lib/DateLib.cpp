// date_* (behaviour as YoYo's Function_Date): a date is days since 1899-12-30 counted in UTC, so
// date_current_datetime is the same moment everywhere; its fields (year, hour, ...) are read in
// local time or UTC as date_set_timezone says, and date_create_datetime builds from fields the
// same way.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "runtime/Builtins.h"

namespace gm {
namespace {

constexpr double kEpochDays = 25569.0;  // 1970-01-01 as a GameMaker date
bool gUtc = false;                      // date_set_timezone(timezone_utc)

// Days since 1970-01-01 for a proleptic Gregorian date (month 1-12; out of range months and days
// roll over as JavaScript's Date does).
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
    y += (m - 1 >= 0 ? (m - 1) / 12 : -((12 - m) / 12));
    m = ((m - 1) % 12 + 12) % 12 + 1;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool leap(int y) { return y % 400 == 0 || (y % 100 != 0 && y % 4 == 0); }
int monthDays(int y, int m) {  // m 1-12
    static const int len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : len[m - 1];
}

struct Fields {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0, weekday = 4, yday = 1;
};

Fields fieldsOf(double date, bool utc) {
    double secs = (date - kEpochDays) * 86400.0;
    std::time_t t = static_cast<std::time_t>(std::floor(secs + 1e-6));
    std::tm tm{};
#ifdef _WIN32
    utc ? gmtime_s(&tm, &t) : localtime_s(&tm, &t);
#else
    utc ? gmtime_r(&t, &tm) : localtime_r(&t, &tm);
#endif
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday, tm.tm_yday + 1};
}
Fields fieldsOf(double date) { return fieldsOf(date, gUtc); }

// The date for those fields (in the current timezone), plus extra seconds.
double dateOf(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, double s, bool utc) {
    if (utc) {
        double days = static_cast<double>(daysFromCivil(y, mo, d));
        return kEpochDays + days + (h * 3600.0 + mi * 60.0 + s) / 86400.0;
    }
    std::tm tm{};
    tm.tm_year = static_cast<int>(y - 1900);
    tm.tm_mon = static_cast<int>(mo - 1);
    tm.tm_mday = static_cast<int>(d);
    tm.tm_hour = static_cast<int>(h);
    tm.tm_min = static_cast<int>(mi);
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    std::time_t t = std::mktime(&tm);
    return kEpochDays + (static_cast<double>(t) + s) / 86400.0;
}

double nowSeconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() / 1000.0;
}

std::string two(int v) {
    char b[16];
    snprintf(b, sizeof b, "%02d", v);
    return b;
}

}  // namespace

GMFN(date_current_datetime) { return Value(std::floor(nowSeconds()) / 86400.0 + kEpochDays); }
GMFN(date_current_date) { return Value(std::trunc(nowSeconds() / 86400.0) + kEpochDays); }
GMFN(date_current_time) {
    double d = nowSeconds() / 86400.0 + kEpochDays;
    return Value(d - std::trunc(d));
}
GMFN(date_create_datetime) {  // the reference adds 10ms so the fields read back exactly
    return Value(dateOf(I(0), I(1), I(2), I(3), I(4), I(5) + 0.01, gUtc));
}
GMFN(date_valid_datetime) {
    int y = I(0), mo = I(1), d = I(2), h = I(3), mi = I(4), s = I(5);
    bool ok = y >= 1970 && mo >= 1 && mo <= 12 && d >= 1 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && s >= 0 &&
              s <= 59 && d <= monthDays(y, mo);
    return boolV(ok);
}
GMFN(date_get_year) { return Value(static_cast<double>(fieldsOf(D(0)).year)); }
GMFN(date_get_month) { return Value(static_cast<double>(fieldsOf(D(0)).month)); }
GMFN(date_get_day) { return Value(static_cast<double>(fieldsOf(D(0)).day)); }
GMFN(date_get_hour) { return Value(static_cast<double>(fieldsOf(D(0)).hour)); }
GMFN(date_get_minute) { return Value(static_cast<double>(fieldsOf(D(0)).minute)); }
GMFN(date_get_second) { return Value(static_cast<double>(fieldsOf(D(0)).second)); }
GMFN(date_get_weekday) { return Value(static_cast<double>(fieldsOf(D(0)).weekday)); }
GMFN(date_get_day_of_year) { return Value(static_cast<double>(fieldsOf(D(0)).yday)); }
GMFN(date_get_week) { return Value(std::floor(fieldsOf(D(0)).yday / 7.0)); }
GMFN(date_get_hour_of_year) {
    Fields f = fieldsOf(D(0));
    return Value((f.yday - 1) * 24.0 + f.hour);
}
GMFN(date_get_minute_of_year) {
    Fields f = fieldsOf(D(0));
    return Value(((f.yday - 1) * 24.0 + f.hour) * 60 + f.minute);
}
GMFN(date_get_second_of_year) {
    Fields f = fieldsOf(D(0));
    return Value((((f.yday - 1) * 24.0 + f.hour) * 60 + f.minute) * 60 + f.second);
}
// date_inc_year / month change the UTC fields (rolling over like JavaScript dates); the others
// add their length.
static Value incMonths(double date, int64_t months) {
    double secs = (date - kEpochDays) * 86400.0;
    double whole = std::floor(secs);
    Fields f = fieldsOf(date, true);
    double into = whole - std::floor(whole / 86400.0) * 86400.0 + (secs - whole);
    return Value(dateOf(f.year, f.month + months, f.day, 0, 0, into, true));
}
GMFN(date_inc_year) { return incMonths(D(0), static_cast<int64_t>(I(1)) * 12); }
GMFN(date_inc_month) { return incMonths(D(0), I(1)); }
GMFN(date_inc_week) { return Value(D(0) + I(1) * 7.0); }
GMFN(date_inc_day) { return Value(D(0) + I(1)); }
GMFN(date_inc_hour) { return Value(D(0) + I(1) / 24.0); }
GMFN(date_inc_minute) { return Value(D(0) + I(1) / 1440.0); }
GMFN(date_inc_second) { return Value(D(0) + I(1) / 86400.0); }
// Spans: whole seconds apart, in the unit asked (fractional), never negative.
static double spanSeconds(Value* args, int argc) {
    return std::fabs(std::floor((D(1) - D(0)) * 86400.0 + 1e-6));
}
GMFN(date_year_span) { return Value(spanSeconds(args, argc) / 86400.0 / 365.25); }
GMFN(date_month_span) { return Value(spanSeconds(args, argc) / 86400.0 / 30.4375); }
GMFN(date_week_span) { return Value(spanSeconds(args, argc) / 86400.0 / 7); }
GMFN(date_day_span) { return Value(spanSeconds(args, argc) / 86400.0); }
GMFN(date_hour_span) { return Value(spanSeconds(args, argc) / 3600.0); }
GMFN(date_minute_span) { return Value(spanSeconds(args, argc) / 60.0); }
GMFN(date_second_span) { return Value(spanSeconds(args, argc)); }
static double sign(double a, double b) { return a == b ? 0.0 : (a > b ? 1.0 : -1.0); }
GMFN(date_compare_datetime) { return Value(sign(D(0), D(1))); }
GMFN(date_compare_date) {
    Fields a = fieldsOf(D(0)), b = fieldsOf(D(1));
    return Value(sign(a.year * 366.0 + a.month * 31 + a.day, b.year * 366.0 + b.month * 31 + b.day));
}
GMFN(date_compare_time) {
    Fields a = fieldsOf(D(0)), b = fieldsOf(D(1));
    return Value(sign(a.hour * 3600.0 + a.minute * 60 + a.second, b.hour * 3600.0 + b.minute * 60 + b.second));
}
GMFN(date_date_of) {
    Fields f = fieldsOf(D(0));
    return Value(dateOf(f.year, f.month, f.day, 0, 0, 0.5, gUtc));
}
GMFN(date_time_of) { return Value(D(0) - std::trunc(D(0))); }
GMFN(date_datetime_string) {
    Fields f = fieldsOf(D(0));
    return Value(two(f.day) + "/" + two(f.month) + "/" + std::to_string(f.year) + " " + two(f.hour) + ":" +
                 two(f.minute) + ":" + two(f.second));
}
GMFN(date_date_string) {
    Fields f = fieldsOf(D(0));
    return Value(two(f.day) + "/" + two(f.month) + "/" + std::to_string(f.year));
}
GMFN(date_time_string) {
    Fields f = fieldsOf(D(0));
    return Value(two(f.hour) + ":" + two(f.minute) + ":" + two(f.second));
}
GMFN(date_days_in_month) {
    Fields f = fieldsOf(D(0));
    return Value(static_cast<double>(monthDays(f.year, f.month)));
}
GMFN(date_days_in_year) { return Value(leap(fieldsOf(D(0)).year) ? 366.0 : 365.0); }
GMFN(date_leap_year) { return boolV(leap(fieldsOf(D(0)).year)); }
GMFN(date_is_today) {
    Fields a = fieldsOf(D(0)), b = fieldsOf(nowSeconds() / 86400.0 + kEpochDays);
    return boolV(a.year == b.year && a.month == b.month && a.day == b.day);
}
GMFN(date_set_timezone) { gUtc = I(0) == 1; return Value(); }
GMFN(date_get_timezone) { return Value(gUtc ? 1.0 : 0.0); }

void registerDateBuiltins(Runner& rt) {
    GMREG(date_current_datetime); GMREG(date_current_date); GMREG(date_current_time); GMREG(date_create_datetime);
    GMREG(date_valid_datetime); GMREG(date_get_year); GMREG(date_get_month); GMREG(date_get_day);
    GMREG(date_get_hour); GMREG(date_get_minute); GMREG(date_get_second); GMREG(date_get_weekday);
    GMREG(date_get_day_of_year); GMREG(date_get_week); GMREG(date_get_hour_of_year); GMREG(date_get_minute_of_year);
    GMREG(date_get_second_of_year); GMREG(date_inc_year); GMREG(date_inc_month); GMREG(date_inc_week);
    GMREG(date_inc_day); GMREG(date_inc_hour); GMREG(date_inc_minute); GMREG(date_inc_second);
    GMREG(date_year_span); GMREG(date_month_span); GMREG(date_week_span); GMREG(date_day_span);
    GMREG(date_hour_span); GMREG(date_minute_span); GMREG(date_second_span); GMREG(date_compare_datetime);
    GMREG(date_compare_date); GMREG(date_compare_time); GMREG(date_date_of); GMREG(date_time_of);
    GMREG(date_datetime_string); GMREG(date_date_string); GMREG(date_time_string); GMREG(date_days_in_month);
    GMREG(date_days_in_year); GMREG(date_leap_year); GMREG(date_is_today); GMREG(date_set_timezone);
    GMREG(date_get_timezone);
}

}  // namespace gm
