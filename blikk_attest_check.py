#!/usr/bin/env python3
"""blikk-attest-check

Logs in to the Blikk REST API (https://publicapidocs.blikk.com) and checks
the attestation ("attest") flags of the current month's time reports for a
single Blikk user - the person who owns the configured API credentials.
Meant to be called as a step in an automation flow (Power Automate, a
scheduled task, Google Apps Script shelling out to it, ...): it prints a
machine-readable JSON summary to stdout and signals the result via its exit
code.

Blikk's public API authenticates an *application* (an id/secret pair), not a
person - there is no "who am I" endpoint to derive a user from the token. So
this script requires a Blikk user id up front (BLIKK_USER_ID / --user-id)
and only ever queries that one user's time reports; it refuses to run
without it rather than silently falling back to every user the application
credentials can see.

Pure standard library - no pip install needed, just python3 (3.8+).

Exit codes:
    0  OK      - request succeeded, every time report in the period is attested
    1  PENDING - request succeeded, but some reports are not (yet) attested
                 (or no time reports at all were found for the period)
    2  USAGE   - bad arguments / missing configuration
    3  ERROR   - authentication or API request failed

See README.md for configuration and usage examples.
"""

from __future__ import annotations

import argparse
import base64
import calendar
import datetime
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

DEFAULT_BASE_URL = "https://publicapi.blikk.com"

PAGE_SIZE = 100
MAX_PAGES = 500  # safety cap: 50000 reports is far beyond one month
MAX_429_RETRIES = 5
DEFAULT_RETRY_AFTER_SECONDS = 2
HTTP_TIMEOUT_SECONDS = 30

EXIT_OK = 0
EXIT_PENDING = 1
EXIT_USAGE = 2
EXIT_ERROR = 3


class BlikkApiError(Exception):
    """Raised for any authentication or API-level failure."""


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

def _http_request(url: str, method: str, headers: Dict[str, str]) -> Tuple[int, str, Dict[str, str]]:
    """Performs one HTTP request. Returns (status_code, body, response_headers).

    Deliberately does not raise on non-2xx status codes (the caller decides
    what to do with e.g. 401/429) - only network-level failures raise.
    """
    request = urllib.request.Request(url, method=method, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return resp.status, body, dict(resp.headers)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace") if e.fp else ""
        return e.code, body, dict(e.headers or {})
    except urllib.error.URLError as e:
        raise BlikkApiError(f"could not reach {url}: {e.reason}") from e
    except TimeoutError as e:
        raise BlikkApiError(f"request to {url} timed out after {HTTP_TIMEOUT_SECONDS}s") from e


# --------------------------------------------------------------------------
# Blikk API
# --------------------------------------------------------------------------

def authenticate(base_url: str, app_id: str, app_secret: str) -> str:
    """POST /v1/Auth/Token with Basic auth, returns a bearer access token."""
    url = f"{base_url}/v1/Auth/Token"
    raw = f"{app_id}:{app_secret}".encode("utf-8")
    b64 = base64.b64encode(raw).decode("ascii")
    headers = {"Authorization": f"Basic {b64}", "Accept": "application/json"}

    status, body, _ = _http_request(url, "POST", headers)

    if status == 401:
        raise BlikkApiError(
            "authentication rejected (401) - check BLIKK_APP_ID / BLIKK_APP_SECRET"
        )
    if status != 200:
        raise BlikkApiError(f"POST /v1/Auth/Token failed with HTTP {status}: {body}")

    try:
        data = json.loads(body)
    except json.JSONDecodeError as e:
        raise BlikkApiError("could not parse auth response as JSON") from e

    token = data.get("accessToken")
    if not token:
        raise BlikkApiError("auth response did not contain an accessToken")
    return token


def fetch_month_timereports(
    base_url: str,
    token: str,
    from_date: str,
    to_date: str,
    user_id: str,
) -> List[Dict[str, Any]]:
    """Fetches every time report for `user_id` in [from_date, to_date]
    (inclusive, "YYYY-MM-DD"). Paginates internally and retries on HTTP 429,
    honouring Retry-After.

    Returns a flat list of simplified time report dicts.
    """
    headers = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
    reports: List[Dict[str, Any]] = []
    page = 1
    total_pages = 1

    while page <= total_pages:
        if page > MAX_PAGES:
            raise BlikkApiError(
                f"aborting after {MAX_PAGES} pages - unexpectedly large result set"
            )

        params = [
            ("page", page),
            ("pageSize", PAGE_SIZE),
            ("filter.from", from_date),
            ("filter.to", to_date),
            # filter.userIds is documented as an "integer array"; the public
            # docs don't show a concrete example, so this uses ASP.NET Web
            # API's usual convention for array filters (repeat the query
            # key). A single value works either way this is interpreted.
            ("filter.userIds", user_id),
        ]

        url = f"{base_url}/v1/Core/TimeReports?" + urllib.parse.urlencode(params)

        retries = 0
        while True:
            status, body, resp_headers = _http_request(url, "GET", headers)
            if status == 429 and retries < MAX_429_RETRIES:
                retry_after_header = resp_headers.get("Retry-After")
                wait = (
                    int(retry_after_header)
                    if retry_after_header and retry_after_header.isdigit()
                    else DEFAULT_RETRY_AFTER_SECONDS
                )
                time.sleep(wait)
                retries += 1
                continue
            break

        if status != 200:
            raise BlikkApiError(
                f"GET /v1/Core/TimeReports (page {page}) failed with HTTP {status}: {body}"
            )

        try:
            data = json.loads(body)
        except json.JSONDecodeError as e:
            raise BlikkApiError(
                f"could not parse time reports response as JSON (page {page})"
            ) from e

        total_pages = data.get("totalPages") or 1

        for item in data.get("items", []):
            user = item.get("user") or {}
            reports.append(
                {
                    "id": item.get("id"),
                    "date": item.get("date"),
                    "hours": item.get("hours") or 0,
                    "userId": user.get("id"),
                    "userName": user.get("name") or "",
                    # The list response only carries the two dates; the
                    # public API exposes filter.isSentToAttest /
                    # filter.isAttested as booleans, so derive them here.
                    "sentToAttest": bool(item.get("sentToAttestDate")),
                    "attested": bool(item.get("attestedDate")),
                }
            )

        page += 1

    return reports


# --------------------------------------------------------------------------
# Summarising / reporting
# --------------------------------------------------------------------------

def pending_status(report: Dict[str, Any]) -> str:
    if report["attested"]:
        return "attested"
    if report["sentToAttest"]:
        return "sentAwaitingAttest"
    return "notSentToAttest"


def _new_user_summary(user_id: Optional[int], user_name: str) -> Dict[str, Any]:
    return {
        "userId": user_id,
        "userName": user_name,
        "totalReports": 0,
        "totalHours": 0.0,
        "notSentToAttest": 0,
        "sentAwaitingAttest": 0,
        "attested": 0,
    }


def build_summary(reports: List[Dict[str, Any]]) -> Dict[str, Any]:
    summary: Dict[str, Any] = {
        "totalReports": 0,
        "totalHours": 0.0,
        "notSentToAttest": 0,
        "sentAwaitingAttest": 0,
        "attested": 0,
        "users": {},  # keyed by userId while building, flattened to a list at the end
    }

    for report in reports:
        user_id = report["userId"]
        user = summary["users"].setdefault(user_id, _new_user_summary(user_id, report["userName"]))

        status = pending_status(report)
        bucket = {
            "attested": "attested",
            "sentAwaitingAttest": "sentAwaitingAttest",
            "notSentToAttest": "notSentToAttest",
        }[status]

        for target in (summary, user):
            target["totalReports"] += 1
            target["totalHours"] += report["hours"]
            target[bucket] += 1

    summary["users"] = list(summary["users"].values())
    for user in summary["users"]:
        user["allAttested"] = user["totalReports"] > 0 and user["attested"] == user["totalReports"]

    return summary


def all_attested(summary: Dict[str, Any]) -> bool:
    return summary["totalReports"] > 0 and summary["attested"] == summary["totalReports"]


def to_json_document(
    summary: Dict[str, Any],
    reports: List[Dict[str, Any]],
    month: str,
    from_date: str,
    to_date: str,
    generated_at: str,
) -> Dict[str, Any]:
    pending_reports = [
        {
            "id": r["id"],
            "date": r["date"],
            "userId": r["userId"],
            "userName": r["userName"],
            "hours": r["hours"],
            "status": pending_status(r),
        }
        for r in reports
        if not r["attested"]
    ]

    return {
        "objectName": "blikkAttestCheck.summary",
        "generatedAt": generated_at,
        "month": month,
        "from": from_date,
        "to": to_date,
        "totalReports": summary["totalReports"],
        "totalHours": summary["totalHours"],
        "notSentToAttest": summary["notSentToAttest"],
        "sentAwaitingAttest": summary["sentAwaitingAttest"],
        "attested": summary["attested"],
        "allAttested": all_attested(summary),
        "users": summary["users"],
        "pendingReports": pending_reports,
    }


def print_text_report(
    summary: Dict[str, Any],
    reports: List[Dict[str, Any]],
    month: str,
    from_date: str,
    to_date: str,
) -> None:
    print(f"Blikk attestation check for {month} ({from_date} - {to_date})")
    print(f"  Total time reports : {summary['totalReports']} ({summary['totalHours']:.2f} h)")
    print(f"  Attested           : {summary['attested']}")
    print(f"  Awaiting attest    : {summary['sentAwaitingAttest']}")
    print(f"  Not sent to attest : {summary['notSentToAttest']}")
    print(f"  All attested       : {'yes' if all_attested(summary) else 'no'}")

    if len(summary["users"]) > 1:
        print("\nPer user:")
        for user in summary["users"]:
            print(
                f"  - {user['userName']} (id {user['userId']}): "
                f"{user['totalReports']} reports, {user['attested']} attested, "
                f"{user['sentAwaitingAttest']} awaiting, {user['notSentToAttest']} not sent"
            )

    pending = [r for r in reports if not r["attested"]]
    if pending:
        print("\nNot yet attested:")
        for r in pending:
            print(f"  - {r['date']}  {r['userName']:<24}  {r['hours']:>5.2f} h  [{pending_status(r)}]")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def month_bounds(year: int, month: int) -> Tuple[str, str]:
    last_day = calendar.monthrange(year, month)[1]
    return f"{year:04d}-{month:02d}-01", f"{year:04d}-{month:02d}-{last_day:02d}"


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="blikk_attest_check.py",
        description="Checks the Blikk attestation flags for the current month's time reports.",
        epilog=(
            "Exit codes:\n"
            "  0  all time reports in the period are attested\n"
            "  1  request succeeded, but some reports are not (yet) attested\n"
            "  2  bad arguments / missing configuration\n"
            "  3  authentication or API request failed"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--app-id", default=os.environ.get("BLIKK_APP_ID"),
                         help="Blikk API application id (or env BLIKK_APP_ID)")
    parser.add_argument("--app-secret", default=os.environ.get("BLIKK_APP_SECRET"),
                         help="Blikk API application secret (or env BLIKK_APP_SECRET)")
    parser.add_argument("--user-id", default=os.environ.get("BLIKK_USER_ID"),
                         help="Your Blikk user id - only this user's time reports are "
                              "fetched (or env BLIKK_USER_ID). Required.")
    parser.add_argument("--month", default=None, metavar="YYYY-MM",
                         help="Check this month instead of the current one.")
    parser.add_argument("--base-url", default=os.environ.get("BLIKK_BASE_URL") or DEFAULT_BASE_URL,
                         help=f"Override the API base URL (or env BLIKK_BASE_URL). Default: {DEFAULT_BASE_URL}")
    parser.add_argument("--format", choices=["json", "text"], default="json",
                         help="Output format. Default: json")

    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)

    if not args.app_id or not args.app_secret:
        print(
            "error: missing Blikk API credentials "
            "(set --app-id/--app-secret or BLIKK_APP_ID/BLIKK_APP_SECRET)",
            file=sys.stderr,
        )
        return EXIT_USAGE

    if not args.user_id:
        print(
            "error: missing Blikk user id - this tool only ever checks one user's own "
            "time reports (set --user-id or BLIKK_USER_ID to your Blikk user id)",
            file=sys.stderr,
        )
        return EXIT_USAGE

    if args.month:
        try:
            year, month = (int(p) for p in args.month.split("-", 1))
            if not (1 <= month <= 12):
                raise ValueError
        except ValueError:
            print(f'error: --month expects YYYY-MM, got "{args.month}"', file=sys.stderr)
            return EXIT_USAGE
    else:
        today = datetime.date.today()
        year, month = today.year, today.month

    month_str = f"{year:04d}-{month:02d}"
    from_date, to_date = month_bounds(year, month)

    try:
        token = authenticate(args.base_url, args.app_id, args.app_secret)
        reports = fetch_month_timereports(args.base_url, token, from_date, to_date, args.user_id)
        if any(r["userId"] is not None and str(r["userId"]) != str(args.user_id) for r in reports):
            # Defensive check: the API is expected to already scope this to
            # filter.userIds, but never surface another user's data if it
            # somehow didn't.
            reports = [r for r in reports if str(r["userId"]) == str(args.user_id)]
    except BlikkApiError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_ERROR

    summary = build_summary(reports)

    if args.format == "text":
        print_text_report(summary, reports, month_str, from_date, to_date)
    else:
        generated_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        document = to_json_document(summary, reports, month_str, from_date, to_date, generated_at)
        print(json.dumps(document, indent=2, ensure_ascii=False))

    return EXIT_OK if all_attested(summary) else EXIT_PENDING


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
