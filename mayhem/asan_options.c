/*
 * Disable LeakSanitizer for Mayhem coverage runs.
 *
 * LSan ptrace-attaches to the process at exit to walk the heap for leaks.
 * Mayhem already traces the process (for edge coverage), so LSan's ptrace
 * call fails → SIGABRT at exit → Mayhem records 0 edges for the run.
 *
 * Providing a STRONG definition of __asan_default_options() here overrides
 * the WEAK copy inside the ASan runtime and injects detect_leaks=0 before
 * any user code or environment-variable processing runs.
 */
const char *__asan_default_options(void) {
    return "detect_leaks=0";
}
