/* libgcc's unwinder references abort(); nothing in Enaga unwinds. */
void abort(void) { for (;;) ; }

/* libgcc's helpers carry unwind tables naming these personality routines (exFAT's 64-bit division in FatFs
 * pulls in __c6xabi_divlli, whose table names pr3); defining them keeps libgcc's unwinder out of the link. */
void __c6xabi_unwind_cpp_pr0(void) { abort(); }
void __c6xabi_unwind_cpp_pr1(void) { abort(); }
void __c6xabi_unwind_cpp_pr2(void) { abort(); }
void __c6xabi_unwind_cpp_pr3(void) { abort(); }
void __c6xabi_unwind_cpp_pr4(void) { abort(); }
