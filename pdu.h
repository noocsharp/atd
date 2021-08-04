enum dcs {
	DCS_GSM = 0,
	DCS_UCS2 = 8,
};

enum smstype {
	SMS_DELIVER = 0,
	SMS_SUBMIT = 1,
};

struct phonenumber {
	uint8_t len;
	char enc;
	char number[15];
};

/* data is utf-8 encoded */
struct message {
	char *data;
	uint8_t len;
};

struct udh {
	uint16_t ref;
	uint8_t part;
	uint8_t parts;
};

struct sms_deliver_msg {
	struct phonenumber sender;
	enum dcs dcs;
	char pid;
	bool mms;
	bool udhi;
	struct {
		char year;
		char month;
		char day;
	} date;
	struct {
		char hour;
		char minute;
		char second;
		char tz;
	} time;
	struct udh udh;
	struct message msg;
};

struct pdu_msg {
	struct phonenumber smsc;
	enum smstype smstype;
	union {
		struct sms_deliver_msg d;
	} d;
};

int encode_pdu(char *dest, char *number, char *message);
int decode_pdu(struct pdu_msg *pdu_msg, char *raw);
char *htoa(char *str, char val);
