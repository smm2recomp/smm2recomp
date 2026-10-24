// Which parts of Course World the mod answers, per online backend (hybrid.h).
#include <string.h>

#include "hybrid.h"

HyMode hy_mode(int backend, bool hundred, const char *course_world) {
    const char *cw = course_world ? course_world : "";
    HyMode none = {false, false, false}, hybrid = {true, false, true};
    switch (backend) {
    case 0: return (HyMode){hundred, hundred, false};          // ours: the mod's whole path, or nothing
    case 1: return !strcmp(cw, "mod") ? hybrid : none;          // local-nex: hybrid only when asked for (tests)
    case 2: return !strcmp(cw, "backend") ? none : hybrid;      // nextendo: hybrid unless Course World is theirs
    case 3: return none;                                        // ocw: all of it is OCW's own, the mod answers none
    default: return none;
    }
}
