#include "kernel.h"

/* Enaga's log: the last LOG_LINES lines, shown on Enaga's own screen and read by the emulator tests. */
char log_lines[LOG_LINES][LOG_WIDTH];
u32 log_count;

void log_line(const char *text)
{
    u32 gie = irq_global_disable();           /* apps may log from the audio callback */
    char *line = log_lines[log_count % LOG_LINES];
    unsigned i;
    for (i = 0; i + 1 < LOG_WIDTH && text[i]; i++) line[i] = text[i];
    line[i] = 0;
    log_count++;
    if (gie) irq_global_enable();
}
