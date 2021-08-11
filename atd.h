#ifndef ATD_H
#define ATD_H

#include <stdbool.h>

#define PHONE_NUMBER_MAX_LEN 15
#define DIALING_DIGITS "0123456789*#+ABC"
#define MAX_CALLS 8

/* should have at most 256 things */
enum ops {
    CMD_NONE = 0,
    CMD_DIAL,
    CMD_ANSWER,
    CMD_HANGUP,
    CMD_CALL_EVENTS,
    CMD_SMS_EVENTS,
    CMD_SUBMIT,
};

enum callstatus {
    CALL_ACTIVE,
    CALL_HELD,
    CALL_DIALING,
    CALL_INCOMING,
    CALL_ANSWERED,
    CALL_INACTIVE,
    CALL_LAST,
};

enum status {
	STATUS_OK = 1,
	STATUS_ERROR,
	STATUS_CALL,
	STATUS_DELIVERED,
};

enum atcmd {
	ATNONE,
	ATD,
	ATA,
	ATH,
	CLCC,
	ATCMGS,
};

union atdata {
	struct {
		char *num;
	} dial;
	struct {
		char len;
		char *pdu;
	} submit;
};

struct command {
    int index;
    enum ops op;
    union atdata data;
};

struct call {
	enum callstatus status;
	char num[PHONE_NUMBER_MAX_LEN + 1];
};

struct sms {
	char num[PHONE_NUMBER_MAX_LEN + 1];
	char *msg;
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

#endif
