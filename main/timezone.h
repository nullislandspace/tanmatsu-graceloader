#pragma once
// Graceloader — local time for the app and its files (see timezone.c).
//
// Applies the timezone the launcher stored in NVS to this process, so
// the app sees local time and the files it writes are stamped in local
// time (FATFS timestamps come from localtime_r). Call once, early,
// before anything is mounted or written. Falls back to UTC, with a log
// line saying why.
void graceloader_apply_timezone(void);
