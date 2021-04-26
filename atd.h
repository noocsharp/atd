#define PHONE_NUMBER_MAX_LEN 15
#define DIALING_DIGITS "0123456789*#+ABC"

enum ops {
    CMD_NONE = 0,
    CMD_DIAL,
    CMD_ANSWER,
    CMD_HANGUP,
};

struct command {
    enum ops op;
    void *data;
};

struct data_dial {
    char num[20];
    size_t count;
};
