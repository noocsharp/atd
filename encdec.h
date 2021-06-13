int atd_cmd_dial(int fd, char *num);
int atd_cmd_hangup(int fd);
int atd_cmd_answer(int fd);
int atd_cmd_call_events(int fd);
int atd_status_call(int fd, enum callstatus status, char *num);
ssize_t dec_str(char *in, char **out);
int dec_call_status(int fd, struct call *calls);
