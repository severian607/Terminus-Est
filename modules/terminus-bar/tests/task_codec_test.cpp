#include "tasks/task_codec.h"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition)
      std::cerr << "task_codec_test: " << message << '\n';
    return condition;
  }
  std::string calendar(const std::string& due) {
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Task test//EN\r\n"
           "BEGIN:VTODO\r\nUID:original\r\nSUMMARY:Midnight boundary\r\n"
        + due
        + "\r\nEND:VTODO\r\nEND:VCALENDAR\r\n";
  }
} // namespace

int main() {
  // Each test executable has its own environment; no host timezone changes.
  setenv("TZ", "America/New_York", 1);
  tzset();
  bool ok = true;
  const auto utc = calendar("DUE:20260914T003000Z");
  const auto timed = tasks::parseTask(utc);
  ok &= expect(timed && timed->dueDate == "2026-09-13", "UTC due instant must use the local day");
  const auto floating = tasks::parseTask(calendar("DUE:20260914T003000"));
  ok &= expect(floating && floating->dueDate == "2026-09-14", "floating due must retain its civil date");
  const auto allDay = tasks::parseTask(calendar("DUE;VALUE=DATE:20260914"));
  ok &= expect(allDay && allDay->dueDate == "2026-09-14", "all-day due must not shift timezone");
  auto zoned = calendar("DUE;TZID=Test/East:20260914T003000");
  zoned.insert(
      zoned.find("BEGIN:VTODO"),
      "BEGIN:VTIMEZONE\r\nTZID:Test/East\r\nBEGIN:STANDARD\r\n"
      "DTSTART:19700101T000000\r\nTZOFFSETFROM:+0200\r\nTZOFFSETTO:+0200\r\n"
      "END:STANDARD\r\nEND:VTIMEZONE\r\n"
  );
  const auto embedded = tasks::parseTask(zoned);
  ok &= expect(
      embedded && embedded->dueDate == "2026-09-13", "embedded TZID must resolve before local-day classification"
  );
  if (timed) {
    const TaskDraft draft{timed->title, "Edited notes", timed->dueDate};
    const auto edited = tasks::editTask(utc, *timed, &draft);
    ok &= expect(
        edited.find("DUE:20260914T003000Z") != std::string::npos,
        "unchanged displayed due day must preserve original timed DUE"
    );
  }
  auto series = calendar(
      "DUE;VALUE=DATE:20260914\r\nRRULE:FREQ=DAILY;COUNT=3\r\n"
      "STATUS:COMPLETED\r\nCOMPLETED:20260913T100000Z\r\nX-TASKS-PRIVATE:keep\r\n"
      "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT10M\r\nDESCRIPTION:Reminder\r\nEND:VALARM"
  );
  series.insert(
      series.find("END:VCALENDAR"),
      "BEGIN:VTODO\r\nUID:original\r\n"
      "RECURRENCE-ID;VALUE=DATE:20260915\r\nSUMMARY:Exception\r\nEND:VTODO\r\n"
  );
  const auto restored = tasks::restoreTask(series, "restored");
  ok &= expect(
      restored.find("UID:original") == std::string::npos,
      "restore must replace identity across the whole recurrence series"
  );
  ok &= expect(
      tasks::equivalent(tasks::restoreTask(restored, "original"), series),
      "restore must preserve recurrence exceptions, alarms, completion and provider metadata"
  );
  ok &= expect(tasks::validDraft({"Title", "", "2026-02-28"}), "valid draft with due date must be accepted");
  ok &= expect(tasks::validDraft({"Title", "", ""}), "empty due date must be accepted");
  ok &= expect(!tasks::validDraft({"Title", "", "2026-02-30"}), "impossible calendar day must be refused");
  ok &= expect(!tasks::validDraft({"Title", "", "2026-2-28"}), "malformed due date must be refused");
  ok &= expect(!tasks::validDraft({" \t", "", ""}), "blank title must be refused");
  return ok ? 0 : 1;
}
