// JavaScript runtime objects scripts expect from Node or a browser:
// Date, URL / URLSearchParams, window.location, and the URI encoders.
//
// A Date is a dict {"__date_ms": <epoch ms>}; a URL is a dict of its parts
// plus a "searchParams" dict {"__usp_pairs": [[k, v], ...]}. Their methods
// are dispatched from VM::callDictMethod through callJsObjectMethod, so the
// objects stay plain data (printing, copying and member reads just work).
#include "Vm.h"
#include "Error.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
const char *DATE_KEY = "__date_ms";
const char *URL_KEY = "__url";
const char *USP_KEY = "__usp_pairs";
const double NaN = std::numeric_limits<double>::quiet_NaN();

const char *kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char *kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                         "August", "September", "October", "November", "December"};

double realNowMs()
{
    using namespace std::chrono;
    return static_cast<double>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// Broken-down time of `ms` in local time or UTC. False for an invalid date.
bool breakDown(double ms, bool utc, std::tm &out, int &millis)
{
    if (!std::isfinite(ms))
        return false;
    double secs = std::floor(ms / 1000.0);
    millis = static_cast<int>(ms - secs * 1000.0);
    std::time_t t = static_cast<std::time_t>(secs);
#ifdef _WIN32
    return (utc ? gmtime_s(&out, &t) : localtime_s(&out, &t)) == 0;
#else
    return (utc ? gmtime_r(&t, &out) : localtime_r(&t, &out)) != nullptr;
#endif
}

// Epoch ms from calendar fields; out-of-range fields roll over as in JS
// (month 12 is next January, day 0 the last day of the previous month).
double compose(double y, double mon, double d, double h, double mi, double s, double ms, bool utc)
{
    for (double v : {y, mon, d, h, mi, s, ms})
        if (!std::isfinite(v))
            return NaN;
    std::tm t{};
    t.tm_year = static_cast<int>(y) - 1900;
    t.tm_mon = static_cast<int>(mon);
    t.tm_mday = static_cast<int>(d);
    t.tm_hour = static_cast<int>(h);
    t.tm_min = static_cast<int>(mi);
    t.tm_sec = static_cast<int>(s);
    t.tm_isdst = -1;
#ifdef _WIN32
    std::time_t secs = utc ? _mkgmtime(&t) : std::mktime(&t);
#else
    std::time_t secs = utc ? timegm(&t) : std::mktime(&t);
#endif
    if (secs == static_cast<std::time_t>(-1))
        return NaN;
    return static_cast<double>(secs) * 1000.0 + ms;
}

int monthIndex(const std::string &name)
{
    std::string n = name.substr(0, 3);
    for (auto &c : n)
        c = (char)std::tolower((unsigned char)c);
    static const char *abbr[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int i = 0; i < 12; ++i)
        if (n == abbr[i])
            return i;
    return -1;
}

// Date.parse: ISO 8601 (date-only is UTC, date-time without zone is local),
// "YYYY/MM/DD", "MM/DD/YYYY", and "Month D, YYYY [hh:mm[:ss]]".
double parseDate(const std::string &text)
{
    std::smatch m;
    std::string s = text;
    static const std::regex isoDate(R"(^\s*(\d{4})-(\d{2})-(\d{2})\s*$)");
    static const std::regex isoDateTime(
        R"(^\s*(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})(?::(\d{2})(?:\.(\d{1,3})\d*)?)?\s*(Z|[+-]\d{2}:?\d{2})?\s*$)");
    static const std::regex slashYmd(
        R"(^\s*(\d{4})/(\d{1,2})/(\d{1,2})(?:\s+(\d{1,2}):(\d{2})(?::(\d{2}))?)?\s*$)");
    static const std::regex slashMdy(
        R"(^\s*(\d{1,2})/(\d{1,2})/(\d{4})(?:\s+(\d{1,2}):(\d{2})(?::(\d{2}))?)?\s*$)");
    static const std::regex monthDy(
        R"(^\s*(?:[A-Za-z]+,?\s+)?([A-Za-z]+)\.?\s+(\d{1,2}),?\s+(\d{4})(?:\s+(\d{1,2}):(\d{2})(?::(\d{2}))?)?\s*$)");
    static const std::regex dayMonthY(
        R"(^\s*(\d{1,2})\s+([A-Za-z]+)\.?,?\s+(\d{4})(?:\s+(\d{1,2}):(\d{2})(?::(\d{2}))?)?\s*$)");
    auto num = [&](size_t i, double def = 0.0)
    { return m[i].matched && m[i].length() ? std::stod(m[i].str()) : def; };

    if (std::regex_match(s, m, isoDate))
        return compose(num(1), num(2) - 1, num(3), 0, 0, 0, 0, true);
    if (std::regex_match(s, m, isoDateTime))
    {
        double ms = 0;
        if (m[7].matched)
        {
            std::string frac = m[7].str();
            while (frac.size() < 3)
                frac += '0';
            ms = std::stod(frac);
        }
        if (!m[8].matched)
            return compose(num(1), num(2) - 1, num(3), num(4), num(5), num(6), ms, false);
        double t = compose(num(1), num(2) - 1, num(3), num(4), num(5), num(6), ms, true);
        std::string zone = m[8].str();
        if (zone != "Z")
        {
            int sign = zone[0] == '-' ? -1 : 1;
            std::string digits;
            for (char c : zone)
                if (std::isdigit((unsigned char)c))
                    digits += c;
            double offMin = std::stod(digits.substr(0, 2)) * 60 + std::stod(digits.substr(2, 2));
            t -= sign * offMin * 60000.0;
        }
        return t;
    }
    if (std::regex_match(s, m, slashYmd))
        return compose(num(1), num(2) - 1, num(3), num(4), num(5), num(6), 0, false);
    if (std::regex_match(s, m, slashMdy))
        return compose(num(3), num(1) - 1, num(2), num(4), num(5), num(6), 0, false);
    if (std::regex_match(s, m, monthDy) && monthIndex(m[1].str()) >= 0)
        return compose(num(3), monthIndex(m[1].str()), num(2), num(4), num(5), num(6), 0, false);
    if (std::regex_match(s, m, dayMonthY) && monthIndex(m[2].str()) >= 0)
        return compose(num(3), monthIndex(m[2].str()), num(1), num(4), num(5), num(6), 0, false);
    return NaN;
}

std::string pad(int v, int width = 2)
{
    std::ostringstream o;
    o << std::setw(width) << std::setfill('0') << v;
    return o.str();
}

// "+0500" style offset of local time from UTC at `ms`.
std::string zoneOffset(double ms)
{
    std::tm loc{}, utc{};
    int millis;
    if (!breakDown(ms, false, loc, millis) || !breakDown(ms, true, utc, millis))
        return "+0000";
    double diffMin = (compose(loc.tm_year + 1900, loc.tm_mon, loc.tm_mday, loc.tm_hour,
                              loc.tm_min, loc.tm_sec, 0, true) -
                      std::floor(ms / 1000.0) * 1000.0) /
                     60000.0;
    int off = static_cast<int>(std::lround(diffMin));
    return std::string(off < 0 ? "-" : "+") + pad(std::abs(off) / 60) + pad(std::abs(off) % 60);
}

std::string isoString(double ms)
{
    std::tm t{};
    int millis;
    breakDown(ms, true, t, millis);
    return std::to_string(t.tm_year + 1900) + "-" + pad(t.tm_mon + 1) + "-" + pad(t.tm_mday) +
           "T" + pad(t.tm_hour) + ":" + pad(t.tm_min) + ":" + pad(t.tm_sec) + "." +
           pad(millis, 3) + "Z";
}

std::string dateString(const std::tm &t)
{
    return std::string(kDays[t.tm_wday]).substr(0, 3) + " " +
           std::string(kMonths[t.tm_mon]).substr(0, 3) + " " + pad(t.tm_mday) + " " +
           std::to_string(t.tm_year + 1900);
}

std::string optionOf(const QuantumValue &opts, const char *key)
{
    if (!opts.isDict())
        return "";
    auto it = opts.asDict()->find(key);
    return it == opts.asDict()->end() || it->second.isNil() ? "" : it->second.toString();
}

// Intl.DateTimeFormat for en-US — what toLocaleDateString / TimeString /
// String produce. `defaults`: "date", "time" or "all" fields when the
// options name none of that kind.
std::string localeFormat(double ms, const QuantumValue &opts, const std::string &defaults)
{
    std::tm t{};
    int millis;
    if (!breakDown(ms, false, t, millis))
        return "Invalid Date";
    std::string weekday = optionOf(opts, "weekday"), year = optionOf(opts, "year"),
                month = optionOf(opts, "month"), day = optionOf(opts, "day"),
                hour = optionOf(opts, "hour"), minute = optionOf(opts, "minute"),
                second = optionOf(opts, "second"), hour12 = optionOf(opts, "hour12");
    bool anyDate = !weekday.empty() || !year.empty() || !month.empty() || !day.empty();
    bool anyTime = !hour.empty() || !minute.empty() || !second.empty();
    if ((defaults == "date" || defaults == "all") && !anyDate && !(defaults == "all" && anyTime))
        year = month = day = "numeric";
    if ((defaults == "time" || defaults == "all") && !anyTime && !(defaults == "all" && anyDate))
        hour = minute = second = "numeric";

    auto text = [](const std::string &style, const char *full)
    {
        std::string f = full;
        return style == "long" ? f : style == "narrow" ? f.substr(0, 1) : f.substr(0, 3);
    };
    auto numeric = [&](const std::string &style, int v)
    { return style == "2-digit" ? pad(v % 100) : std::to_string(v); };

    std::string datePart;
    bool textMonth = month == "long" || month == "short" || month == "narrow";
    if (textMonth)
    {
        datePart = text(month, kMonths[t.tm_mon]);
        if (!day.empty())
            datePart += " " + numeric(day, t.tm_mday);
        if (!year.empty())
            datePart += (day.empty() ? " " : ", ") + numeric(year, t.tm_year + 1900);
    }
    else
    {
        std::vector<std::string> parts;
        if (!month.empty())
            parts.push_back(numeric(month, t.tm_mon + 1));
        if (!day.empty())
            parts.push_back(numeric(day, t.tm_mday));
        if (!year.empty())
            parts.push_back(numeric(year, t.tm_year + 1900));
        for (size_t i = 0; i < parts.size(); ++i)
            datePart += (i ? "/" : "") + parts[i];
    }
    if (!weekday.empty())
        datePart = text(weekday, kDays[t.tm_wday]) + (datePart.empty() ? "" : ", " + datePart);

    std::string timePart;
    if (!hour.empty() || !minute.empty() || !second.empty())
    {
        bool use12 = hour12 != "false";
        int h = t.tm_hour;
        if (use12)
            h = h % 12 == 0 ? 12 : h % 12;
        if (!hour.empty())
            timePart = hour == "2-digit" ? pad(h) : std::to_string(h);
        if (!minute.empty())
            timePart += (timePart.empty() ? "" : ":") + pad(t.tm_min);
        if (!second.empty())
            timePart += (timePart.empty() ? "" : ":") + pad(t.tm_sec);
        if (use12 && !hour.empty())
            timePart += t.tm_hour < 12 ? " AM" : " PM";
    }
    if (datePart.empty())
        return timePart;
    return timePart.empty() ? datePart : datePart + ", " + timePart;
}

double argNum(const std::vector<QuantumValue> &a, size_t i, double def)
{
    if (i >= a.size())
        return def;
    if (a[i].isNumber())
        return a[i].asNumber();
    if (a[i].isString())
    {
        try
        {
            return std::stod(a[i].asString());
        }
        catch (...)
        {
            return NaN;
        }
    }
    return NaN;
}

QuantumValue makeDate(double ms)
{
    auto d = std::make_shared<Dict>();
    (*d)[DATE_KEY] = QuantumValue(ms);
    return QuantumValue(d);
}

// ── URL ─────────────────────────────────────────────────────────────────────

std::string percentEncode(const std::string &s, const std::string &keep)
{
    std::ostringstream o;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || keep.find((char)c) != std::string::npos)
            o << c;
        else
            o << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c
              << std::nouppercase << std::dec;
    }
    return o.str();
}

std::string percentDecode(const std::string &s, bool plusIsSpace)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2]))
        {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else if (plusIsSpace && s[i] == '+')
            out += ' ';
        else
            out += s[i];
    }
    return out;
}

// application/x-www-form-urlencoded, as URLSearchParams serializes.
std::string formEncode(const std::string &s)
{
    std::string enc = percentEncode(s, "*-._ ");
    for (auto &c : enc)
        if (c == ' ')
            c = '+';
    return enc;
}

std::shared_ptr<Array> uspPairs(const std::shared_ptr<Dict> &usp)
{
    auto it = usp->find(USP_KEY);
    if (it == usp->end() || !it->second.isArray())
        (*usp)[USP_KEY] = QuantumValue(std::make_shared<Array>());
    return (*usp)[USP_KEY].asArray();
}

std::string uspString(const std::shared_ptr<Dict> &usp)
{
    std::string out;
    for (auto &p : *uspPairs(usp))
    {
        const Array &kv = *p.asArray();
        out += (out.empty() ? "" : "&") + formEncode(kv[0].toString()) + "=" + formEncode(kv[1].toString());
    }
    return out;
}

QuantumValue makeSearchParams(const std::string &query)
{
    auto usp = std::make_shared<Dict>();
    auto pairs = uspPairs(usp);
    std::string q = (!query.empty() && query[0] == '?') ? query.substr(1) : query;
    size_t start = 0;
    while (start <= q.size() && !q.empty())
    {
        size_t amp = q.find('&', start);
        std::string part = q.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!part.empty())
        {
            size_t eq = part.find('=');
            auto kv = std::make_shared<Array>();
            kv->push_back(QuantumValue(percentDecode(part.substr(0, eq), true)));
            kv->push_back(QuantumValue(eq == std::string::npos ? std::string()
                                                               : percentDecode(part.substr(eq + 1), true)));
            pairs->push_back(QuantumValue(kv));
        }
        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }
    return QuantumValue(usp);
}

// Recomputes a URL's derived fields (search, host, origin, href) from its
// parts and current searchParams.
void syncUrl(const std::shared_ptr<Dict> &url)
{
    auto get = [&](const char *k)
    {
        auto it = url->find(k);
        return it == url->end() ? std::string() : it->second.toString();
    };
    auto sp = url->find("searchParams");
    if (sp != url->end() && sp->second.isDict())
    {
        std::string q = uspString(sp->second.asDict());
        (*url)["search"] = QuantumValue(q.empty() ? std::string() : "?" + q);
    }
    std::string host = get("hostname") + (get("port").empty() ? "" : ":" + get("port"));
    (*url)["host"] = QuantumValue(host);
    (*url)["origin"] = QuantumValue(get("protocol") + "//" + host);
    std::string auth = get("username").empty()
                           ? ""
                           : get("username") + (get("password").empty() ? "" : ":" + get("password")) + "@";
    (*url)["href"] = QuantumValue(get("protocol") + "//" + auth + host + get("pathname") +
                                  get("search") + get("hash"));
}

// Parses an absolute URL (optionally relative to `base`); nullptr if invalid.
std::shared_ptr<Dict> parseUrl(const std::string &input, const std::string &base)
{
    static const std::regex abs(
        R"(^([A-Za-z][A-Za-z0-9+.-]*:)//(?:([^:@/]*)(?::([^@/]*))?@)?([^:/?#]*)(?::(\d+))?([^?#]*)(\?[^#]*)?(#.*)?$)");
    std::smatch m;
    std::string s = input;
    if (!std::regex_match(s, m, abs))
    {
        if (base.empty())
            return nullptr;
        auto b = parseUrl(base, "");
        if (!b)
            return nullptr;
        std::string origin = (*b)["protocol"].toString() + "//" + (*b)["host"].toString();
        std::string path = (*b)["pathname"].toString();
        if (!s.empty() && s[0] == '/')
            return parseUrl(origin + s, "");
        if (!s.empty() && (s[0] == '?' || s[0] == '#'))
            return parseUrl(origin + path + s, "");
        return parseUrl(origin + path.substr(0, path.rfind('/') + 1) + s, "");
    }
    if (m[4].str().empty())
        return nullptr;
    auto url = std::make_shared<Dict>();
    (*url)[URL_KEY] = QuantumValue(true);
    std::string protocol = m[1].str();
    for (auto &c : protocol)
        c = (char)std::tolower((unsigned char)c);
    (*url)["protocol"] = QuantumValue(protocol);
    (*url)["username"] = QuantumValue(m[2].str());
    (*url)["password"] = QuantumValue(m[3].str());
    std::string hostname = m[4].str();
    for (auto &c : hostname)
        c = (char)std::tolower((unsigned char)c);
    (*url)["hostname"] = QuantumValue(hostname);
    std::string port = m[5].str();
    if ((protocol == "http:" && port == "80") || (protocol == "https:" && port == "443"))
        port.clear();
    (*url)["port"] = QuantumValue(port);
    (*url)["pathname"] = QuantumValue(m[6].str().empty() ? std::string("/") : m[6].str());
    (*url)["hash"] = QuantumValue(m[8].str() == "#" ? std::string() : m[8].str());
    (*url)["searchParams"] = makeSearchParams(m[7].str());
    syncUrl(url);
    return url;
}
} // namespace

bool isJsDate(const QuantumValue &v)
{
    return v.isDict() && v.asDict()->count(DATE_KEY);
}

double jsDateMs(const QuantumValue &v)
{
    const QuantumValue &ms = v.asDict()->at(DATE_KEY);
    return ms.isNumber() ? ms.asNumber() : NaN;
}

std::string jsDateToString(double ms)
{
    std::tm t{};
    int millis;
    if (!breakDown(ms, false, t, millis))
        return "Invalid Date";
    return dateString(t) + " " + pad(t.tm_hour) + ":" + pad(t.tm_min) + ":" + pad(t.tm_sec) +
           " GMT" + zoneOffset(ms);
}

std::string jsDisplayString(const std::shared_ptr<Dict> &d, bool &handled)
{
    handled = true;
    if (d->count(DATE_KEY))
        return jsDateToString(jsDateMs(QuantumValue(d)));
    if (d->count(URL_KEY))
        return (*d)["href"].toString();
    if (d->count(USP_KEY))
        return uspString(d);
    handled = false;
    return "";
}

QuantumValue makeJsDateModule(std::shared_ptr<double> simulatedMs)
{
    auto module = std::make_shared<Dict>();
    auto nat = [&](const std::string &name, QuantumNativeFunc fn)
    {
        auto n = std::make_shared<QuantumNative>();
        n->name = "Date." + name;
        n->fn = std::move(fn);
        (*module)[name] = QuantumValue(n);
    };
    // setTimeout runs callbacks immediately and advances this simulated
    // offset, so time measured across a timer still reads as elapsed.
    auto now = [simulatedMs]()
    { return realNowMs() + *simulatedMs; };
    nat("now", [now](std::vector<QuantumValue>) -> QuantumValue
        { return QuantumValue(now()); });
    nat("parse", [](std::vector<QuantumValue> a) -> QuantumValue
        { return QuantumValue(a.empty() ? NaN : parseDate(a[0].toString())); });
    nat("UTC", [](std::vector<QuantumValue> a) -> QuantumValue
        {
        return QuantumValue(compose(argNum(a, 0, NaN), argNum(a, 1, 0), argNum(a, 2, 1),
                                    argNum(a, 3, 0), argNum(a, 4, 0), argNum(a, 5, 0),
                                    argNum(a, 6, 0), true)); });
    // new Date() / new Date(ms) / new Date(string) / new Date(date) /
    // new Date(y, m[, d, h, mi, s, ms]) — the last in local time.
    nat("__new__", [now](std::vector<QuantumValue> a) -> QuantumValue
        {
        if (a.empty()) return makeDate(now());
        if (a.size() == 1) {
            if (isJsDate(a[0])) return makeDate(jsDateMs(a[0]));
            if (a[0].isNumber()) return makeDate(a[0].asNumber());
            if (a[0].isString()) return makeDate(parseDate(a[0].asString()));
            return makeDate(NaN);
        }
        return makeDate(compose(argNum(a, 0, NaN), argNum(a, 1, 0), argNum(a, 2, 1),
                                argNum(a, 3, 0), argNum(a, 4, 0), argNum(a, 5, 0),
                                argNum(a, 6, 0), false)); });
    return QuantumValue(module);
}

bool callJsObjectMethod(const std::shared_ptr<Dict> &d, const std::string &m,
                        const std::vector<QuantumValue> &a, QuantumValue &out)
{
    // ── Date ──────────────────────────────────────────────────────────────
    if (d->count(DATE_KEY))
    {
        double ms = jsDateMs(QuantumValue(d));
        bool utc = m.find("UTC") != std::string::npos;
        std::tm t{};
        int millis = 0;
        bool valid = breakDown(ms, utc, t, millis);
        auto field = [&](double v)
        { out = QuantumValue(valid ? v : NaN); return true; };
        auto store = [&](double v)
        {
            (*d)[DATE_KEY] = QuantumValue(v);
            out = QuantumValue(v);
            return true;
        };
        auto fields = [&](double y, double mo, double day, double h, double mi, double s, double msec)
        { return store(compose(y, mo, day, h, mi, s, msec, utc)); };
        const double Y = t.tm_year + 1900, M = t.tm_mon, D = t.tm_mday, H = t.tm_hour,
                     MI = t.tm_min, S = t.tm_sec, MS = millis;

        if (m == "getTime" || m == "valueOf")
            return field(ms);
        if (m == "getFullYear" || m == "getUTCFullYear")
            return field(Y);
        if (m == "getYear")
            return field(Y - 1900);
        if (m == "getMonth" || m == "getUTCMonth")
            return field(M);
        if (m == "getDate" || m == "getUTCDate")
            return field(D);
        if (m == "getDay" || m == "getUTCDay")
            return field(t.tm_wday);
        if (m == "getHours" || m == "getUTCHours")
            return field(H);
        if (m == "getMinutes" || m == "getUTCMinutes")
            return field(MI);
        if (m == "getSeconds" || m == "getUTCSeconds")
            return field(S);
        if (m == "getMilliseconds" || m == "getUTCMilliseconds")
            return field(MS);
        if (m == "getTimezoneOffset")
        {
            std::string z = zoneOffset(ms);
            double off = std::stod(z.substr(1, 2)) * 60 + std::stod(z.substr(3, 2));
            return field(z[0] == '+' ? -off : off);
        }
        if (m == "setTime")
            return store(argNum(a, 0, NaN));
        if (m == "setFullYear" || m == "setUTCFullYear")
            return fields(argNum(a, 0, NaN), argNum(a, 1, M), argNum(a, 2, D), H, MI, S, MS);
        if (m == "setMonth" || m == "setUTCMonth")
            return fields(Y, argNum(a, 0, NaN), argNum(a, 1, D), H, MI, S, MS);
        if (m == "setDate" || m == "setUTCDate")
            return fields(Y, M, argNum(a, 0, NaN), H, MI, S, MS);
        if (m == "setHours" || m == "setUTCHours")
            return fields(Y, M, D, argNum(a, 0, NaN), argNum(a, 1, MI), argNum(a, 2, S), argNum(a, 3, MS));
        if (m == "setMinutes" || m == "setUTCMinutes")
            return fields(Y, M, D, H, argNum(a, 0, NaN), argNum(a, 1, S), argNum(a, 2, MS));
        if (m == "setSeconds" || m == "setUTCSeconds")
            return fields(Y, M, D, H, MI, argNum(a, 0, NaN), argNum(a, 1, MS));
        if (m == "setMilliseconds" || m == "setUTCMilliseconds")
            return fields(Y, M, D, H, MI, S, argNum(a, 0, NaN));
        if (m == "toISOString" || m == "toJSON")
        {
            if (!valid)
                throw RuntimeError("RangeError: Invalid time value");
            out = QuantumValue(isoString(ms));
            return true;
        }
        if (m == "toString")
        {
            out = QuantumValue(jsDateToString(ms));
            return true;
        }
        if (m == "toDateString")
        {
            out = QuantumValue(valid ? dateString(t) : "Invalid Date");
            return true;
        }
        if (m == "toTimeString")
        {
            out = QuantumValue(valid ? pad(t.tm_hour) + ":" + pad(t.tm_min) + ":" + pad(t.tm_sec) +
                                           " GMT" + zoneOffset(ms)
                                     : "Invalid Date");
            return true;
        }
        if (m == "toUTCString" || m == "toGMTString")
        {
            std::tm u{};
            out = QuantumValue(breakDown(ms, true, u, millis)
                                   ? std::string(kDays[u.tm_wday]).substr(0, 3) + ", " + pad(u.tm_mday) +
                                         " " + std::string(kMonths[u.tm_mon]).substr(0, 3) + " " +
                                         std::to_string(u.tm_year + 1900) + " " + pad(u.tm_hour) +
                                         ":" + pad(u.tm_min) + ":" + pad(u.tm_sec) + " GMT"
                                   : "Invalid Date");
            return true;
        }
        if (m == "toLocaleDateString" || m == "toLocaleTimeString" || m == "toLocaleString")
        {
            QuantumValue opts = a.size() > 1 ? a[1] : QuantumValue();
            std::string defaults = m == "toLocaleDateString"   ? "date"
                                   : m == "toLocaleTimeString" ? "time"
                                                               : "all";
            out = QuantumValue(localeFormat(ms, opts, defaults));
            return true;
        }
        return false;
    }

    // ── URLSearchParams ───────────────────────────────────────────────────
    if (d->count(USP_KEY))
    {
        auto pairs = uspPairs(d);
        std::string key = a.empty() ? "" : a[0].toString();
        auto find = [&]()
        {
            for (size_t i = 0; i < pairs->size(); ++i)
                if ((*(*pairs)[i].asArray())[0].toString() == key)
                    return (long)i;
            return -1L;
        };
        auto owner = d->find("__owner_sync");
        auto changed = [&]()
        {
            if (owner != d->end() && owner->second.isNative())
                owner->second.asNative()->fn({});
        };
        if (m == "get")
        {
            long i = find();
            out = i < 0 ? QuantumValue() : (*(*pairs)[i].asArray())[1];
            return true;
        }
        if (m == "getAll")
        {
            auto all = std::make_shared<Array>();
            for (auto &p : *pairs)
                if ((*p.asArray())[0].toString() == key)
                    all->push_back((*p.asArray())[1]);
            out = QuantumValue(all);
            return true;
        }
        if (m == "has")
        {
            out = QuantumValue(find() >= 0);
            return true;
        }
        if (m == "set" || m == "append")
        {
            std::string value = a.size() > 1 ? a[1].toString() : "undefined";
            long i = m == "set" ? find() : -1;
            if (i >= 0)
            {
                (*(*pairs)[i].asArray())[1] = QuantumValue(value);
                // `set` keeps the first entry and drops later duplicates
                for (size_t j = pairs->size(); j-- > (size_t)i + 1;)
                    if ((*(*pairs)[j].asArray())[0].toString() == key)
                        pairs->erase(pairs->begin() + j);
            }
            else
            {
                auto kv = std::make_shared<Array>();
                kv->push_back(QuantumValue(key));
                kv->push_back(QuantumValue(value));
                pairs->push_back(QuantumValue(kv));
            }
            changed();
            out = QuantumValue();
            return true;
        }
        if (m == "delete")
        {
            for (size_t j = pairs->size(); j-- > 0;)
                if ((*(*pairs)[j].asArray())[0].toString() == key)
                    pairs->erase(pairs->begin() + j);
            changed();
            out = QuantumValue();
            return true;
        }
        if (m == "toString")
        {
            out = QuantumValue(uspString(d));
            return true;
        }
        if (m == "entries" || m == "keys" || m == "values")
        {
            auto list = std::make_shared<Array>();
            for (auto &p : *pairs)
                list->push_back(m == "entries" ? QuantumValue(std::make_shared<Array>(*p.asArray()))
                                               : (*p.asArray())[m == "keys" ? 0 : 1]);
            out = QuantumValue(list);
            return true;
        }
        return false;
    }

    // ── URL ───────────────────────────────────────────────────────────────
    if (d->count(URL_KEY) && (m == "toString" || m == "toJSON"))
    {
        out = (*d)["href"];
        return true;
    }
    return false;
}

void VM::registerJsRuntimeNatives()
{
    auto reg = [&](const std::string &name, QuantumValue value)
    {
        globals->define(name, std::move(value));
        weakGlobals_.insert(name); // yields to a user's own `location`, `URL`, ...
    };
    auto native = [](const std::string &name, QuantumNativeFunc fn)
    {
        auto n = std::make_shared<QuantumNative>();
        n->name = name;
        n->fn = std::move(fn);
        return QuantumValue(n);
    };

    // new URL(url[, base]) — throws TypeError("Invalid URL") like the real one.
    auto urlClass = std::make_shared<Dict>();
    (*urlClass)["__new__"] = native("URL", [](std::vector<QuantumValue> a) -> QuantumValue
                                    {
        auto url = parseUrl(a.empty() ? "" : a[0].toString(), a.size() > 1 ? a[1].toString() : "");
        if (!url)
            throw TypeError("Invalid URL: " + (a.empty() ? std::string() : a[0].toString()));
        // searchParams edits keep the URL's search/href current. A native
        // holding a weak reference, so the two objects form no cycle.
        std::weak_ptr<Dict> weakUrl = url;
        auto sync = std::make_shared<QuantumNative>();
        sync->name = "URL.__sync";
        sync->fn = [weakUrl](std::vector<QuantumValue>) -> QuantumValue {
            if (auto u = weakUrl.lock()) syncUrl(u);
            return QuantumValue();
        };
        (*(*url)["searchParams"].asDict())["__owner_sync"] = QuantumValue(sync);
        return QuantumValue(url); });
    reg("URL", QuantumValue(urlClass));

    // new URLSearchParams("a=1&b=2" | {a: 1} | [["a", 1]])
    auto uspClass = std::make_shared<Dict>();
    (*uspClass)["__new__"] = native("URLSearchParams", [](std::vector<QuantumValue> a) -> QuantumValue
                                    {
        if (a.empty() || a[0].isNil()) return makeSearchParams("");
        if (a[0].isString()) return makeSearchParams(a[0].asString());
        QuantumValue usp = makeSearchParams("");
        auto pairs = uspPairs(usp.asDict());
        auto add = [&](const QuantumValue &k, const QuantumValue &v) {
            auto kv = std::make_shared<Array>();
            kv->push_back(QuantumValue(k.toString()));
            kv->push_back(QuantumValue(v.toString()));
            pairs->push_back(QuantumValue(kv));
        };
        if (a[0].isDict())
            for (auto &[k, v] : *a[0].asDict()) add(QuantumValue(k), v);
        else if (a[0].isArray())
            for (auto &p : *a[0].asArray())
                if (p.isArray() && p.asArray()->size() >= 2) add((*p.asArray())[0], (*p.asArray())[1]);
        return usp; });
    reg("URLSearchParams", QuantumValue(uspClass));

    reg("encodeURIComponent", native("encodeURIComponent", [](std::vector<QuantumValue> a) -> QuantumValue
                                     { return QuantumValue(percentEncode(a.empty() ? "undefined" : a[0].toString(), "-_.!~*'()")); }));
    reg("encodeURI", native("encodeURI", [](std::vector<QuantumValue> a) -> QuantumValue
                            { return QuantumValue(percentEncode(a.empty() ? "undefined" : a[0].toString(), "-_.!~*'();/?:@&=+$,#")); }));
    for (const char *name : {"decodeURIComponent", "decodeURI"})
        reg(name, native(name, [](std::vector<QuantumValue> a) -> QuantumValue
                         { return QuantumValue(percentDecode(a.empty() ? "undefined" : a[0].toString(), false)); }));

    // A browser-like `window` for scripts written against the DOM: the page
    // is http://localhost/ with no query string.
    auto location = std::make_shared<Dict>();
    for (auto &[k, v] : std::vector<std::pair<std::string, std::string>>{
             {"href", "http://localhost/"}, {"protocol", "http:"}, {"host", "localhost"},
             {"hostname", "localhost"}, {"port", ""}, {"pathname", "/"}, {"search", ""},
             {"hash", ""}, {"origin", "http://localhost"}})
        (*location)[k] = QuantumValue(v);
    for (const char *noop : {"reload", "assign", "replace"})
        (*location)[noop] = native(std::string("location.") + noop,
                                   [](std::vector<QuantumValue>) -> QuantumValue { return QuantumValue(); });
    auto navigator = std::make_shared<Dict>();
    (*navigator)["userAgent"] = QuantumValue(std::string("Mozilla/5.0 (Quantum)"));
    (*navigator)["language"] = QuantumValue(std::string("en-US"));
    (*navigator)["onLine"] = QuantumValue(true);

    auto window = std::make_shared<Dict>();
    (*window)["location"] = QuantumValue(location);
    (*window)["navigator"] = QuantumValue(navigator);
    (*window)["innerWidth"] = QuantumValue(1024.0);
    (*window)["innerHeight"] = QuantumValue(768.0);
    for (const char *shared : {"document", "console", "localStorage", "sessionStorage", "alert",
                               "setTimeout", "clearTimeout", "setInterval", "clearInterval",
                               "fetch", "Date", "Math", "JSON", "URL", "URLSearchParams"})
        if (globals->has(shared))
            (*window)[shared] = globals->get(shared);
    reg("location", QuantumValue(location));
    reg("navigator", QuantumValue(navigator));
    reg("window", QuantumValue(window));
}
