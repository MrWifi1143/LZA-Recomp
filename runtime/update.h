#pragma once
#include <stdbool.h>
// runtime/update.c: the latest release of RT_UPDATE_REPO (paths.h) - a look at start, the launcher's update line
#ifdef __cplusplus
extern "C" {
#endif
enum { UPD_IDLE, UPD_CHECKING, UPD_NONE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_INSTALLING, UPD_DONE, UPD_FAILED };
void update_check_start(void);    // once, in the background (check_updates off: nothing)
void update_cleanup(void);        // at start: the files the last update set aside
int update_state(void);
const char *update_version(void); // the newer release's version ("1.2.2")
const char *update_error(void);
bool update_can_install(void);    // false: a link to the release page instead (a folder install, another platform)
float update_progress(void);      // 0..1 while downloading
void update_install_start(void);  // download + put in place, in the background
void update_open_page(void);
const char *update_current_version(void);  // RT_APP_VERSION (LARECOMP_UPDATE_AS: that, for tests)
#ifdef __cplusplus
}
#endif
