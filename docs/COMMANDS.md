# Command reference

| Command | Meaning |
| --- | --- |
| break LOCATION | Set an address, symbol, module-symbol, or source-line breakpoint. |
| continue | Resume every stopped traced thread. |
| stepi | Execute one instruction on the selected thread. |
| step, next, finish | Source-step, step over, or run to the current frame return. |
| watch ADDRESS [r|w|rw] [1|2|4|8] | Set a hardware watchpoint. |
| info break, info threads, info sharedlibrary | Inspect debugger state. |
| thread TID, frame N | Select a stopped thread or displayed frame. |
| backtrace, info locals, print NAME | Inspect source-level state. |
| info registers, register read NAME, register write NAME VALUE | Inspect or update registers. |
| x ADDRESS [COUNT], set memory ADDRESS VALUE | Read words or write a word of target memory. |
| quit | Restore software breakpoints and terminate Cosy. |
