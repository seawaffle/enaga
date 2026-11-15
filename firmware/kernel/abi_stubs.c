/* libgcc's unwinder references abort(); nothing in Enaga unwinds. */
void abort(void) { for (;;) ; }
