#define PHONE_NUMBER_MAX_LEN 15
#define DIALING_DIGITS "0123456789*#+ABC"

/* should have at most 256 things */
enum ops {
    CMD_NONE = 0,
    CMD_DIAL,
    CMD_ANSWER,
    CMD_HANGUP,
};

struct command {
    int index;
    enum ops op;
    union {
        struct {
            char *num;
        } dial;
    } data;
};


/* should have at most 256 things */
enum types {
    TYPE_NONE = 0,
    TYPE_STRING,
};

#define MAX_PARAMS 1
struct command_args {
    char *atcmd;
    char type[MAX_PARAMS + 1];
};

struct command_args cmddata[] = {
    [CMD_DIAL] = { "ATD%s;\r", { TYPE_STRING, TYPE_NONE} },
    [CMD_ANSWER] = { "ATA\r", { TYPE_NONE} },
    [CMD_HANGUP] = { "ATH\r", { TYPE_NONE} },
};
