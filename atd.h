#define PHONE_NUMBER_MAX_LEN 15
#define DIALING_DIGITS "0123456789*#+ABC"

enum ops {
    CMD_NONE = 0,
    CMD_DIAL,
    CMD_ANSWER,
    CMD_HANGUP,
};

struct command {
    int index;
    enum ops op;
    void *data;
};

char *cmd_to_at[] = {
    [CMD_DIAL] = "ATD%s;\r\n",
    [CMD_ANSWER] = "ATA\r\n",
    [CMD_HANGUP] = "ATH\r\n",
};
