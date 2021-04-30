#define QUEUE_SIZE 50

struct {
    struct command cmds[QUEUE_SIZE];
    int first;
    int next; /* where to place the next command */
    int count;
} cmdq;

int command_enqueue(struct command cmd) {
    assert(cmdq.count <= QUEUE_SIZE);
    if (cmdq.count == QUEUE_SIZE)
        return -1;

    cmdq.cmds[cmdq.next] = cmd;
    cmdq.next = (cmdq.next + 1) % QUEUE_SIZE;
    return ++cmdq.count;
}

struct command command_dequeue() {
    struct command cmd;
    if (cmdq.count == 0)
        return (struct command){ .op = CMD_NONE };

    cmd = cmdq.cmds[cmdq.first];
    cmdq.first = (cmdq.first + 1) % QUEUE_SIZE;
    cmdq.count--;
    return cmd;
}
