#!/usr/bin/env python3
"""Export the current user's ZZU course table as an RFC 5545 ICS file."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


API_BASE_URL = "https://jwxt.zzu.edu.cn/eams-micro-server/api/v1"
SEMESTERS_TARGET = "/semester/std-semesters"
CURRENT_SEMESTER_TARGET = "/semester/current-semester"
COURSE_TABLE_TARGET = "/lesson/student/course-table/{semester_id}?hasExperiment=true"
COURSE_APP_URL = "https://jwxt.zzu.edu.cn/eams-student-course-table-app/index.html"
SUPER_APP_UA = (
    "Mozilla/5.0 (iPhone; CPU iPhone OS 18_7 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 "
    "SuperApp SuperApp-10460 appVersion-2.5.2"
)


class CourseError(RuntimeError):
    pass


def state_directory() -> Path:
    configured = os.environ.get("ZZUASSISTANT_STATE_DIR")
    if configured:
        return Path(configured)
    if sys.platform == "win32":
        root = os.environ.get("LOCALAPPDATA")
        if root:
            return Path(root) / "ZZUAssistant"
    elif sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "ZZUAssistant"
    else:
        root = os.environ.get("XDG_STATE_HOME")
        if root:
            return Path(root) / "zzu-assistant"
        return Path.home() / ".local" / "state" / "zzu-assistant"
    raise CourseError("Cannot determine the ZZUAssistant state directory")


def current_user(root: Path) -> str:
    path = root / "current-app-user.db"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CourseError("No current Super App user; run 'app login <username>' first") from error
    if len(lines) < 2 or lines[0] != "ZZUAssistant-AppUser-v1" or not lines[1]:
        raise CourseError("The current Super App user file is invalid")
    return lines[1]


def load_id_token(root: Path, username: str) -> str:
    digest = hashlib.sha256(username.encode("utf-8")).hexdigest()
    path = root / "sessions" / f"{digest}.superapp.db"
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise CourseError(
            f"No Super App session for {username}; run 'app login {username}' first"
        ) from error
    except (OSError, json.JSONDecodeError) as error:
        raise CourseError(f"Cannot read the Super App session for {username}") from error
    token = state.get("idToken")
    if not isinstance(token, str) or not token:
        raise CourseError(f"The Super App session for {username} has no idToken")
    try:
        encoded = token.split(".", 2)[1]
        encoded += "=" * (-len(encoded) % 4)
        claims = json.loads(base64.urlsafe_b64decode(encoded).decode("utf-8"))
        expires = claims.get("exp")
        if isinstance(expires, (int, float)) and expires <= time.time():
            raise CourseError(
                f"The Super App session for {username} expired; run 'app login {username}' again"
            )
    except (IndexError, ValueError, UnicodeDecodeError, json.JSONDecodeError):
        raise CourseError("The saved Super App idToken is malformed") from None
    return token


def request_json(target: str, token: str) -> Any:
    referer = COURSE_APP_URL + "?idToken=" + urllib.parse.quote(token, safe="")
    request = urllib.request.Request(
        API_BASE_URL + target,
        headers={
            "Accept": "application/json",
            "Authorization": token,
            "Referer": referer,
            "User-Agent": SUPER_APP_UA,
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            status = response.status
            body = response.read()
    except urllib.error.HTTPError as error:
        if error.code in {401, 403}:
            raise CourseError(
                "The course system rejected the Super App session; run 'app login' again"
            ) from error
        raise CourseError(f"Course API returned HTTP {error.code}") from error
    except urllib.error.URLError as error:
        raise CourseError(f"Cannot reach the course system: {error.reason}") from error
    if status != 200:
        raise CourseError(f"Course API returned HTTP {status}")
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CourseError("Course API returned invalid JSON") from error
    if not isinstance(payload, dict) or payload.get("result") != 0:
        message = payload.get("message") if isinstance(payload, dict) else None
        raise CourseError(str(message or "Course API reported an error"))
    return payload.get("data")


def select_semester(token: str, selector: str) -> dict[str, Any]:
    if selector == "current":
        semester = request_json(CURRENT_SEMESTER_TARGET, token)
        if not isinstance(semester, dict):
            raise CourseError("Current semester response is incomplete")
        return semester
    semesters = request_json(SEMESTERS_TARGET, token)
    if not isinstance(semesters, list):
        raise CourseError("Semester list response is incomplete")
    for semester in semesters:
        if not isinstance(semester, dict):
            continue
        if str(semester.get("id")) == selector or semester.get("code") == selector:
            return semester
    raise CourseError(f"Semester not found: {selector}")


def ics_escape(value: Any) -> str:
    text = "" if value is None else str(value)
    return (
        text.replace("\\", "\\\\")
        .replace("\r\n", "\\n")
        .replace("\r", "\\n")
        .replace("\n", "\\n")
        .replace(";", "\\;")
        .replace(",", "\\,")
    )


def fold_line(line: str) -> list[str]:
    result: list[str] = []
    current = ""
    limit = 75
    for character in line:
        candidate = current + character
        if current and len(candidate.encode("utf-8")) > limit:
            result.append(current)
            current = " " + character
            limit = 75
        else:
            current = candidate
    result.append(current)
    return result


def local_datetime(date: str, hhmm: Any) -> str:
    try:
        numeric = int(hhmm)
        hour, minute = divmod(numeric, 100)
        parsed = datetime.strptime(date, "%Y-%m-%d")
        if hour > 23 or minute > 59:
            raise ValueError
    except (TypeError, ValueError) as error:
        raise CourseError(f"Invalid course date or time: {date} {hhmm}") from error
    return f"{parsed:%Y%m%d}T{hour:02d}{minute:02d}00"


def event_lines(lesson: dict[str, Any], schedule: dict[str, Any],
                semester_id: Any, stamp: str) -> list[str]:
    course = lesson.get("course") or {}
    course_name = course.get("nameZh") or course.get("nameEn") or lesson.get("code") or "Course"
    project = schedule.get("expProjectNameZh") or lesson.get("expProjectName")
    summary = f"{course_name} - {project}" if project else str(course_name)
    room = schedule.get("room") or {}
    campus = room.get("campus") or lesson.get("campus") or {}
    place = schedule.get("customPlace") or room.get("nameZh") or room.get("nameEn") or ""
    location = " ".join(filter(None, [campus.get("nameZh") or campus.get("nameEn"), place]))
    teacher = schedule.get("teacherName") or schedule.get("teacherNameEn")
    if not teacher:
        teachers = lesson.get("teacherAssignmentList") or []
        teacher = ", ".join(str(item) for item in teachers if item)
    description_parts = [
        f"Teacher: {teacher}" if teacher else "",
        f"Week: {schedule.get('weekIndex')}" if schedule.get("weekIndex") is not None else "",
        (f"Sections: {schedule.get('startUnit')}-{schedule.get('endUnit')}"
         if schedule.get("startUnit") is not None else ""),
        f"Course code: {course.get('code') or lesson.get('code')}"
        if course.get("code") or lesson.get("code") else "",
        f"Type: {schedule.get('lessonType')}" if schedule.get("lessonType") else "",
    ]
    description = "\n".join(part for part in description_parts if part)
    date = schedule.get("date")
    start = schedule.get("realStartTime") or schedule.get("startTime")
    end = schedule.get("realEndTime") or schedule.get("endTime")
    uid = "-".join(str(value) for value in (
        semester_id, lesson.get("id"), schedule.get("scheduleGroupId"),
        date, start,
    )) + "@zzuassistant"
    lines = [
        "BEGIN:VEVENT",
        f"UID:{ics_escape(uid)}",
        f"DTSTAMP:{stamp}",
        f"DTSTART;TZID=Asia/Shanghai:{local_datetime(str(date), start)}",
        f"DTEND;TZID=Asia/Shanghai:{local_datetime(str(date), end)}",
        f"SUMMARY:{ics_escape(summary)}",
    ]
    if location:
        lines.append(f"LOCATION:{ics_escape(location)}")
    if description:
        lines.append(f"DESCRIPTION:{ics_escape(description)}")
    course_type = lesson.get("courseType") or {}
    if course_type.get("nameZh") or course_type.get("nameEn"):
        lines.append(f"CATEGORIES:{ics_escape(course_type.get('nameZh') or course_type.get('nameEn'))}")
    lines.extend(("STATUS:CONFIRMED", "TRANSP:OPAQUE", "END:VEVENT"))
    return lines


def build_calendar(semester: dict[str, Any], lessons: list[Any]) -> tuple[str, int]:
    semester_id = semester.get("id")
    semester_code = semester.get("code") or str(semester_id)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    lines = [
        "BEGIN:VCALENDAR",
        "PRODID:-//ZZUAssistant//Course Schedule//EN",
        "VERSION:2.0",
        "CALSCALE:GREGORIAN",
        "METHOD:PUBLISH",
        f"X-WR-CALNAME:{ics_escape('ZZU ' + str(semester_code))}",
        "X-WR-TIMEZONE:Asia/Shanghai",
        "BEGIN:VTIMEZONE",
        "TZID:Asia/Shanghai",
        "X-LIC-LOCATION:Asia/Shanghai",
        "BEGIN:STANDARD",
        "TZOFFSETFROM:+0800",
        "TZOFFSETTO:+0800",
        "TZNAME:CST",
        "DTSTART:19700101T000000",
        "END:STANDARD",
        "END:VTIMEZONE",
    ]
    seen: set[str] = set()
    count = 0
    for lesson in lessons:
        if not isinstance(lesson, dict):
            continue
        for schedule in lesson.get("schedules") or []:
            if not isinstance(schedule, dict) or not schedule.get("date"):
                continue
            event = event_lines(lesson, schedule, semester_id, stamp)
            uid = next(line for line in event if line.startswith("UID:"))
            if uid in seen:
                continue
            seen.add(uid)
            lines.extend(event)
            count += 1
    lines.append("END:VCALENDAR")
    folded = [part for line in lines for part in fold_line(line)]
    return "\r\n".join(folded) + "\r\n", count


def output_path(argument: str | None, semester: dict[str, Any]) -> Path:
    if argument:
        path = Path(argument).expanduser()
    else:
        code = str(semester.get("code") or semester.get("id") or "current")
        path = Path(f"course-{code}.ics")
    return path.resolve()


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export the ZZU course table as an ICS calendar."
    )
    parser.add_argument("username", nargs="?")
    parser.add_argument("--semester", default="current", metavar="CURRENT|ID|CODE")
    parser.add_argument("-o", "--output", metavar="FILE")
    parser.add_argument("--porcelain", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
        root = state_directory()
        username = arguments.username or current_user(root)
        token = load_id_token(root, username)
        semester = select_semester(token, arguments.semester)
        semester_id = semester.get("id")
        if semester_id is None:
            raise CourseError("Selected semester has no ID")
        lessons = request_json(
            COURSE_TABLE_TARGET.format(semester_id=semester_id), token
        )
        if not isinstance(lessons, list):
            raise CourseError("Course table response is incomplete")
        calendar, count = build_calendar(semester, lessons)
        destination = output_path(arguments.output, semester)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + ".tmp")
        temporary.write_text(calendar, encoding="utf-8", newline="")
        temporary.replace(destination)
        code = str(semester.get("code") or semester_id)
        if arguments.porcelain:
            print(f"output={destination}")
            print(f"semester={code}")
            print(f"events={count}")
        else:
            print(f"Course calendar written: {destination}")
            print(f"Semester: {code}")
            print(f"Events: {count}")
        return 0
    except CourseError as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1
    except (OSError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

