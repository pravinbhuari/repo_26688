import os
import re
from datetime import datetime, timezone, timedelta

def parse_timestamp(timestamp, tzinfo=timezone.utc):
    """Parse a ISO 8601 timestamp string.

    For naive/unaware dt, assume it is in tzinfo timezone (default: UTC).
    """
    dt = datetime.fromisoformat(timestamp)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=tzinfo)
    return dt


def parse_local_timestamp(timestamp, tzinfo=None):
    """Parse a ISO 8601 timestamp string.

    For naive/unaware dt, assume it is in local timezone.
    Convert to tzinfo timezone (the default None means: local timezone).
    """
    dt = datetime.fromisoformat(timestamp)
    if dt.tzinfo is None:
        dt = dt.astimezone(tz=tzinfo)
    return dt


def timestamp(s):
    """Convert a --timestamp=s argument to a datetime object"""
    try:
        # is it pointing to a file / directory?
        ts = safe_s(os.stat(s).st_mtime)
        return datetime.fromtimestamp(ts, tz=timezone.utc)
    except OSError:
        # didn't work, try parsing as a ISO timestamp. if no TZ is given, we assume local timezone.
        return parse_local_timestamp(s)


# Not too rarely, we get crappy timestamps from the fs, that overflow some computations.
# As they are crap anyway (valid filesystem timestamps always refer to the past up to
# the present, but never to the future), nothing is lost if we just clamp them to the
# maximum value we can support.
# As long as people are using borg on 32bit platforms to access borg archives, we must
# keep this value True. But we can expect that we can stop supporting 32bit platforms
# well before coming close to the year 2038, so this will never be a practical problem.
SUPPORT_32BIT_PLATFORMS = True  # set this to False before y2038.

if SUPPORT_32BIT_PLATFORMS:
    # second timestamps will fit into a signed int32 (platform time_t limit).
    # nanosecond timestamps thus will naturally fit into a signed int64.
    # subtract last 48h to avoid any issues that could be caused by tz calculations.
    # this is in the year 2038, so it is also less than y9999 (which is a datetime internal limit).
    # msgpack can pack up to uint64.
    MAX_S = 2**31 - 1 - 48 * 3600
    MAX_NS = MAX_S * 1000000000
else:
    # nanosecond timestamps will fit into a signed int64.
    # subtract last 48h to avoid any issues that could be caused by tz calculations.
    # this is in the year 2262, so it is also less than y9999 (which is a datetime internal limit).
    # round down to 1e9 multiple, so MAX_NS corresponds precisely to a integer MAX_S.
    # msgpack can pack up to uint64.
    MAX_NS = (2**63 - 1 - 48 * 3600 * 1000000000) // 1000000000 * 1000000000
    MAX_S = MAX_NS // 1000000000


def safe_s(ts):
    if 0 <= ts <= MAX_S:
        return ts
    elif ts < 0:
        return 0
    else:
        return MAX_S


def safe_ns(ts):
    if 0 <= ts <= MAX_NS:
        return ts
    elif ts < 0:
        return 0
    else:
        return MAX_NS


def safe_timestamp(item_timestamp_ns):
    t_ns = safe_ns(item_timestamp_ns)
    return datetime.fromtimestamp(t_ns / 1e9, timezone.utc)  # return tz-aware utc datetime obj


def format_time(ts: datetime, format_spec=""):
    """
    Convert *ts* to a human-friendly format with textual weekday.
    """
    return ts.strftime("%a, %Y-%m-%d %H:%M:%S %z" if format_spec == "" else format_spec)


def format_timedelta(td):
    """Format timedelta in a human friendly format"""
    ts = td.total_seconds()
    s = ts % 60
    m = int(ts / 60) % 60
    h = int(ts / 3600) % 24
    txt = "%.2f seconds" % s
    if m:
        txt = "%d minutes %s" % (m, txt)
    if h:
        txt = "%d hours %s" % (h, txt)
    if td.days:
        txt = "%d days %s" % (td.days, txt)
    return txt


def calculate_relative_offset(format_string, from_date, earlier=False):
    """ Calculates offset based on a relative marker. 7d (7 days), 8m (8 months)
        earlier: whether offset should be calculated to an earlier time.
    """
    if from_date is None:
        from_date = archive_ts_now().date()

    if format_string is None:
        return from_date

    day_offset_regex = re.compile(r'\d+d')
    month_offset_regex = re.compile(r'\d+m')

    day_offset_str = day_offset_regex.search(format_string)
    if day_offset_str is not None:
        day_offset = int(day_offset_str.group()[:-1])
        day_offset *= -1 if earlier else 1
        return from_date + timedelta(days=day_offset)

    month_offset_str = month_offset_regex.search(format_string)
    if month_offset_str is not None:
        month_offset = int(month_offset_str.group()[:-1])
        month_offset *= -1 if earlier else 1
        return offset_n_months(from_date, month_offset)

    return from_date


def offset_n_months(from_date, n_months):
    # Calculate target month and year by getting completed total_months until target_month
    total_months = (from_date.year * 12) + from_date.month + n_months
    target_year = (total_months - 1) // 12
    target_month = (total_months % 12) or 12

    # calculate the max days of the target month by subtracting a day from the next month
    next_month_month = ((total_months + 1) % 12) or 12
    next_month_year = (total_months + 1) // 12
    max_days_in_month = (datetime(next_month_year, next_month_month, 1) - timedelta(1)).day

    return datetime(day=min(from_date.day, max_days_in_month), month=target_month, year=target_year).replace(tzinfo=timezone.utc)


class OutputTimestamp:
    def __init__(self, ts: datetime):
        self.ts = ts

    def __format__(self, format_spec):
        return format_time(self.ts, format_spec=format_spec)

    def __str__(self):
        return f"{self}"

    def isoformat(self):
        return self.ts.isoformat(timespec="microseconds")

    to_json = isoformat


def archive_ts_now():
    """return tz-aware datetime obj for current time for usage as archive timestamp"""
    return datetime.now(timezone.utc)  # utc time / utc timezone
