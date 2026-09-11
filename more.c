ptrace(PTRACE_SETOPTIONS, m_pid, nullptr, PTRACE_O_TRACECLONE);

case (SIGTRAP | (PTRACE_EVENT_CLONE << 8)):
    //get the new thread ID
    unsigned long event_message = 0;
    ptrace(PTRACE_GETEVENTMSG, pid, nullptr, message);

    //handle creation
    //...

    