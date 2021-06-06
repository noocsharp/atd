#include <stdbool.h>

#define PHONE_NUMBER_MAX_LEN 15
#define DIALING_DIGITS "0123456789*#+ABC"

/* should have at most 256 things */
enum ops {
    CMD_NONE = 0,
    CMD_DIAL,
    CMD_ANSWER,
    CMD_HANGUP,
    CMD_CALL_EVENTS,
};

enum callstatus {
    CALL_ACTIVE,
    CALL_HELD,
    CALL_DIALING,
    CALL_ALERTING,
    CALL_INCOMING,
    CALL_WAITING,
};

enum status {
	STATUS_OK = 1,
	STATUS_ERROR,
	STATUS_CALL,
};

enum atcmd {
	ATD,
	ATA,
	ATH,
	CLCC,
};

union atdata {
	struct {
		char *num;
	} dial;
};

struct command {
    int index;
    enum ops op;
    union atdata data;
};

struct call {
    bool present;
	enum callstatus status;
	char num[PHONE_NUMBER_MAX_LEN];
};


/* should have at most 256 things */
enum type {
    TYPE_NONE = 0,
    TYPE_STRING,
};

#define MAX_PARAMS 1
struct command_args {
	enum atcmd atcmd;
    enum type type[MAX_PARAMS + 1];
};

struct command_args cmddata[] = {
    [CMD_DIAL] = { ATD, { TYPE_STRING, TYPE_NONE} },
    [CMD_ANSWER] = { ATA, { TYPE_NONE} },
    [CMD_HANGUP] = { ATH, { TYPE_NONE} },
};

char *atcmds[] = {
	[ATD] = "ATD%s;\r",
	[ATA] = "ATA\r",
	[ATH] = "ATH\r",
	[CLCC] = "AT+CLCC\r",
};
