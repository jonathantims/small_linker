/* link_tests runs the checks that need cl.exe, link.exe, and a real process.
   The program is freestanding: it does not use the C runtime, and the only
   static import is kernel32.dll. advapi32 is loaded at runtime for the
   registry reads that name the OS and the CPU in the report. */

#if !defined(_M_X64)
#error link_tests must be compiled as a 64-bit executable
#endif

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
typedef signed int int32_t;
typedef signed __int64 int64_t;
typedef signed __int64 intptr_t;
typedef unsigned __int64 size_t;
typedef unsigned short wchar_t;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;

#define NULL ((void *)0)
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define OPEN_EXISTING 3u
#define CREATE_ALWAYS 2u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define PAGE_READWRITE 0x04u
#define STARTF_USESTDHANDLES 0x00000100u
#define CREATE_NO_WINDOW 0x08000000u
#define HANDLE_FLAG_INHERIT 1u
#define WAIT_OBJECT_0 0u
#define ERROR_ALREADY_EXISTS 183u
#define BENCH_ITERS 30

/* Security descriptor and inheritance flag passed to CreatePipe and CreateFileW.
   The test fills one of these when it creates the capture pipe and the NUL device. */
typedef struct {
    /* Byte length of this record. The test sets it to the struct size so the kernel reads bInheritHandle. */
    DWORD nLength;
    /* Optional security descriptor pointer. The test stores NULL so the new handle keeps the default DACL. */
    void *lpSecurityDescriptor;
    /* Nonzero when a child may inherit the handle. The test sets it so the child receives NUL and the pipe write end. */
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;

/* Startup record CreateProcessW reads for the child window and standard handles.
   run_child zeroes it, then sets the size, the std-handle flag, and the three handles. */
typedef struct {
    /* Byte length of STARTUPINFOW. run_child sets it to the struct size before CreateProcessW. */
    DWORD cb;
    /* Reserved pointer that must stay NULL. The test never writes it after the zero fill. */
    wchar_t *lpReserved;
    /* Desktop name for the child. Left NULL so the child stays on the parent desktop. */
    wchar_t *lpDesktop;
    /* Console title. Left NULL because the test creates the child with CREATE_NO_WINDOW. */
    wchar_t *lpTitle;
    /* Suggested window origin x. Left zero; STARTF_USEPOSITION is never set, so CreateProcessW ignores it. */
    DWORD dwX;
    /* Suggested window origin y. Left zero and ignored for the same reason as dwX. */
    DWORD dwY;
    /* Suggested window width. Left zero; the size flag is not set, so the test never places a window. */
    DWORD dwXSize;
    /* Suggested window height. Left zero and unused, matching dwXSize. */
    DWORD dwYSize;
    /* Console buffer width in characters. Left zero; STARTF_USECOUNTCHARS is not set. */
    DWORD dwXCountChars;
    /* Console buffer height in characters. Left zero and unused, matching dwXCountChars. */
    DWORD dwYCountChars;
    /* Console color attribute. Left zero; STARTF_USEFILLATTRIBUTE is not set. */
    DWORD dwFillAttribute;
    /* Mask of which fields are live. The test sets STARTF_USESTDHANDLES so the three standard handles are applied. */
    DWORD dwFlags;
    /* Show-window command. Left zero; STARTF_USESHOWWINDOW is not set and the child has no window. */
    WORD wShowWindow;
    /* Reserved length that must stay zero. The test leaves the zero from memset in place. */
    WORD cbReserved2;
    /* Reserved pointer that must stay NULL. The test does not point it at anything. */
    unsigned char *lpReserved2;
    /* Child standard-input handle. The test passes the inheritable NUL handle so tools do not read a console. */
    HANDLE hStdInput;
    /* Child standard-output handle. Capture runs pass the pipe write end; other runs pass NUL. */
    HANDLE hStdOutput;
    /* Child standard-error handle. The test uses the same handle as standard output, either the pipe or NUL. */
    HANDLE hStdError;
} STARTUPINFOW;

/* Handles and ids CreateProcessW writes after it starts a child.
   run_child waits, queries the exit code, and closes the handles stored here. */
typedef struct {
    /* Process handle of the child. The test waits on it, reads the exit code, may terminate it, then closes it. */
    HANDLE hProcess;
    /* Primary-thread handle. The test closes it right after CreateProcessW and does not wait on the thread. */
    HANDLE hThread;
    /* Process id assigned by the kernel. CreateProcessW writes it; the test does not read it afterward. */
    DWORD dwProcessId;
    /* Primary-thread id. CreateProcessW writes it; the test does not read it afterward. */
    DWORD dwThreadId;
} PROCESS_INFORMATION;

/* Processor and address-space facts filled by GetSystemInfo.
   The report path keeps only the logical processor count and ignores the other fields. */
typedef struct {
    /* Architecture code such as x64. GetSystemInfo fills it; the test does not print it. */
    WORD wProcessorArchitecture;
    /* Padding the API requires between the architecture and the page size. The test leaves it unread. */
    WORD wReserved;
    /* Virtual-memory page size in bytes. GetSystemInfo fills it; the test does not read it. */
    DWORD dwPageSize;
    /* Lowest address an application can use. Filled by GetSystemInfo and not read by the test. */
    void *lpMinimumApplicationAddress;
    /* Highest address an application can use. Filled by GetSystemInfo and not read by the test. */
    void *lpMaximumApplicationAddress;
    /* Bit mask of processors configured into this process. Filled by GetSystemInfo and not read. */
    uint64_t dwActiveProcessorMask;
    /* Count of logical processors. The test copies it into g_cpus for the benchmark report. */
    DWORD dwNumberOfProcessors;
    /* Obsolete processor-type code. GetSystemInfo still fills it; the test does not read it. */
    DWORD dwProcessorType;
    /* Granularity of virtual address reservations. Filled by GetSystemInfo and not read. */
    DWORD dwAllocationGranularity;
    /* Architecture-specific processor level. Filled by GetSystemInfo and not printed. */
    WORD wProcessorLevel;
    /* Architecture-specific processor revision. Filled by GetSystemInfo and not printed. */
    WORD wProcessorRevision;
} SYSTEM_INFO;

/* Local calendar time filled by GetLocalTime.
   The test formats the date and time fields into the g_when stamp on the report. */
typedef struct {
    /* Four-digit year. Printed first in the g_when timestamp. */
    WORD wYear;
    /* Month from 1 to 12. Printed as two digits after the year. */
    WORD wMonth;
    /* Weekday from 0 for Sunday. GetLocalTime fills it; the timestamp does not include it. */
    WORD wDayOfWeek;
    /* Day of the month. Printed as two digits in g_when. */
    WORD wDay;
    /* Hour from 0 to 23. Printed as two digits in g_when. */
    WORD wHour;
    /* Minute from 0 to 59. Printed as two digits in g_when. */
    WORD wMinute;
    /* Second from 0 to 59. Printed as two digits at the end of the clock time. */
    WORD wSecond;
    /* Milliseconds past the second. GetLocalTime fills them; the timestamp stops at seconds. */
    WORD wMilliseconds;
} SYSTEMTIME;

/* Time-zone biases and transition dates from GetTimeZoneInformation.
   The test adds the active bias to Bias and prints the resulting UTC offset in g_when. */
typedef struct {
    /* Minutes to add to local time to reach UTC. The test starts the printed offset from this value. */
    long Bias;
    /* Standard-time zone name. GetTimeZoneInformation fills it; the report does not print it. */
    wchar_t StandardName[32];
    /* Local date when standard time begins. The test relies on the API kind code instead of reading this date. */
    SYSTEMTIME StandardDate;
    /* Extra bias during standard time, usually zero. Added to Bias when the zone kind is standard. */
    long StandardBias;
    /* Daylight-time zone name. Filled by the API and not printed in the report. */
    wchar_t DaylightName[32];
    /* Local date when daylight time begins. Stored for the API and not read by the test. */
    SYSTEMTIME DaylightDate;
    /* Extra bias during daylight time, usually minus 60. Added to Bias when the zone kind is daylight. */
    long DaylightBias;
} TIME_ZONE_INFORMATION;

/* Signed 64-bit value used wherever the Win32 API wants a LARGE_INTEGER.
   File sizes and the performance counter land in QuadPart. */
typedef struct {
    /* The 64-bit payload. GetFileSizeEx and the performance-counter calls store their result here. */
    long long QuadPart;
} LARGE_INTEGER;

typedef char si_size_ok[(sizeof(STARTUPINFOW) == 104) ? 1 : -1];
typedef char sa_size_ok[(sizeof(SECURITY_ATTRIBUTES) == 24) ? 1 : -1];
typedef char pi_size_ok[(sizeof(PROCESS_INFORMATION) == 24) ? 1 : -1];
typedef char sysinfo_size_ok[(sizeof(SYSTEM_INFO) == 48) ? 1 : -1];
typedef char tzi_size_ok[(sizeof(TIME_ZONE_INFORMATION) == 172) ? 1 : -1];

extern void ExitProcess(DWORD code);
extern HANDLE GetStdHandle(DWORD standard_handle);
extern BOOL WriteFile(HANDLE file, const void *buffer, DWORD bytes, DWORD *written, void *overlapped);
extern HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *security, DWORD disposition, DWORD flags, HANDLE template_file);
extern BOOL ReadFile(HANDLE file, void *buffer, DWORD bytes, DWORD *read_count, void *overlapped);
extern BOOL CloseHandle(HANDLE handle);
extern BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size);
extern void *VirtualAlloc(void *address, size_t size, DWORD type, DWORD protect);
extern DWORD GetModuleFileNameW(HANDLE module, wchar_t *filename, DWORD size);
extern DWORD GetLastError(void);
extern BOOL CreateDirectoryW(const wchar_t *path, SECURITY_ATTRIBUTES *security);
extern BOOL DeleteFileW(const wchar_t *path);
extern DWORD GetFileAttributesW(const wchar_t *path);
extern DWORD SearchPathW(const wchar_t *path, const wchar_t *name, const wchar_t *extension, DWORD buffer_chars, wchar_t *buffer, wchar_t **file_part);
extern BOOL CreateProcessW(const wchar_t *application, wchar_t *command, SECURITY_ATTRIBUTES *process_attr, SECURITY_ATTRIBUTES *thread_attr, BOOL inherit, DWORD flags, void *environment, const wchar_t *directory, STARTUPINFOW *startup, PROCESS_INFORMATION *info);
extern DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
extern BOOL GetExitCodeProcess(HANDLE process, DWORD *code);
extern BOOL TerminateProcess(HANDLE process, DWORD code);
extern BOOL CreatePipe(HANDLE *read_pipe, HANDLE *write_pipe, SECURITY_ATTRIBUTES *security, DWORD size);
extern BOOL SetHandleInformation(HANDLE handle, DWORD mask, DWORD flags);
extern BOOL PeekNamedPipe(HANDLE pipe, void *buffer, DWORD buffer_bytes, DWORD *read_count, DWORD *available, DWORD *left);
extern BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
extern BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);
extern void GetSystemInfo(SYSTEM_INFO *info);
extern void GetLocalTime(SYSTEMTIME *time);
extern DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION *zone);
extern DWORD GetTickCount(void);
extern void *LoadLibraryW(const wchar_t *name);
extern void *GetProcAddress(void *module, const char *procedure);

typedef long (*reg_open_fn)(void *key, const wchar_t *subkey, DWORD options, DWORD sam, void **opened);
typedef long (*reg_query_fn)(void *key, const wchar_t *value, DWORD *reserved, DWORD *type, unsigned char *data, DWORD *bytes);
typedef long (*reg_close_fn)(void *key);

/* Fills destination with size bytes of the low eight bits of value and returns destination.
   It reads value and size and writes every byte of destination.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
void *memset(void *destination, int value, size_t size) {
    unsigned char *bytes = (unsigned char *)destination;
    size_t index;
    /* Store the fill byte into each destination byte. */
    for (index = 0; index < size; index++) bytes[index] = (unsigned char)value;
    return destination;
}

/* Copies size bytes from source to destination and returns destination.
   It reads source and writes destination.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *out_bytes = (unsigned char *)destination;
    const unsigned char *in_bytes = (const unsigned char *)source;
    size_t index;
    /* Copy each source byte into the matching destination byte. */
    for (index = 0; index < size; index++) out_bytes[index] = in_bytes[index];
    return destination;
}

static HANDLE g_nul = NULL;
static uint8_t *g_file = NULL;
static size_t g_file_cap = 0;
static size_t g_file_len = 0;
static uint32_t g_pe = 0;
static int g_show_output = 0;
static uint64_t g_qpc_freq = 0;
static wchar_t g_root[1024];
static wchar_t g_exe[1024];
static wchar_t g_small[1024];
static wchar_t g_cl[1024];
static wchar_t g_link[1024];
static wchar_t g_cl_x86[1024];
static wchar_t g_link_x86[1024];
static wchar_t g_report_path[1024];
static wchar_t g_cmd[4096];
static wchar_t g_cmd_run[4096];
static wchar_t g_path_a[1024];
static wchar_t g_path_b[1024];
static wchar_t g_path_c[1024];
static char g_note[2048];
static size_t g_note_len = 0;
static char g_report[65536];
static size_t g_report_len = 0;
static char g_os[320];
static char g_cpu[320];
static char g_cl_ver[512];
static char g_link_ver[512];
static char g_when[64];
static DWORD g_cpus = 0;
static reg_open_fn g_reg_open = NULL;
static reg_query_fn g_reg_query = NULL;
static reg_close_fn g_reg_close = NULL;
static uint8_t g_needle[144];

/* Outcome of the latest child process started from g_cmd.
   run_child fills it, drain_pipe appends captured text, and later checks decide pass or fail from these fields. */
static struct {
    /* Nonzero after CreateProcessW succeeds. Zero means the pipe or the process never launched, and win_error says why. */
    int started;
    /* Nonzero when the wait exceeded the caller timeout and the test terminated the child. */
    int timed_out;
    /* Process exit code from GetExitCodeProcess, or 1 if that query itself fails. */
    DWORD exit_code;
    /* GetLastError value saved when CreatePipe or CreateProcessW fails before the child starts. */
    DWORD win_error;
    /* Captured stdout and stderr, kept NUL-terminated. fail prints it when g_show_output is set. */
    char output[8192];
    /* Number of captured bytes stored in output, not counting the terminator drain_pipe maintains. */
    size_t output_length;
} g_child;

/* A slice of the loaded file image, used while walking PE headers and import names.
   span_rva writes it, and the compare helpers read ptr for remaining bytes. */
typedef struct {
    /* First byte of the slice inside g_file. Callers treat it as raw header bytes or as a C string. */
    const uint8_t *ptr;
    /* How many bytes from ptr are still inside the file. Callers reject a slice shorter than the record they need. */
    uint32_t remaining;
} Span;

/* One linked fixture: the object, both output images, and the header facts the report checks.
   setup_fixture fills the names and paths, and link_pair writes the sizes after both linkers run. */
typedef struct {
    /* Short label such as the case name. Printed in progress lines, failure notes, and the report table. */
    const char *name;
    /* Who produced the object, for example the COFF writer or cl.exe. The report prints it beside the name. */
    const char *producer;
    /* Exit status both linked images must return when the test runs them. */
    int exit_code;
    /* Expected COFF machine, 0x8664 or 0x14c. assert_header compares it with the PE file header. */
    uint16_t machine;
    /* Expected optional-header magic, 0x20b for PE32+ or 0x10b for PE32. assert_header checks it. */
    uint16_t magic;
    /* Nonzero when this fixture is linked with the x86 tools and checked as a PE32 image. */
    int is_x86;
    /* Wide path of the input object file. small_cmd and vs_cmd pass it to the linkers. */
    wchar_t object[1024];
    /* Wide path of the image small_linker writes. The test runs it and records its size. */
    wchar_t small_exe[1024];
    /* Wide path of the image link.exe writes for the same object. Compared with the small_linker image. */
    wchar_t vs_exe[1024];
    /* Byte length of the small_linker image after a successful link. The size table prints it. */
    uint64_t small_size;
    /* Byte length of the link.exe image. The report subtracts it from small_size. */
    uint64_t vs_size;
} Fixture;

/* Timing row for one linker command, measured over BENCH_ITERS runs.
   benchmark_row fills the samples and the summary, and the report prints them as milliseconds. */
typedef struct {
    /* Row label, including which linker was timed. The timing table prints it. */
    const char *name;
    /* One elapsed time in microseconds for each measured link, before the mean and median are taken. */
    uint64_t samples[BENCH_ITERS];
    /* Arithmetic mean of samples, in microseconds. Printed with app_ms. */
    uint64_t mean_us;
    /* Middle sample after sorting, in microseconds. Printed beside the mean. */
    uint64_t median_us;
    /* Fastest sample, in microseconds. Printed as the low end of the row. */
    uint64_t min_us;
    /* Slowest sample, in microseconds. Printed as the high end of the row. */
    uint64_t max_us;
} Bench;

/* One import-linking result recorded for the report.
   The test stores the image size and the exit code of the program that was produced. */
typedef struct {
    /* Label of the import case. The import table in the report prints it. */
    const char *name;
    /* Size of the produced image, copied from g_file_len after the file is loaded. */
    uint64_t bytes;
    /* Exit status observed when that image is run. The report prints it next to the size. */
    int exit_code;
} ImportRow;

/* One header-padding case: how big the image was and where the number pattern sat.
   find_needle supplies the offset that the report prints in hex. */
typedef struct {
    /* Label of the padding case. The padding table in the report prints it. */
    const char *name;
    /* Size of the produced image, copied from g_file_len. */
    uint64_t bytes;
    /* File offset of the 144-byte number pattern. The report prints it with app_hex. */
    uint32_t offset;
} PadRow;

static Fixture g_fix[4];
static Bench g_bench[4];
static ImportRow g_imports[4];
static PadRow g_pads[3];

static void fail(const char *text);
static void app(char *dst, size_t cap, size_t *len, const char *src);

/* Counts the bytes in a narrow NUL-terminated string, not including the terminator.
   It reads text and writes nothing but the returned length.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static size_t slen(const char *text) {
    size_t length = 0;
    /* Advance until the NUL that ends the string. */
    while (text[length] != 0) length++;
    return length;
}

/* Counts the wide characters in a NUL-terminated wchar_t string, not including the terminator.
   It reads text and writes nothing but the returned length.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static size_t wlen(const wchar_t *text) {
    size_t length = 0;
    /* Advance until the wide NUL that ends the string. */
    while (text[length] != 0) length++;
    return length;
}

/* Writes text to the standard-error handle and stops the process.
   It reads text and, when g_show_output is set, the captured child bytes in g_child, and it writes those bytes with WriteFile.
   This is the failure sink: failure calls fail(), which writes a message and ExitProcess(1). */
static void fail(const char *text) {
    DWORD wrote = 0;
    HANDLE err = GetStdHandle((DWORD)-12);
    const char *prefix = "link_tests: ";
    WriteFile(err, prefix, 12, &wrote, NULL);
    WriteFile(err, text, (DWORD)slen(text), &wrote, NULL);
    WriteFile(err, "\n", 1, &wrote, NULL);
    /* After the message, also dump captured tool output when that dump was requested. */
    if (g_show_output && g_child.output_length > 0) {
        const char *header = "---- tool output ----\n";
        WriteFile(err, header, (DWORD)slen(header), &wrote, NULL);
        WriteFile(err, g_child.output, (DWORD)g_child.output_length, &wrote, NULL);
        /* Add a newline when the captured output does not already end with one. */
        if (g_child.output[g_child.output_length - 1] != '\n') WriteFile(err, "\n", 1, &wrote, NULL);
    }
    ExitProcess(1);
}

/* Writes text to the standard-output handle and ignores a short write.
   It reads text and writes those bytes with WriteFile.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static void say(const char *text) {
    DWORD wrote = 0;
    HANDLE out = GetStdHandle((DWORD)-11);
    WriteFile(out, text, (DWORD)slen(text), &wrote, NULL);
}

/* Appends the NUL-terminated src onto dst at *len and keeps a terminator in dst.
   It reads src, cap, and *len, and it writes dst and *len.
   If the next byte would not fit, failure calls fail(), which writes a message and ExitProcess(1). */
static void app(char *dst, size_t cap, size_t *len, const char *src) {
    /* Copy source bytes until the terminator. */
    while (*src != 0) {
        /* Stop when the next byte plus a terminator would overrun dst. */
        if (*len + 1 >= cap) fail("internal buffer is full");
        dst[(*len)++] = *src++;
    }
    dst[*len] = 0;
}

/* Appends one character by turning it into a two-byte string and calling app.
   It reads ch, cap, and *len, and it writes dst and *len.
   If the destination cannot hold the character, failure calls fail(), which writes a message and ExitProcess(1). */
static void app_ch(char *dst, size_t cap, size_t *len, char ch) {
    char text[2];
    text[0] = ch;
    text[1] = 0;
    app(dst, cap, len, text);
}

/* Appends the decimal digits of an unsigned value, with a single zero when value is zero.
   It reads value and writes digits into dst through app.
   If the destination fills up, failure calls fail(), which writes a message and ExitProcess(1). */
static void app_u64(char *dst, size_t cap, size_t *len, uint64_t value) {
    char rev[32];
    char text[32];
    int count = 0;
    int index;
    /* Zero has no digits to peel off, so write a single 0. */
    if (value == 0) {
        app(dst, cap, len, "0");
        return;
    }
    /* Peel decimal digits from least significant to most. */
    while (value > 0 && count < 32) {
        rev[count++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    }
    /* Reverse the peeled digits into most-significant-first order. */
    for (index = 0; index < count; index++) text[index] = rev[count - 1 - index];
    text[count] = 0;
    app(dst, cap, len, text);
}

/* Appends a signed decimal value, with a leading minus when value is negative.
   It reads value and writes dst through app_ch and app_u64.
   If the destination fills up, failure calls fail(), which writes a message and ExitProcess(1). */
static void app_i64(char *dst, size_t cap, size_t *len, int64_t value) {
    /* Write a minus sign and then the absolute value. */
    if (value < 0) {
        app_ch(dst, cap, len, '-');
        app_u64(dst, cap, len, (uint64_t)(-value));
    /* Write a non-negative value with no sign character. */
    } else {
        app_u64(dst, cap, len, (uint64_t)value);
    }
}

/* Appends the tens and ones digits of value as exactly two characters.
   It reads value and writes dst through app_ch, so values past 99 wrap within two digits.
   If the destination fills up, failure calls fail(), which writes a message and ExitProcess(1). */
static void app_2(char *dst, size_t cap, size_t *len, unsigned value) {
    app_ch(dst, cap, len, (char)('0' + (value / 10u) % 10u));
    app_ch(dst, cap, len, (char)('0' + (value % 10u)));
}

/* Appends value as a 0x hexadecimal number, skipping leading zero nibbles.
   It reads value and writes dst through app and app_ch.
   If the destination fills up, failure calls fail(), which writes a message and ExitProcess(1). */
static void app_hex(char *dst, size_t cap, size_t *len, uint32_t value) {
    int shift;
    int started = 0;
    app(dst, cap, len, "0x");
    /* Zero is printed as 0x0 instead of an empty digit run. */
    if (value == 0) {
        app_ch(dst, cap, len, '0');
        return;
    }
    /* Emit one hex digit for each nibble from the high end. */
    for (shift = 28; shift >= 0; shift -= 4) {
        unsigned digit = (value >> shift) & 0xfu;
        /* Skip leading zero nibbles until the first nonzero digit. */
        if (!started && digit == 0) continue;
        started = 1;
        app_ch(dst, cap, len, (char)(digit < 10 ? '0' + digit : 'A' + (digit - 10)));
    }
}

/* Appends a microsecond count as milliseconds with three decimal places.
   It reads microseconds and writes dst through app_u64 and app_ch.
   If the destination fills up, failure calls fail(), which writes a message and ExitProcess(1). */
static void app_ms(char *dst, size_t cap, size_t *len, uint64_t microseconds) {
    uint64_t frac = microseconds % 1000ull;
    app_u64(dst, cap, len, microseconds / 1000ull);
    app_ch(dst, cap, len, '.');
    app_ch(dst, cap, len, (char)('0' + (frac / 100ull)));
    app_ch(dst, cap, len, (char)('0' + ((frac / 10ull) % 10ull)));
    app_ch(dst, cap, len, (char)('0' + (frac % 10ull)));
}

/* Empties the scratch failure note in g_note so the next message starts blank.
   It writes g_note_len and the first byte of g_note and reads nothing else.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static void note_clear(void) {
    g_note_len = 0;
    g_note[0] = 0;
}

/* Appends text to the scratch note g_note.
   It reads text and writes g_note and g_note_len through app.
   If the note buffer fills, failure calls fail(), which writes a message and ExitProcess(1). */
static void note_cat(const char *text) {
    app(g_note, sizeof g_note, &g_note_len, text);
}

/* Appends the decimal form of value to the scratch note g_note.
   It reads value and writes g_note through app_u64.
   If the note buffer fills, failure calls fail(), which writes a message and ExitProcess(1). */
static void note_u64(uint64_t value) {
    app_u64(g_note, sizeof g_note, &g_note_len, value);
}

/* Copies a wide string into dst, including the terminator.
   It reads src and cap and writes dst.
   If cap is zero or the source does not fit, failure calls fail(), which writes a message and ExitProcess(1). */
static void wcopy(wchar_t *dst, size_t cap, const wchar_t *src) {
    size_t index = 0;
    /* Refuse to write through a destination that cannot hold a terminator. */
    if (cap == 0) fail("path buffer is empty");
    /* Copy each wide character until the source terminator. */
    while (src[index] != 0) {
        /* Stop before a copy that would leave no room for the terminator. */
        if (index + 1 >= cap) fail("path is too long");
        dst[index] = src[index];
        index++;
    }
    dst[index] = 0;
}

/* Appends a wide string onto the wide string already stored in dst.
   It reads dst and src and writes the combined text back to dst.
   If the combined text does not fit in cap, failure calls fail(), which writes a message and ExitProcess(1). */
static void wcat(wchar_t *dst, size_t cap, const wchar_t *src) {
    size_t used = wlen(dst);
    size_t index = 0;
    /* Copy each source character onto the end of dst. */
    while (src[index] != 0) {
        /* Stop when the next character would drop the terminator. */
        if (used + 1 >= cap) fail("path or command is too long");
        dst[used++] = src[index++];
    }
    dst[used] = 0;
}

/* Copies a wide string into a narrow buffer, replacing non-ASCII characters with question marks.
   It reads src and cap and writes dst, and it returns without writing when cap is zero.
   It does not call fail(), which writes a message and ExitProcess(1). */
static void wide_to_ascii(char *dst, size_t cap, const wchar_t *src) {
    size_t index = 0;
    /* Leave the destination untouched when it has no room at all. */
    if (cap == 0) return;
    /* Copy each wide character that still fits, folding non-ASCII to a question mark. */
    while (src[index] != 0 && index + 1 < cap) {
        wchar_t ch = src[index];
        dst[index] = (ch < 128) ? (char)ch : '?';
        index++;
    }
    dst[index] = 0;
}

/* Converts a wide path or message to ASCII and appends it to g_note.
   It reads text, writes a stack buffer, and then writes g_note through note_cat.
   If the note buffer fills, failure calls fail(), which writes a message and ExitProcess(1). */
static void note_wide(const wchar_t *text) {
    char ascii[1024];
    wide_to_ascii(ascii, sizeof ascii, text);
    note_cat(ascii);
}

/* Clears the wide command line in g_cmd so the next command starts empty.
   It writes a NUL at g_cmd[0] and reads nothing.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static void cmd_reset(void) {
    g_cmd[0] = 0;
}

/* Appends text to the wide command buffer g_cmd.
   It reads text and writes g_cmd through wcat.
   If the command does not fit in 4096 wide characters, failure calls fail(), which writes a message and ExitProcess(1). */
static void cmd_append(const wchar_t *text) {
    wcat(g_cmd, 4096, text);
}

/* Appends one space to g_cmd so the next argument is separated.
   It writes g_cmd through wcat and reads no caller pointer.
   If the command buffer is full, failure calls fail(), which writes a message and ExitProcess(1). */
static void cmd_space(void) {
    wcat(g_cmd, 4096, L" ");
}

/* Wraps text in double quotes and appends that token to g_cmd.
   It reads text and writes g_cmd through three wcat calls.
   If the command buffer is full, failure calls fail(), which writes a message and ExitProcess(1). */
static void cmd_quote(const wchar_t *text) {
    wcat(g_cmd, 4096, L"\"");
    wcat(g_cmd, 4096, text);
    wcat(g_cmd, 4096, L"\"");
}

/* Builds a path under the repository root from g_root, a backslash, and relative.
   It reads g_root and relative and writes dst.
   If the path does not fit in 1024 wide characters, failure calls fail(), which writes a message and ExitProcess(1). */
static void under(wchar_t *dst, const wchar_t *relative) {
    wcopy(dst, 1024, g_root);
    wcat(dst, 1024, L"\\");
    wcat(dst, 1024, relative);
}

/* Removes the last path component by writing a NUL over the final slash.
   It reads path and writes the cut back into the same buffer.
   If there is no parent directory, failure calls fail(), which writes a message and ExitProcess(1). */
static void strip_last(wchar_t *path) {
    int index;
    int last = -1;
    /* Scan the path and remember the last slash. */
    for (index = 0; path[index] != 0; index++) {
        /* This character is a separator, so it is the cut point so far. */
        if (path[index] == L'\\' || path[index] == L'/') last = index;
    }
    /* A path with no parent cannot be walked up to the repository root. */
    if (last <= 0) fail("cannot locate the repository root from the executable path");
    path[last] = 0;
}

/* Creates the directory at path and treats an existing directory as success.
   It reads path and calls CreateDirectoryW, then GetLastError on failure.
   If creation fails for any other reason, failure calls fail(), which writes a message and ExitProcess(1). */
static void ensure_dir(const wchar_t *path) {
    /* A newly created directory is success. */
    if (CreateDirectoryW(path, NULL)) return;
    /* An existing directory is also success. */
    if (GetLastError() == ERROR_ALREADY_EXISTS) return;
    note_clear();
    note_cat("cannot create directory ");
    note_wide(path);
    fail(g_note);
}

/* Reads a little-endian uint16_t from the first two bytes at bytes.
   It reads those bytes and writes only the returned value.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static uint16_t ru16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

/* Reads a little-endian uint32_t from the first four bytes at bytes.
   It reads those bytes and writes only the returned value.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static uint32_t ru32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads the whole file at path into g_file and sets g_file_len to the byte count.
   It reads the file with CreateFileW, GetFileSizeEx, and ReadFile, and it writes g_file.
   If the file cannot be opened, sized, or fully read, failure calls fail(), which writes a message and ExitProcess(1). */
static void read_entire(const wchar_t *path) {
    HANDLE file;
    LARGE_INTEGER size;
    DWORD got_total = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    /* The open failed, so report the path and stop. */
    if (file == INVALID_HANDLE_VALUE) {
        note_clear();
        note_cat("cannot read ");
        note_wide(path);
        fail(g_note);
    }
    /* The size is missing, negative, or larger than the buffer, so close and stop. */
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart > g_file_cap) {
        CloseHandle(file);
        note_clear();
        note_cat("cannot size ");
        note_wide(path);
        fail(g_note);
    }
    /* Keep reading until every byte of the reported size is in g_file. */
    while ((uint64_t)got_total < (uint64_t)size.QuadPart) {
        DWORD got = 0;
        DWORD want = (DWORD)((uint64_t)size.QuadPart - got_total);
        /* Cap each ReadFile at one megabyte. */
        if (want > 1u << 20) want = 1u << 20;
        /* A failed or empty read is a short read, so close and stop. */
        if (!ReadFile(file, g_file + got_total, want, &got, NULL) || got == 0) {
            CloseHandle(file);
            note_clear();
            note_cat("short read of ");
            note_wide(path);
            fail(g_note);
        }
        got_total += got;
    }
    CloseHandle(file);
    g_file_len = got_total;
}

/* Creates or replaces the file at path and writes size bytes from data.
   It reads data and writes the file with CreateFileW and WriteFile.
   If the file cannot be created or the write is short, failure calls fail(), which writes a message and ExitProcess(1). */
static void write_entire(const wchar_t *path, const void *data, DWORD size) {
    HANDLE file;
    DWORD wrote = 0;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    /* The create failed, so report the path and stop. */
    if (file == INVALID_HANDLE_VALUE) {
        note_clear();
        note_cat("cannot write ");
        note_wide(path);
        fail(g_note);
    }
    /* A failed or short write is fatal, so close and stop. */
    if ((size > 0 && !WriteFile(file, data, size, &wrote, NULL)) || wrote != size) {
        CloseHandle(file);
        note_clear();
        note_cat("short write of ");
        note_wide(path);
        fail(g_note);
    }
    CloseHandle(file);
}

/* The integration cases that are not compiled with cl.exe are ordinary COFF
   objects. link_tests writes them while it runs so the repository does not
   store object files. */
#define COFF_TEXT 0x60500020u
#define COFF_DATA 0xC0500040u
#define COFF_RDATA 0x40500040u
#define COFF_REL_ADDR64 0x0001u
#define COFF_REL_ADDR32NB 0x0003u
#define COFF_REL_REL32 0x0004u
#define COFF_REL_SECREL 0x000Bu

/* One section the in-memory COFF writer can emit, including up to eight relocations.
   coff_section and coff_reloc fill it, and coff_write turns it into header, raw, and reloc bytes. */
typedef struct {
    /* Section name, at most eight bytes plus a spare NUL. Copied into the COFF section header. */
    char name[9];
    /* Section flags such as COFF_TEXT or COFF_DATA. Written at byte 36 of the section header. */
    uint32_t characteristics;
    /* Payload length in bytes. The writer stores it as both the virtual size and the raw size. */
    uint32_t size;
    /* Raw section payload, capped at 512 bytes. Copied into the object at the section raw offset. */
    uint8_t bytes[512];
    /* Byte offset of each relocation inside the section. Written as the first field of each reloc record. */
    uint32_t reloc_offset[8];
    /* Symbol-table index each relocation refers to. Written as the second field of each reloc record. */
    uint32_t reloc_symbol[8];
    /* Relocation type code such as REL32 or ADDR64. Written as the last field of each reloc record. */
    uint16_t reloc_type[8];
    /* How many slots in the three relocation arrays are live. The writer emits that many ten-byte records. */
    uint16_t reloc_count;
} CoffSection;

/* One COFF symbol, either a defined section symbol or an undefined import.
   coff_symbol fills it, and coff_write emits an eighteen-byte symbol record. */
typedef struct {
    /* Symbol name. Names of eight bytes or fewer stay inline; longer names move to the string table. */
    char name[32];
    /* Symbol value, usually a section offset. Written at byte 8 of the symbol record. */
    uint32_t value;
    /* One-based section number, or zero for an undefined symbol such as an import. */
    short section;
    /* COFF symbol type. The generator uses 0x20 for a function and 0 for data. */
    uint16_t type;
} CoffSymbol;

static CoffSection g_coff_sec[4];
static CoffSymbol g_coff_sym[8];
static int g_coff_sections;
static int g_coff_symbols;
static uint8_t g_coff[4096];

/* Stores value as a little-endian uint16_t at bytes.
   It reads value and writes two bytes.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static void coff_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

/* Stores value as a little-endian uint32_t at bytes.
   It reads value and writes four bytes.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static void coff_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/* Clears the in-memory section and symbol tables and zeroes their counts.
   It writes g_coff_sec and g_coff_sym through memset, then writes the two counts.
   It has no failure path, so it does not call fail(), which writes a message and ExitProcess(1). */
static void coff_reset(void) {
    /* memset zeroes every section record so names, sizes, and relocations start clear. */
    memset(g_coff_sec, 0, sizeof g_coff_sec);
    /* memset zeroes every symbol record so names and section numbers start clear. */
    memset(g_coff_sym, 0, sizeof g_coff_sym);
    g_coff_sections = 0;
    g_coff_symbols = 0;
}

/* Adds one section to g_coff_sec and returns its index.
   It reads name, characteristics, bytes, and size, and it writes the next CoffSection, copying the name with memcpy.
   If the table, the name, or the 512-byte payload cannot hold it, failure calls fail(), which writes a message and ExitProcess(1). */
static int coff_section(const char *name, uint32_t characteristics, const uint8_t *bytes, uint32_t size) {
    CoffSection *section;
    size_t length = slen(name);
    /* Reject a section the fixed tables cannot store. */
    if (g_coff_sections >= 4 || size > 512 || length == 0 || length > 8) fail("COFF section does not fit");
    section = &g_coff_sec[g_coff_sections];
    /* memcpy copies the section name into the zeroed nine-byte slot. */
    memcpy(section->name, name, length);
    section->characteristics = characteristics;
    section->size = size;
    /* memcpy copies the payload only when the section is non-empty. */
    if (size != 0) memcpy(section->bytes, bytes, size);
    return g_coff_sections++;
}

/* Records one relocation against a section that coff_section already added.
   It reads section, offset, symbol, and type, and it writes the next slot of that section's relocation arrays.
   If the section index is invalid or the section already has eight relocations, failure calls fail(), which writes a message and ExitProcess(1). */
static void coff_reloc(int section, uint32_t offset, int symbol, uint16_t type) {
    CoffSection *target;
    /* Reject a relocation aimed at a section that was not added. */
    if (section < 0 || section >= g_coff_sections) fail("COFF relocation section is invalid");
    target = &g_coff_sec[section];
    /* Reject a ninth relocation on this section. */
    if (target->reloc_count >= 8) fail("COFF relocation table is full");
    target->reloc_offset[target->reloc_count] = offset;
    target->reloc_symbol[target->reloc_count] = (uint32_t)symbol;
    target->reloc_type[target->reloc_count] = type;
    target->reloc_count++;
}

/* Adds one symbol to g_coff_sym and returns its index.
   It reads name, value, section, and type, and it writes the next CoffSymbol, copying the name with memcpy.
   If the table is full or the name is empty or longer than the slot, failure calls fail(), which writes a message and ExitProcess(1). */
static int coff_symbol(const char *name, uint32_t value, int section, uint16_t type) {
    CoffSymbol *symbol;
    size_t length = slen(name);
    /* Reject a symbol the table or the name slot cannot hold. */
    if (g_coff_symbols >= 8 || length == 0 || length >= sizeof g_coff_sym[0].name) fail("COFF symbol does not fit");
    symbol = &g_coff_sym[g_coff_symbols];
    /* memcpy copies the symbol name into the zeroed name slot. */
    memcpy(symbol->name, name, length);
    symbol->value = value;
    symbol->section = (short)section;
    symbol->type = type;
    return g_coff_symbols++;
}

/* Serializes the current sections and symbols into g_coff and writes that image to path.
   It reads g_coff_sec and g_coff_sym, writes g_coff with memset and memcpy, and writes the file through write_entire.
   If a string, the object image, or the file write does not fit, failure calls fail(), which writes a message and ExitProcess(1). */
static void coff_write(const wchar_t *path) {
    uint32_t string_at[8];
    uint8_t strings[256];
    uint32_t string_size = 4;
    uint32_t raw_at[4];
    uint32_t reloc_at[4];
    uint32_t cursor;
    uint32_t symbol_at;
    uint32_t string_base;
    int index;
    int reloc;
    /* memset zeroes the object image so unused header bytes stay zero. */
    memset(g_coff, 0, sizeof g_coff);
    /* memset zeroes the string table before long symbol names are copied in. */
    memset(strings, 0, sizeof strings);
    /* Place each long symbol name into the string table. */
    for (index = 0; index < g_coff_symbols; index++) {
        size_t length = slen(g_coff_sym[index].name);
        /* Names longer than eight bytes live in the string table. */
        if (length > 8) {
            /* Stop when the next name would overrun the fixed string buffer. */
            if (string_size + length + 1 > sizeof strings) fail("COFF string table is full");
            string_at[index] = string_size;
            /* memcpy copies the long name and its NUL into the string table. */
            memcpy(strings + string_size, g_coff_sym[index].name, length + 1);
            string_size += (uint32_t)length + 1u;
        /* A short name stays inline, so this symbol has no string-table offset. */
        } else {
            string_at[index] = 0;
        }
    }
    cursor = 20u + 40u * (uint32_t)g_coff_sections;
    /* Assign aligned raw and relocation offsets for every section. */
    for (index = 0; index < g_coff_sections; index++) {
        cursor = (cursor + 3u) & ~3u;
        /* Reserve file space for this section payload and remember its raw offset. */
        if (g_coff_sec[index].size != 0) {
            raw_at[index] = cursor;
            cursor += g_coff_sec[index].size;
        /* An empty section has no raw data pointer. */
        } else {
            raw_at[index] = 0;
        }
        /* Align and reserve ten bytes for each relocation record. */
        if (g_coff_sec[index].reloc_count != 0) {
            cursor = (cursor + 3u) & ~3u;
            reloc_at[index] = cursor;
            cursor += (uint32_t)g_coff_sec[index].reloc_count * 10u;
        /* A section with no relocations stores a zero relocation pointer. */
        } else {
            reloc_at[index] = 0;
        }
    }
    cursor = (cursor + 3u) & ~3u;
    symbol_at = cursor;
    cursor += (uint32_t)g_coff_symbols * 18u;
    string_base = cursor;
    cursor += string_size;
    /* Reject an image that does not fit in the object buffer. */
    if (cursor > sizeof g_coff) fail("COFF object is too large");
    coff_u16(g_coff, 0x8664);
    coff_u16(g_coff + 2, (uint16_t)g_coff_sections);
    coff_u32(g_coff + 8, symbol_at);
    coff_u32(g_coff + 12, (uint32_t)g_coff_symbols);
    /* Write each section header, its payload, and its relocation records. */
    for (index = 0; index < g_coff_sections; index++) {
        CoffSection *section = &g_coff_sec[index];
        uint8_t *header = g_coff + 20 + 40 * index;
        /* memcpy copies the section name into the eight-byte header field. */
        memcpy(header, section->name, slen(section->name));
        coff_u32(header + 8, section->size);
        coff_u32(header + 16, section->size);
        coff_u32(header + 20, raw_at[index]);
        coff_u32(header + 24, reloc_at[index]);
        coff_u16(header + 32, section->reloc_count);
        coff_u32(header + 36, section->characteristics);
        /* memcpy copies the section payload into the object at its raw offset. */
        if (section->size != 0) memcpy(g_coff + raw_at[index], section->bytes, section->size);
        /* Write one ten-byte relocation at this section's relocation offset. */
        for (reloc = 0; reloc < section->reloc_count; reloc++) {
            uint8_t *record = g_coff + reloc_at[index] + (uint32_t)reloc * 10u;
            coff_u32(record, section->reloc_offset[reloc]);
            coff_u32(record + 4, section->reloc_symbol[reloc]);
            coff_u16(record + 8, section->reloc_type[reloc]);
        }
    }
    /* Write each eighteen-byte symbol record. */
    for (index = 0; index < g_coff_symbols; index++) {
        uint8_t *record = g_coff + symbol_at + (uint32_t)index * 18u;
        size_t length = slen(g_coff_sym[index].name);
        /* A long name writes its string-table offset into the symbol name field. */
        if (string_at[index] != 0) coff_u32(record + 4, string_at[index]);
        /* memcpy copies a short name into the symbol's inline name bytes. */
        else memcpy(record, g_coff_sym[index].name, length);
        coff_u32(record + 8, g_coff_sym[index].value);
        coff_u16(record + 12, (uint16_t)g_coff_sym[index].section);
        coff_u16(record + 14, g_coff_sym[index].type);
        record[16] = 2;
    }
    coff_u32(g_coff + string_base, string_size);
    /* memcpy copies the string-table bytes that follow the four-byte length. */
    if (string_size > 4) memcpy(g_coff + string_base + 4, strings + 4, string_size - 4);
    write_entire(path, g_coff, cursor);
}

/* Builds the four freestanding objects the integration tests link instead of checked-in files.
   It writes return.obj, relocations.obj, direct_import.obj, and header_padding_sum.obj under out\test, and it prints a progress line.
   The helpers it calls use fail(), which writes a message and ExitProcess(1), if a COFF table or a path overflows. */
static void write_generated_objects(void) {
    static const uint8_t return_text[] = { 0xB8, 0x25, 0x00, 0x00, 0x00, 0xC3 };
    static const uint8_t reloc_text[] = {
        0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x00, 0xC3,
        0xE8, 0xF1, 0xFF, 0xFF, 0xFF, 0xB8, 0x17, 0x00, 0x00, 0x00, 0xC3
    };
    static const uint8_t reloc_data[] = {
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t import_text[] = {
        0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00, 0x00,
        0xB9, 0x49, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00, 0xCC
    };
    static const uint8_t pad_text[] = {
        0x48, 0x8D, 0x15, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC0,
        0xB9, 0x24, 0x00, 0x00, 0x00, 0x03, 0x02, 0x48, 0x83, 0xC2, 0x04,
        0xFF, 0xC9, 0x75, 0xF6, 0x3D, 0x9A, 0x02, 0x00, 0x00,
        0x0F, 0x95, 0xC0, 0x0F, 0xB6, 0xC0, 0xC3
    };
    uint8_t numbers[144];
    uint32_t number;
    int text;
    int data;
    say("generating objects\n");
    /* Fill thirty-six little-endian counters, one through thirty-six, into the padding payload. */
    for (number = 0; number < 36; number++) {
        numbers[number * 4] = (uint8_t)(number + 1);
        numbers[number * 4 + 1] = 0;
        numbers[number * 4 + 2] = 0;
        numbers[number * 4 + 3] = 0;
    }
    coff_reset();
    text = coff_section(".text", COFF_TEXT, return_text, (uint32_t)sizeof return_text);
    coff_symbol("start", 0, text + 1, 0x20);
    under(g_path_a, L"out\\test\\return.obj");
    coff_write(g_path_a);

    coff_reset();
    text = coff_section(".text", COFF_TEXT, reloc_text, (uint32_t)sizeof reloc_text);
    data = coff_section(".data", COFF_DATA, reloc_data, (uint32_t)sizeof reloc_data);
    coff_symbol("value", 0, data + 1, 0);
    coff_symbol("helper", 0, text + 1, 0x20);
    coff_symbol("start", 10, text + 1, 0x20);
    coff_reloc(text, 3, 0, COFF_REL_REL32);
    coff_reloc(data, 8, 0, COFF_REL_ADDR64);
    coff_reloc(data, 16, 0, COFF_REL_ADDR32NB);
    coff_reloc(data, 20, 0, COFF_REL_SECREL);
    under(g_path_a, L"out\\test\\relocations.obj");
    coff_write(g_path_a);

    coff_reset();
    text = coff_section(".text", COFF_TEXT, import_text, (uint32_t)sizeof import_text);
    coff_symbol("ExitProcess", 0, 0, 0x20);
    coff_symbol("GetCurrentProcessId", 0, 0, 0x20);
    coff_symbol("start", 0, text + 1, 0x20);
    coff_reloc(text, 5, 1, COFF_REL_REL32);
    coff_reloc(text, 15, 0, COFF_REL_REL32);
    under(g_path_a, L"out\\test\\direct_import.obj");
    coff_write(g_path_a);

    coff_reset();
    text = coff_section(".text", COFF_TEXT, pad_text, (uint32_t)sizeof pad_text);
    data = coff_section(".rdata", COFF_RDATA, numbers, (uint32_t)sizeof numbers);
    coff_symbol("numbers", 0, data + 1, 0);
    coff_symbol("start", 0, text + 1, 0x20);
    coff_reloc(text, 3, 0, COFF_REL_REL32);
    under(g_path_a, L"out\\test\\header_padding_sum.obj");
    coff_write(g_path_a);
}

/* Opens path and returns its length in bytes.
   It reads the file with CreateFileW and GetFileSizeEx into a LARGE_INTEGER cleared by memset.
   If the file cannot be opened or sized, failure calls fail(), which writes a message and ExitProcess(1). */
static uint64_t file_size(const wchar_t *path) {
    HANDLE file;
    LARGE_INTEGER size;
    /* memset clears the LARGE_INTEGER before GetFileSizeEx writes QuadPart. */
    memset(&size, 0, sizeof size);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    /* The open or the size query failed, so report the path. */
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        /* Close the handle when it was opened before the size check failed. */
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        note_clear();
        note_cat("cannot size ");
        note_wide(path);
        fail(g_note);
    }
    CloseHandle(file);
    return (uint64_t)size.QuadPart;
}

/* Finds the PE signature inside g_file and stores its offset in g_pe.
   It reads the DOS e_lfanew field and the bytes at that offset, and it writes g_pe.
   If the image is too small or is not a PE, failure calls fail(), which writes a message and ExitProcess(1), passing label. */
static void locate_pe(const char *label) {
    uint32_t pe;
    uint16_t opt;
    uint16_t sections;
    /* Reject an image too small to hold a DOS header. */
    if (g_file_len < 0x40) fail(label);
    pe = ru32(g_file + 0x3c);
    /* Reject an e_lfanew that leaves no room for the signature and file header. */
    if ((uint64_t)pe + 24 > g_file_len) fail(label);
    /* Reject a file whose e_lfanew does not point at a PE signature. */
    if (g_file[pe] != 'P' || g_file[pe + 1] != 'E' || g_file[pe + 2] != 0 || g_file[pe + 3] != 0) fail(label);
    sections = ru16(g_file + pe + 6);
    opt = ru16(g_file + pe + 20);
    /* Reject a header whose section table would run past the end of the file. */
    if ((uint64_t)pe + 24u + opt + (uint64_t)sections * 40u > g_file_len) fail(label);
    g_pe = pe;
}

/* Maps rva to a Span inside g_file, either in the headers or in a section's raw bytes.
   It reads g_file and g_pe and writes span when the mapping stays inside the file.
   A miss returns 0 and does not call fail(), which writes a message and ExitProcess(1). */
static int span_rva(uint32_t rva, Span *span) {
    uint32_t headers;
    uint16_t sections;
    uint16_t opt;
    uint32_t index;
    uint32_t table;
    headers = ru32(g_file + g_pe + 84);
    /* An RVA inside the headers is a file offset of the same value. */
    if (rva < headers && (uint64_t)rva < g_file_len && headers <= g_file_len) {
        span->ptr = g_file + rva;
        span->remaining = headers - rva;
        /* Clamp the header slice so it does not run past g_file. */
        if (span->remaining > g_file_len - rva) span->remaining = (uint32_t)(g_file_len - rva);
        return 1;
    }
    sections = ru16(g_file + g_pe + 6);
    opt = ru16(g_file + g_pe + 20);
    table = g_pe + 24u + opt;
    /* Search each section for an RVA inside its virtual range. */
    for (index = 0; index < sections; index++) {
        const uint8_t *sec = g_file + table + index * 40u;
        uint32_t va = ru32(sec + 12);
        uint32_t raw_size = ru32(sec + 16);
        uint32_t raw = ru32(sec + 20);
        /* This section owns the RVA, so convert it to a raw file offset. */
        if (rva >= va && rva - va < raw_size) {
            uint32_t delta = rva - va;
            /* The mapped byte is not inside the file. */
            if ((uint64_t)raw + delta >= g_file_len) return 0;
            span->ptr = g_file + raw + delta;
            span->remaining = raw_size - delta;
            /* Clamp the section slice to the bytes that are actually present. */
            if ((uint64_t)raw + delta + span->remaining > g_file_len) span->remaining = (uint32_t)(g_file_len - (raw + delta));
            return 1;
        }
    }
    return 0;
}

/* Compares span with a NUL-terminated text, ignoring ASCII case, and requires a NUL after the match.
   It reads span and text and writes nothing.
   A mismatch returns 0 and does not call fail(), which writes a message and ExitProcess(1). */
static int span_eq_i(Span span, const char *text) {
    uint32_t index = 0;
    /* Compare the next text byte with the span, case-insensitively. */
    while (text[index] != 0) {
        unsigned char left;
        unsigned char right;
        /* The span ended before the text did. */
        if (index >= span.remaining) return 0;
        left = span.ptr[index];
        right = (unsigned char)text[index];
        /* Fold an uppercase span byte to lowercase. */
        if (left >= 'A' && left <= 'Z') left = (unsigned char)(left - 'A' + 'a');
        /* Fold an uppercase text byte to lowercase. */
        if (right >= 'A' && right <= 'Z') right = (unsigned char)(right - 'A' + 'a');
        /* This character does not match. */
        if (left != right) return 0;
        index++;
    }
    return index < span.remaining && span.ptr[index] == 0;
}

/* Appends up to 180 bytes of span to g_note, replacing non-printable bytes with question marks.
   It reads span and writes g_note through app_ch, stopping at the first NUL inside the span.
   If the note buffer fills, failure calls fail(), which writes a message and ExitProcess(1). */
static void note_span(Span span) {
    uint32_t index;
    /* Append printable span bytes to the note, up to 180 characters. */
    for (index = 0; index < span.remaining && span.ptr[index] != 0 && index < 180; index++) {
        unsigned char ch = span.ptr[index];
        app_ch(g_note, sizeof g_note, &g_note_len, (ch >= 32 && ch < 127) ? (char)ch : '?');
    }
}

/* Checks the PE machine, optional-header magic, and subsystem against the expected values.
   It reads those three fields from g_file at g_pe and writes g_note only on a mismatch.
   On a mismatch, failure calls fail(), which writes a message and ExitProcess(1). */
static void assert_header(uint16_t machine, uint16_t magic, uint16_t subsystem, const char *label) {
    uint16_t actual_machine = ru16(g_file + g_pe + 4);
    uint16_t actual_magic = ru16(g_file + g_pe + 24);
    uint16_t actual_subsystem = ru16(g_file + g_pe + 92);
    /* The machine, magic, and subsystem already agree, so there is nothing to report. */
    if (actual_machine == machine && actual_magic == magic && actual_subsystem == subsystem) return;
    note_clear();
    note_cat(label);
    note_cat(" PE header mismatch: machine ");
    app_hex(g_note, sizeof g_note, &g_note_len, actual_machine);
    note_cat(" magic ");
    app_hex(g_note, sizeof g_note, &g_note_len, actual_magic);
    note_cat(" subsystem ");
    note_u64(actual_subsystem);
    fail(g_note);
}

/* Walks the import descriptors and rejects an image that does not import only kernel32.dll.
   It reads the data-directory count and import RVA in g_file, then each descriptor and DLL name.
   On a bad magic, a missing directory, or an unexpected DLL, failure calls fail(), which writes a message and ExitProcess(1). */
static void assert_only_kernel32(const char *label) {
    uint16_t magic = ru16(g_file + g_pe + 24);
    uint32_t directories = 0;
    uint32_t import_rva = 0;
    int count = 0;
    /* PE32+ stores the data-directory count at file offset g_pe plus 132. */
    if (magic == 0x20b) directories = ru32(g_file + g_pe + 132);
    /* PE32 stores that count at file offset g_pe plus 116. */
    else if (magic == 0x10b) directories = ru32(g_file + g_pe + 116);
    /* Any other optional-header magic is not a header this check accepts. */
    else fail(label);
    /* Fewer than two data directories means there is no import directory. */
    if (directories < 2) {
        note_clear();
        note_cat(label);
        note_cat(" has no import directory");
        fail(g_note);
    }
    import_rva = (magic == 0x20b) ? ru32(g_file + g_pe + 144) : ru32(g_file + g_pe + 128);
    /* A zero import RVA means the image imports nothing. */
    if (import_rva == 0) {
        note_clear();
        note_cat(label);
        note_cat(" imports nothing");
        fail(g_note);
    }
    /* Walk import descriptors until the null terminator. */
    for (;;) {
        Span desc;
        Span name;
        uint32_t name_rva;
        uint32_t original;
        uint32_t first;
        /* The descriptor is not fully inside the file. */
        if (!span_rva(import_rva, &desc) || desc.remaining < 20) {
            note_clear();
            note_cat(label);
            note_cat(" import descriptor is outside the file");
            fail(g_note);
        }
        original = ru32(desc.ptr);
        name_rva = ru32(desc.ptr + 12);
        first = ru32(desc.ptr + 16);
        /* The null descriptor ends the import table. */
        if (original == 0 && name_rva == 0 && first == 0) break;
        /* This descriptor names some DLL other than kernel32.dll, or the name is unreadable. */
        if (!span_rva(name_rva, &name) || !span_eq_i(name, "kernel32.dll")) {
            note_clear();
            note_cat(label);
            note_cat(" imports ");
            /* The DLL name is readable, so append it to the note. */
            if (span_rva(name_rva, &name)) note_span(name);
            /* The DLL name could not be mapped, so say it is unreadable. */
            else note_cat("an unreadable DLL");
            fail(g_note);
        }
        count++;
        import_rva += 20;
        /* Stop a corrupt table that never reaches a null descriptor. */
        if (count > 64) fail(label);
    }
    /* The table ended without a single kernel32.dll descriptor. */
    if (count < 1) {
        note_clear();
        note_cat(label);
        note_cat(" imports nothing");
        fail(g_note);
    }
}

/* Searches g_file for needle with a case-insensitive byte compare.
   It reads needle and g_file and writes nothing.
   A miss returns 0 and does not call fail(), which writes a message and ExitProcess(1). */
static int contains_i(const char *needle) {
    size_t needle_len = slen(needle);
    size_t index;
    size_t cursor;
    /* An empty needle or one longer than the file cannot match. */
    if (needle_len == 0 || needle_len > g_file_len) return 0;
    /* Try each file offset that could start a full match. */
    for (index = 0; index + needle_len <= g_file_len; index++) {
        /* Compare the needle at this offset, ignoring ASCII case. */
        for (cursor = 0; cursor < needle_len; cursor++) {
            unsigned char left = g_file[index + cursor];
            unsigned char right = (unsigned char)needle[cursor];
            /* Fold an uppercase file byte to lowercase. */
            if (left >= 'A' && left <= 'Z') left = (unsigned char)(left - 'A' + 'a');
            /* Fold an uppercase needle byte to lowercase. */
            if (right >= 'A' && right <= 'Z') right = (unsigned char)(right - 'A' + 'a');
            /* This offset does not match, so try the next one. */
            if (left != right) break;
        }
        /* Every needle byte matched. */
        if (cursor == needle_len) return 1;
    }
    return 0;
}

/* Requires kernel32.dll and four import names to appear somewhere in g_file.
   It reads those names through contains_i and writes g_note when one is missing.
   If any name is absent, failure calls fail(), which writes a message and ExitProcess(1). */
static void require_bytes(const char *label) {
    const char *names[5];
    int index;
    names[0] = "kernel32.dll";
    names[1] = "ExitProcess";
    names[2] = "GetCurrentProcessId";
    names[3] = "GetTickCount";
    names[4] = "GetLastError";
    /* Check each required import name. */
    for (index = 0; index < 5; index++) {
        /* This name is present, so move on. */
        if (contains_i(names[index])) continue;
        note_clear();
        note_cat(label);
        note_cat(" is missing ");
        note_cat(names[index]);
        fail(g_note);
    }
}

/* Searches g_file for the 144-byte pattern in g_needle and stores the file offset.
   It reads g_file and g_needle and writes *offset when every byte matches.
   A miss returns 0 and does not call fail(), which writes a message and ExitProcess(1). */
static int find_needle(uint32_t *offset) {
    size_t index;
    /* The pattern cannot fit in this file. */
    if (g_file_len < 144) return 0;
    /* Try each offset that could hold all 144 pattern bytes. */
    for (index = 0; index + 144 <= g_file_len; index++) {
        size_t cursor;
        /* Compare the pattern one byte at a time. */
        for (cursor = 0; cursor < 144; cursor++) {
            /* This offset is not the pattern. */
            if (g_file[index + cursor] != g_needle[cursor]) break;
        }
        /* Every pattern byte matched, so record the offset. */
        if (cursor == 144) {
            *offset = (uint32_t)index;
            return 1;
        }
    }
    return 0;
}

/* Pulls bytes already buffered in pipe into g_child.output until the pipe has no more.
   It reads the pipe with PeekNamedPipe and ReadFile and writes g_child.output and g_child.output_length.
   A closed or empty pipe returns and does not call fail(), which writes a message and ExitProcess(1). */
static void drain_pipe(HANDLE pipe) {
    /* Keep taking bytes the pipe has already buffered. */
    for (;;) {
        DWORD available = 0;
        DWORD got = 0;
        char temp[512];
        size_t index;
        size_t keep;
        /* The pipe is closed or unusable, so stop draining. */
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) return;
        /* Nothing is buffered, so stop until the caller waits again. */
        if (available == 0) return;
        /* Read at most one stack buffer on this pass. */
        if (available > sizeof temp) available = (DWORD)sizeof temp;
        /* The read delivered nothing, so stop. */
        if (!ReadFile(pipe, temp, available, &got, NULL) || got == 0) return;
        keep = got;
        /* Drop these bytes once the capture buffer has no room for a terminator. */
        if (g_child.output_length + 1 >= sizeof g_child.output) keep = 0;
        /* Keep only the bytes that still fit before the terminator. */
        else if (g_child.output_length + keep >= sizeof g_child.output) keep = sizeof g_child.output - 1 - g_child.output_length;
        /* Append the kept bytes to the child output. */
        for (index = 0; index < keep; index++) g_child.output[g_child.output_length++] = temp[index];
        g_child.output[g_child.output_length] = 0;
    }
}

/* Starts the wide command in g_cmd as a hidden child and waits up to timeout_ms milliseconds.
 * It copies the command to g_cmd_run, inherits handles, and records started, timed_out, exit_code, and win_error in g_child. When capture is nonzero, stdout and stderr are appended to g_child.output.
 * Pipe or process creation failure returns 0. A failed check in the caller calls fail() or fail_child(), which calls ExitProcess(1).
 */
static int run_child(int capture, DWORD timeout_ms) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION info;
    SECURITY_ATTRIBUTES security;
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    DWORD started;

    memset(&g_child, 0, sizeof g_child);
    memset(&startup, 0, sizeof startup);
    memset(&info, 0, sizeof info);
    wcopy(g_cmd_run, 4096, g_cmd);
    security.nLength = (DWORD)sizeof security;
    security.lpSecurityDescriptor = NULL;
    security.bInheritHandle = 1;
    /* Capture stdout and stderr through a pipe when the caller asked for the child output. */
    if (capture) {
        /* Return 0 and store the Win32 error when the capture pipe cannot be created. */
        if (!CreatePipe(&read_pipe, &write_pipe, &security, 65536)) {
            g_child.win_error = GetLastError();
            return 0;
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        startup.hStdInput = g_nul;
        startup.hStdOutput = write_pipe;
        startup.hStdError = write_pipe;
    /* Point stdin, stdout, and stderr at the NUL device when output is not captured. */
    } else {
        startup.hStdInput = g_nul;
        startup.hStdOutput = g_nul;
        startup.hStdError = g_nul;
    }
    startup.cb = (DWORD)sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    /* Record the Win32 error, close any pipe ends, and return 0 when the process cannot start. */
    if (!CreateProcessW(NULL, g_cmd_run, NULL, NULL, 1, CREATE_NO_WINDOW, NULL, g_root, &startup, &info)) {
        g_child.win_error = GetLastError();
        /* Close the pipe write end if it was opened before CreateProcessW failed. */
        if (write_pipe != NULL) CloseHandle(write_pipe);
        /* Close the pipe read end if it was opened before CreateProcessW failed. */
        if (read_pipe != NULL) CloseHandle(read_pipe);
        return 0;
    }
    g_child.started = 1;
    /* Close the parent's pipe write end so the child, which inherited it, can finish its output. */
    if (write_pipe != NULL) {
        CloseHandle(write_pipe);
        write_pipe = NULL;
    }
    CloseHandle(info.hThread);
    started = GetTickCount();
    /* Poll the child every 50 milliseconds until it exits or timeout_ms elapses. */
    for (;;) {
        DWORD wait;
        /* Drain captured output before each wait so a full pipe cannot stall the child. */
        if (capture) drain_pipe(read_pipe);
        wait = WaitForSingleObject(info.hProcess, 50);
        /* Leave the poll once the process object is signaled. */
        if (wait == WAIT_OBJECT_0) {
            /* Drain any output still queued after the process has exited. */
            if (capture) drain_pipe(read_pipe);
            break;
        }
        /* Kill the child, mark g_child.timed_out, and leave the poll when the deadline has passed. */
        if ((DWORD)(GetTickCount() - started) >= timeout_ms) {
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, 5000);
            g_child.timed_out = 1;
            break;
        }
    }
    /* Store exit code 1 when the process exit status cannot be read. */
    if (!GetExitCodeProcess(info.hProcess, &g_child.exit_code)) g_child.exit_code = 1;
    CloseHandle(info.hProcess);
    /* Close the captured-output read end when a pipe was opened. */
    if (read_pipe != NULL) CloseHandle(read_pipe);
    return 1;
}

/* Turns on child-output display and fails the test with text.
 * It sets g_show_output so fail() writes the captured g_child.output to stderr with the message.
 * A failed check calls fail(), which calls ExitProcess(1).
 */
static void fail_child(const char *text) {
    g_show_output = 1;
    fail(text);
}

/* Rewrites g_note as a one-line explanation of why the child named by what did not succeed.
 * It reads g_child.started, g_child.win_error, g_child.timed_out, and g_child.exit_code and appends the matching phrase.
 * A failed check in the caller calls fail_child(), which calls ExitProcess(1).
 */
static void describe_child(const char *what) {
    note_clear();
    note_cat(what);
    /* Describe a process that never started, including the Win32 error. */
    if (!g_child.started) {
        note_cat(" could not start (error ");
        note_u64(g_child.win_error);
        note_cat(")");
    /* Describe a process that started but was killed for running past the timeout. */
    } else if (g_child.timed_out) {
        note_cat(" timed out");
    /* Describe a process that started and finished, including its exit code. */
    } else {
        note_cat(" exited ");
        note_u64(g_child.exit_code);
    }
}

/* Runs the command in g_cmd, captures its output, and accepts only a process that starts, finishes, and exits 0.
 * It passes timeout_ms to run_child and uses what as the label when the run is not acceptable.
 * A failed check calls fail_child(), which calls ExitProcess(1).
 */
static void require_ok(DWORD timeout_ms, const char *what) {
    /* Describe the child and fail when it did not start, timed out, or returned nonzero. */
    if (!run_child(1, timeout_ms) || !g_child.started || g_child.timed_out || g_child.exit_code != 0) {
        describe_child(what);
        fail_child(g_note);
    }
}

/* Runs the command in g_cmd, captures its output, and accepts only a process that starts, finishes, and exits nonzero.
 * It uses timeout_ms as the wait limit and what as the label for a start failure, a timeout, or an unexpected zero exit.
 * A failed check calls fail_child(), which calls ExitProcess(1).
 */
static void require_nonzero(DWORD timeout_ms, const char *what) {
    /* Describe the child and fail when it did not start or timed out. */
    if (!run_child(1, timeout_ms) || !g_child.started || g_child.timed_out) {
        describe_child(what);
        fail_child(g_note);
    }
    /* Fail when the tool accepted the input and returned a zero exit code. */
    if (g_child.exit_code == 0) {
        note_clear();
        note_cat(what);
        note_cat(" was accepted");
        fail_child(g_note);
    }
}

/* Runs exe with an empty argument list and no captured output, and accepts only exit code expected.
 * On a mismatch it runs exe again with capture, writes the actual and expected codes into g_note, and fails using what as the label.
 * A failed check calls fail_child(), which calls ExitProcess(1).
 */
static void require_exit(const wchar_t *exe, DWORD expected, const char *what) {
    cmd_reset();
    cmd_quote(exe);
    /* Rerun with captured output and fail when the exit code is not the expected value. */
    if (!run_child(0, 10000) || !g_child.started || g_child.timed_out || g_child.exit_code != expected) {
        cmd_reset();
        cmd_quote(exe);
        run_child(1, 10000);
        note_clear();
        note_cat(what);
        note_cat(" returned ");
        note_u64(g_child.exit_code);
        note_cat("; expected ");
        note_u64(expected);
        fail_child(g_note);
    }
}

/* Replaces g_cmd with a small_linker invocation that links object to exe.
 * It quotes g_small, object, and exe, and appends tail only when tail is nonempty.
 * A failed check in the command-buffer helpers calls fail(), which calls ExitProcess(1).
 */
static void small_cmd(const wchar_t *object, const wchar_t *exe, const wchar_t *tail) {
    cmd_reset();
    cmd_quote(g_small);
    cmd_space();
    cmd_quote(object);
    cmd_space();
    cmd_append(L"/out:");
    cmd_quote(exe);
    /* Append a separating space and tail when the caller passed a nonempty extra argument. */
    if (tail != NULL && tail[0] != 0) {
        cmd_space();
        cmd_append(tail);
    }
}

/* Replaces g_cmd with a Visual Studio link.exe invocation for machine that writes exe from object.
 * It quotes linker, exe, and object and appends the fixed nologo, nodefaultlib, entry, console, align, and filealign switches.
 * A failed check in the command-buffer helpers calls fail(), which calls ExitProcess(1).
 */
static void vs_cmd(const wchar_t *linker, const wchar_t *machine, const wchar_t *object, const wchar_t *exe) {
    cmd_reset();
    cmd_quote(linker);
    cmd_space();
    cmd_append(L"/nologo /nodefaultlib /entry:start /subsystem:console /machine:");
    cmd_append(machine);
    cmd_append(L" /align:4096 /filealign:512 /out:");
    cmd_quote(exe);
    cmd_space();
    cmd_quote(object);
}

/* Replaces g_cmd with a cl.exe invocation that compiles source to object.
 * It quotes compiler, object, and source, appends the fixed compile-only flags, and includes define_text when it is not null.
 * A failed check in the command-buffer helpers calls fail(), which calls ExitProcess(1).
 */
static void compile_cmd(const wchar_t *compiler, const wchar_t *define_text, const wchar_t *object, const wchar_t *source) {
    cmd_reset();
    cmd_quote(compiler);
    cmd_space();
    cmd_append(L"/nologo /c /O2 /GS- /Zl ");
    /* Append define_text and a space when a preprocessor definition string was provided. */
    if (define_text != NULL) {
        cmd_append(define_text);
        cmd_space();
    }
    cmd_append(L"/Fo");
    cmd_quote(object);
    cmd_space();
    cmd_quote(source);
}

/* Loads the image at path and checks that it is a PE32+ x64 console program importing only kernel32.dll.
 * It fills the file buffer, locates the PE header, and checks machine 0x8664, magic 0x20b, and subsystem 3 under label.
 * A failed check calls fail(), which calls ExitProcess(1).
 */
static void check_tool_image(const wchar_t *path, const char *label) {
    read_entire(path);
    locate_pe(label);
    assert_header(0x8664, 0x20b, 3, label);
    assert_only_kernel32(label);
}

/* Copies src into dst and rewrites the last x64 tool-directory component so the path names the x86 sibling.
 * It scans dst for a slash, the characters x64, and a following slash, then replaces those two digits with 86.
 * A failed check calls fail(), which calls ExitProcess(1), when that directory marker is missing.
 */
static void to_x86_tool(wchar_t *dst, const wchar_t *src) {
    int index;
    int last = -1;
    int length;
    wcopy(dst, 1024, src);
    length = (int)wlen(dst);
    /* Scan each position that could begin a slash, x64, slash directory marker. */
    for (index = 0; index + 4 < length; index++) {
        wchar_t mark = dst[index];
        wchar_t a = dst[index + 1];
        wchar_t b = dst[index + 2];
        wchar_t c = dst[index + 3];
        wchar_t d = dst[index + 4];
        int slash = (mark == L'\\' || mark == L'/');
        int ex = (a == L'x' || a == L'X');
        int end = (d == L'\\' || d == L'/');
        /* Remember the x of the latest x64 directory component. */
        if (slash && ex && b == L'6' && c == L'4' && end) last = index + 1;
    }
    /* Fail when the copied path never contained an x64 tool directory. */
    if (last < 0) fail("cl.exe is not under a Hostx64\\x64 directory, so the x86 tools cannot be located");
    dst[last + 1] = L'8';
    dst[last + 2] = L'6';
}

/* Searches PATH for name and writes the full path into dst, which has room for 1024 wide characters.
 * SearchPathW supplies the length. missing is the message used when the tool is absent or the path does not fit.
 * A failed check calls fail(), which calls ExitProcess(1).
 */
static void find_tool(wchar_t *dst, const wchar_t *name, const char *missing) {
    DWORD found = SearchPathW(NULL, name, NULL, 1024, dst, NULL);
    /* Fail when the tool is missing or its full path does not fit in dst. */
    if (found == 0 || found >= 1024) fail(missing);
}

/* Reads the current performance-counter tick and returns it as an unsigned 64-bit count.
 * It writes a local LARGE_INTEGER and returns QuadPart. Callers convert deltas with g_qpc_freq.
 * A failed check calls fail(), which calls ExitProcess(1).
 */
static uint64_t qpc_now(void) {
    LARGE_INTEGER now;
    /* Fail when the performance counter cannot be read. */
    if (!QueryPerformanceCounter(&now)) fail("QueryPerformanceCounter failed");
    return (uint64_t)now.QuadPart;
}

/* Strips trailing spaces, tabs, carriage returns, and line feeds from text by moving the terminator left.
 * It reads the current length and writes a zero over each trailing whitespace character.
 * This helper does not itself exit. A failed check in the caller calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void trim_inplace(char *text) {
    size_t used = slen(text);
    /* Walk backward from the end of text while trailing whitespace remains. */
    while (used > 0) {
        char ch = text[used - 1];
        /* Stop trimming at the last character that is not space, tab, or a newline. */
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        used--;
        text[used] = 0;
    }
}

/* Queries a string registry value into dst, converts it to ASCII, and trims trailing whitespace.
 * It reads key and value through g_reg_query, accepts REG_SZ or REG_EXPAND_SZ without expanding it, and returns 1 only when dst is nonempty.
 * A bad query returns 0. A failed check in the caller calls fail() or fail_child(), which calls ExitProcess(1).
 */
static int reg_string(void *key, const wchar_t *value, char *dst, size_t cap) {
    wchar_t wide[512];
    DWORD bytes = (DWORD)(sizeof wide - sizeof(wchar_t));
    DWORD type = 0;
    long status = g_reg_query(key, value, NULL, &type, (unsigned char *)wide, &bytes);
    /* Reject the value when the query fails or the type is not a registry string. */
    if (status != 0 || (type != 1 && type != 2)) return 0;
    /* Reject the value when the byte count is odd or does not fit in the wide buffer. */
    if ((bytes & 1u) != 0 || bytes > sizeof wide - sizeof(wchar_t)) return 0;
    wide[bytes / sizeof(wchar_t)] = 0;
    wide_to_ascii(dst, cap, wide);
    trim_inplace(dst);
    return dst[0] != 0;
}

/* Queries a REG_DWORD registry value and writes the 32-bit data through out.
 * It reads key and value through g_reg_query and returns 1 only when the type and size are a 4-byte DWORD.
 * A bad query returns 0. A failed check in the caller calls fail() or fail_child(), which calls ExitProcess(1).
 */
static int reg_dword(void *key, const wchar_t *value, DWORD *out) {
    DWORD data = 0;
    DWORD bytes = 4;
    DWORD type = 0;
    long status = g_reg_query(key, value, NULL, &type, (unsigned char *)&data, &bytes);
    /* Reject the value when the query fails or the data is not a 4-byte DWORD. */
    if (status != 0 || type != 4 || bytes != 4) return 0;
    *out = data;
    return 1;
}

/* Loads advapi32.dll and stores RegOpenKeyExW, RegQueryValueExW, and RegCloseKey in the g_reg function pointers.
 * If the module or any export is missing, it leaves those pointers null so later registry reads are skipped.
 * This loader does not itself exit. A failed check elsewhere calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void load_registry(void) {
    void *module = LoadLibraryW(L"advapi32.dll");
    /* Leave the registry pointers unset when advapi32.dll cannot be loaded. */
    if (module == NULL) return;
    g_reg_open = (reg_open_fn)GetProcAddress(module, "RegOpenKeyExW");
    g_reg_query = (reg_query_fn)GetProcAddress(module, "RegQueryValueExW");
    g_reg_close = (reg_close_fn)GetProcAddress(module, "RegCloseKey");
    /* Clear every registry pointer when any of the three exports is missing. */
    if (g_reg_open == NULL || g_reg_query == NULL || g_reg_close == NULL) {
        g_reg_open = NULL;
        g_reg_query = NULL;
        g_reg_close = NULL;
    }
}

/* Fills g_os, g_cpu, g_cpus, and g_when from the system, the registry, and the local time zone.
 * It defaults the OS and CPU text, reads the processor count, then when the registry API is loaded copies the product name, version, build, and processor name, and formats the local timestamp with a numeric UTC offset.
 * A failed check in the text helpers calls fail(), which calls ExitProcess(1).
 */
static void read_environment(void) {
    void *root;
    void *key = NULL;
    char product[256];
    char build[64];
    DWORD major = 0;
    DWORD minor = 0;
    int got_product = 0;
    int got_build = 0;
    int got_major = 0;
    SYSTEM_INFO info;
    SYSTEMTIME local;
    TIME_ZONE_INFORMATION zone;
    DWORD kind;
    long bias;
    int offset;
    int absolute;
    char sign;

    {
        size_t len = 0;
        app(g_os, sizeof g_os, &len, "Windows");
    }
    {
        size_t len = 0;
        app(g_cpu, sizeof g_cpu, &len, "unknown");
    }
    memset(&info, 0, sizeof info);
    GetSystemInfo(&info);
    g_cpus = info.dwNumberOfProcessors;
    /* Read the OS and CPU strings from the registry when the API was loaded. */
    if (g_reg_open != NULL) {
        root = (void *)(intptr_t)(long)0x80000002;
        /* Query the Windows NT current-version key when that key opens. */
        if (g_reg_open(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, 0x20019, &key) == 0 && key != NULL) {
            got_product = reg_string(key, L"ProductName", product, sizeof product);
            got_major = reg_dword(key, L"CurrentMajorVersionNumber", &major);
            reg_dword(key, L"CurrentMinorVersionNumber", &minor);
            got_build = reg_string(key, L"CurrentBuild", build, sizeof build);
            /* Fall back to CurrentBuildNumber when CurrentBuild was not a usable string. */
            if (!got_build) got_build = reg_string(key, L"CurrentBuildNumber", build, sizeof build);
            g_reg_close(key);
            key = NULL;
            /* Replace the default OS text when a product name was read. */
            if (got_product) {
                size_t len = 0;
                app(g_os, sizeof g_os, &len, product);
                /* Append major.minor.build when both the major version and a build string exist. */
                if (got_major && got_build) {
                    app(g_os, sizeof g_os, &len, ", ");
                    app_u64(g_os, sizeof g_os, &len, major);
                    app(g_os, sizeof g_os, &len, ".");
                    app_u64(g_os, sizeof g_os, &len, minor);
                    app(g_os, sizeof g_os, &len, ".");
                    app(g_os, sizeof g_os, &len, build);
                }
            }
        }
        /* Query the first processor key when that key opens. */
        if (g_reg_open(root, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, 0x20019, &key) == 0 && key != NULL) {
            char cpu[320];
            /* Replace the default CPU text when ProcessorNameString is present. */
            if (reg_string(key, L"ProcessorNameString", cpu, sizeof cpu)) {
                size_t len = 0;
                app(g_cpu, sizeof g_cpu, &len, cpu);
            }
            g_reg_close(key);
        }
    }
    GetLocalTime(&local);
    memset(&zone, 0, sizeof zone);
    kind = GetTimeZoneInformation(&zone);
    bias = zone.Bias;
    /* Add the daylight bias when the current zone is on daylight time. */
    if (kind == 2) bias += zone.DaylightBias;
    /* Add the standard bias when the current zone is on standard time. */
    else if (kind == 1) bias += zone.StandardBias;
    offset = -(int)bias;
    sign = (offset < 0) ? '-' : '+';
    absolute = (offset < 0) ? -offset : offset;
    {
        size_t len = 0;
        app_u64(g_when, sizeof g_when, &len, local.wYear);
        app_ch(g_when, sizeof g_when, &len, '-');
        app_2(g_when, sizeof g_when, &len, local.wMonth);
        app_ch(g_when, sizeof g_when, &len, '-');
        app_2(g_when, sizeof g_when, &len, local.wDay);
        app(g_when, sizeof g_when, &len, " ");
        app_2(g_when, sizeof g_when, &len, local.wHour);
        app_ch(g_when, sizeof g_when, &len, ':');
        app_2(g_when, sizeof g_when, &len, local.wMinute);
        app_ch(g_when, sizeof g_when, &len, ':');
        app_2(g_when, sizeof g_when, &len, local.wSecond);
        app_ch(g_when, sizeof g_when, &len, ' ');
        app_ch(g_when, sizeof g_when, &len, sign);
        app_2(g_when, sizeof g_when, &len, (unsigned)(absolute / 60));
        app_ch(g_when, sizeof g_when, &len, ':');
        app_2(g_when, sizeof g_when, &len, (unsigned)(absolute % 60));
    }
}

/* Copies the first nonempty line of g_child.output into dst, skipping angle-bracket spans, and trims it.
 * It reads g_child.output_length. A zero cap writes nothing. An empty trimmed line is replaced with the word unknown.
 * This helper does not itself exit. A failed check in the caller calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void first_line(char *dst, size_t cap) {
    size_t in = 0;
    size_t out = 0;
    int skipping = 0;
    /* Leave dst untouched when the caller passed a zero capacity. */
    if (cap == 0) return;
    /* Skip leading spaces, tabs, and newlines in the captured child output. */
    while (in < g_child.output_length) {
        char ch = g_child.output[in];
        /* Stop skipping once a character that is not whitespace is reached. */
        if (ch != '\r' && ch != '\n' && ch != ' ' && ch != '\t') break;
        in++;
    }
    /* Copy the first line into dst until the line ends or the buffer is full. */
    while (in < g_child.output_length && out + 1 < cap) {
        char ch = g_child.output[in++];
        /* Stop the copy at the first carriage return or line feed. */
        if (ch == '\r' || ch == '\n') break;
        /* Start skipping angle-bracket markup when a less-than sign is seen. */
        if (ch == '<') {
            skipping = 1;
            continue;
        }
        /* Stop skipping angle-bracket markup when a greater-than sign is seen. */
        if (ch == '>') {
            skipping = 0;
            continue;
        }
        /* Store the character when it is outside an angle-bracket span. */
        if (!skipping) dst[out++] = ch;
    }
    dst[out] = 0;
    trim_inplace(dst);
    /* Write the word unknown when the trimmed line is empty. */
    if (dst[0] == 0) {
        dst[0] = 'u';
        dst[1] = 'n';
        dst[2] = 'k';
        dst[3] = 'n';
        dst[4] = 'o';
        dst[5] = 'w';
        dst[6] = 'n';
        dst[7] = 0;
    }
}

/* Runs tool with no arguments, captures its output, and stores the first useful line in dst.
 * It rewrites g_cmd with the quoted tool path. If the process cannot be started, it writes unknown into dst instead.
 * A failed check in the command helpers calls fail(), which calls ExitProcess(1).
 */
static void capture_version(const wchar_t *tool, char *dst, size_t cap) {
    cmd_reset();
    cmd_quote(tool);
    dst[0] = 0;
    /* Store unknown and return when the tool process cannot be started. */
    if (!run_child(1, 60000) || !g_child.started) {
        size_t len = 0;
        app(dst, cap, &len, "unknown");
        return;
    }
    first_line(dst, cap);
}

/* Fills g_needle with the 144-byte sequence of little-endian integers 1 through 36.
 * Header-packing tests later search linked images for those bytes and record the file offset.
 * This initializer does not itself exit. A failed check in the caller calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void init_needle(void) {
    uint32_t number;
    /* Store each integer from 1 through 36 as a little-endian 32-bit word. */
    for (number = 1; number <= 36; number++) {
        g_needle[(number - 1) * 4] = (uint8_t)number;
        g_needle[(number - 1) * 4 + 1] = 0;
        g_needle[(number - 1) * 4 + 2] = 0;
        g_needle[(number - 1) * 4 + 3] = 0;
    }
}

/* Resolves the repository root from this executable and prepares output paths, the NUL handle, the file buffer, and the timer.
 * It writes g_exe, g_root, g_small, g_report_path, g_path_a, g_nul, g_file, g_file_cap, and g_qpc_freq, and creates the test output directory.
 * A failed check calls fail(), which calls ExitProcess(1).
 */
static void init_paths(void) {
    DWORD length;
    SECURITY_ATTRIBUTES security;
    LARGE_INTEGER freq;
    length = GetModuleFileNameW(NULL, g_exe, 1024);
    /* Fail when the executable path is missing or does not fit in g_exe. */
    if (length == 0 || length >= 1024) fail("cannot read the executable path");
    wcopy(g_root, 1024, g_exe);
    strip_last(g_root);
    strip_last(g_root);
    under(g_small, L"out\\small_linker.exe");
    under(g_report_path, L"out\\BENCHMARK_REPORT.md");
    under(g_path_a, L"out\\test");
    ensure_dir(g_path_a);
    /* Fail when the built small_linker.exe is not on disk. */
    if (GetFileAttributesW(g_small) == INVALID_FILE_ATTRIBUTES) fail("out\\small_linker.exe is missing");
    security.nLength = (DWORD)sizeof security;
    security.lpSecurityDescriptor = NULL;
    security.bInheritHandle = 1;
    g_nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
    /* Fail when the NUL device cannot be opened for inherited standard handles. */
    if (g_nul == INVALID_HANDLE_VALUE) fail("cannot open NUL");
    g_file_cap = 8u << 20;
    g_file = (uint8_t *)VirtualAlloc(NULL, g_file_cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    /* Fail when the file buffer cannot be reserved. */
    if (g_file == NULL) fail("cannot allocate the file buffer");
    /* Fail when the performance-counter frequency is unavailable or not positive. */
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) fail("QueryPerformanceFrequency failed");
    g_qpc_freq = (uint64_t)freq.QuadPart;
}

/* Stores one benchmark fixture in g_fix at index, including names, the expected PE identity, and the paths.
 * It copies object and resolves small_rel and vs_rel under the repository root into that fixture's executable paths.
 * A failed check in the path helpers calls fail(), which calls ExitProcess(1).
 */
static void setup_fixture(int index, const char *name, const char *producer, int exit_code, uint16_t machine, uint16_t magic, int is_x86, const wchar_t *object, const wchar_t *small_rel, const wchar_t *vs_rel) {
    Fixture *fixture = &g_fix[index];
    fixture->name = name;
    fixture->producer = producer;
    fixture->exit_code = exit_code;
    fixture->machine = machine;
    fixture->magic = magic;
    fixture->is_x86 = is_x86;
    wcopy(fixture->object, 1024, object);
    under(fixture->small_exe, small_rel);
    under(fixture->vs_exe, vs_rel);
}

/* Links fixture with small_linker and with link.exe, runs both images, and checks the small_linker PE header.
 * It reads the fixture object, machine, magic, x86 flag, and expected exit code, then writes both executables and stores their sizes on the fixture.
 * A failed check calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void link_pair(Fixture *fixture) {
    const wchar_t *linker = fixture->is_x86 ? g_link_x86 : g_link;
    const wchar_t *machine = fixture->is_x86 ? L"x86" : L"x64";
    say("linking ");
    say(fixture->name);
    say("\n");
    small_cmd(fixture->object, fixture->small_exe, L"/entry:start /subsystem:console");
    require_ok(60000, fixture->name);
    vs_cmd(linker, machine, fixture->object, fixture->vs_exe);
    require_ok(60000, fixture->name);
    require_exit(fixture->small_exe, (DWORD)fixture->exit_code, fixture->name);
    require_exit(fixture->vs_exe, (DWORD)fixture->exit_code, fixture->name);
    read_entire(fixture->small_exe);
    locate_pe(fixture->name);
    assert_header(fixture->machine, fixture->magic, 3, fixture->name);
    fixture->small_size = file_size(fixture->small_exe);
    fixture->vs_size = file_size(fixture->vs_exe);
}

/* Links object with small_linker and tail, and demands that the link fail with a nonzero exit.
 * It deletes the expected-failure image first, writes that path into g_path_b, and uses what as the failure label.
 * A failed check calls fail_child(), which calls ExitProcess(1).
 */
static void reject_link(const wchar_t *object, const wchar_t *tail, const char *what) {
    under(g_path_b, L"out\\test\\expected_failure.exe");
    DeleteFileW(g_path_b);
    small_cmd(object, g_path_b, tail);
    require_nonzero(60000, what);
}

/* Times one run of the command already stored in g_cmd and returns the elapsed microseconds.
 * It reads the performance counter around an uncaptured child. If that child does not start and exit 0, it reruns the command with capture and fails.
 * A failed check calls fail_child(), which calls ExitProcess(1).
 */
static uint64_t time_link(void) {
    uint64_t start;
    uint64_t stop;
    start = qpc_now();
    /* Rerun with captured output and fail when the timed command does not exit 0. */
    if (!run_child(0, 60000) || !g_child.started || g_child.timed_out || g_child.exit_code != 0) {
        run_child(1, 60000);
        describe_child("benchmark invocation");
        fail_child(g_note);
    }
    stop = qpc_now();
    return ((stop - start) * 1000000ull) / g_qpc_freq;
}

/* Sorts values into ascending order with an insertion sort over the BENCH_ITERS samples.
 * It reads and writes that array in place so the caller can take the minimum, median, and maximum from the ordered samples.
 * This sort does not itself exit. A failed check in the caller calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void sort_samples(uint64_t *values) {
    int index;
    /* Insert each later sample into the already sorted prefix. */
    for (index = 1; index < BENCH_ITERS; index++) {
        uint64_t value = values[index];
        int cursor = index;
        /* Shift the previous larger sample one slot right until the insertion hole is found. */
        while (cursor > 0 && values[cursor - 1] > value) {
            values[cursor] = values[cursor - 1];
            cursor--;
        }
        values[cursor] = value;
    }
}

/* Times one linker command for fixture, after a warm-up run, and stores the summary in g_bench at index.
 * It selects small_linker or link.exe with use_small, writes name, and fills samples, min_us, max_us, median_us, and mean_us from BENCH_ITERS timed runs.
 * A failed check calls fail_child(), which calls ExitProcess(1).
 */
static void benchmark_row(int index, const char *name, int use_small, Fixture *fixture) {
    int sample;
    uint64_t sorted[BENCH_ITERS];
    uint64_t sum = 0;
    Bench *bench = &g_bench[index];
    say("timing ");
    say(name);
    say("\n");
    bench->name = name;
    /* Build a small_linker command when this row times small_linker. */
    if (use_small) small_cmd(fixture->object, fixture->small_exe, L"/entry:start /subsystem:console");
    /* Build the Visual Studio x64 link command when this row times link.exe. */
    else vs_cmd(g_link, L"x64", fixture->object, fixture->vs_exe);
    /* Rerun with captured output and fail when the warm-up link does not exit 0. */
    if (!run_child(0, 60000) || !g_child.started || g_child.timed_out || g_child.exit_code != 0) {
        run_child(1, 60000);
        describe_child("benchmark warm-up");
        fail_child(g_note);
    }
    /* Delete the output image, time one link, and add that sample into the running sum. */
    for (sample = 0; sample < BENCH_ITERS; sample++) {
        DeleteFileW(use_small ? fixture->small_exe : fixture->vs_exe);
        bench->samples[sample] = time_link();
        sum += bench->samples[sample];
    }
    memcpy(sorted, bench->samples, sizeof sorted);
    sort_samples(sorted);
    bench->min_us = sorted[0];
    bench->max_us = sorted[BENCH_ITERS - 1];
    bench->median_us = (sorted[BENCH_ITERS / 2 - 1] + sorted[BENCH_ITERS / 2]) / 2ull;
    bench->mean_us = (sum + (BENCH_ITERS / 2)) / BENCH_ITERS;
}

/* Appends text to the markdown report buffer g_report and advances g_report_len.
 * It writes through the shared app helper, which keeps the buffer terminated.
 * A failed check calls fail(), which calls ExitProcess(1), if the report buffer is full.
 */
static void report_cat(const char *text) {
    app(g_report, sizeof g_report, &g_report_len, text);
}

/* Appends a signed byte difference and a one-decimal percent of baseline to g_report, then ends the table row.
 * It writes a leading plus for a positive difference, n/a when baseline is zero, and a rounded tenth of a percent otherwise.
 * A failed check calls fail(), which calls ExitProcess(1), if the report buffer is full.
 */
static void report_diff(int64_t difference, uint64_t baseline) {
    uint64_t magnitude;
    uint64_t tenths;
    /* Write a leading plus when the byte difference is positive. */
    if (difference > 0) report_cat("+");
    {
        size_t len = g_report_len;
        app_i64(g_report, sizeof g_report, &len, difference);
        g_report_len = len;
    }
    report_cat(" | ");
    /* Write n/a and stop when the baseline size is zero so the percent is not divided. */
    if (baseline == 0) {
        report_cat("n/a |\n");
        return;
    }
    magnitude = (difference < 0) ? (uint64_t)(-difference) : (uint64_t)difference;
    tenths = (magnitude * 1000ull + baseline / 2ull) / baseline;
    /* Write a zero percent and stop when the rounded tenth of a percent is zero. */
    if (tenths == 0) {
        report_cat("0.0% |\n");
        return;
    }
    /* Write a leading minus when the percentage is negative. */
    if (difference < 0) report_cat("-");
    /* Write a leading plus when the percentage is positive. */
    else report_cat("+");
    {
        size_t len = g_report_len;
        app_u64(g_report, sizeof g_report, &len, tenths / 10ull);
        app_ch(g_report, sizeof g_report, &len, '.');
        app_ch(g_report, sizeof g_report, &len, (char)('0' + (tenths % 10ull)));
        g_report_len = len;
    }
    report_cat("% |\n");
}

/* Builds the markdown test and performance report in g_report and writes it to g_report_path.
 * It reads g_when, g_os, g_cpu, g_cpus, the tool version strings, g_fix, g_bench, g_imports, and g_pads, including every raw timing sample.
 * A failed check calls fail(), which calls ExitProcess(1).
 */
static void write_report(void) {
    int index;
    int sample;
    report_cat("# small_linker test and performance report\n\n");
    report_cat("Generated: ");
    report_cat(g_when);
    report_cat("\n\n## Environment\n\n");
    report_cat("- OS: ");
    report_cat(g_os);
    report_cat("\n- CPU: ");
    report_cat(g_cpu);
    report_cat("\n- Logical processors: ");
    {
        size_t len = g_report_len;
        app_u64(g_report, sizeof g_report, &len, g_cpus);
        g_report_len = len;
    }
    report_cat("\n- Compiler: ");
    report_cat(g_cl_ver);
    report_cat("\n- Visual Studio linker: ");
    report_cat(g_link_ver);
    report_cat("\n- Benchmark iterations: 30 after one warm-up invocation\n\n");
    report_cat("## Correctness\n\n");
    report_cat("| Fixture | Producer | Entry | Expected exit | small_linker | link.exe |\n");
    report_cat("|---|---|---:|---:|---:|---:|\n");
    /* Emit one correctness-table row for each of the four fixtures. */
    for (index = 0; index < 4; index++) {
        report_cat("| ");
        report_cat(g_fix[index].name);
        report_cat(" | ");
        report_cat(g_fix[index].producer);
        report_cat(" | `start` | ");
        {
            size_t len = g_report_len;
            app_i64(g_report, sizeof g_report, &len, g_fix[index].exit_code);
            g_report_len = len;
        }
        report_cat(" | pass | pass |\n");
    }
    report_cat("\nPE32/x86 and PE32+/x64 outputs passed, console output was selectable, and the default output subsystem was Windows GUI.\n\n");
    report_cat("Negative tests passed: unresolved external, missing entry symbol, truncated input, and unsupported-machine objects were all rejected.\n\n");
    report_cat("## Executable size\n\n");
    report_cat("| Fixture | small_linker bytes | link.exe bytes | Difference | Difference % |\n");
    report_cat("|---|---:|---:|---:|---:|\n");
    /* Emit one size-table row, including the signed difference, for each fixture. */
    for (index = 0; index < 4; index++) {
        int64_t difference = (int64_t)g_fix[index].small_size - (int64_t)g_fix[index].vs_size;
        report_cat("| ");
        report_cat(g_fix[index].name);
        report_cat(" | ");
        {
            size_t len = g_report_len;
            app_u64(g_report, sizeof g_report, &len, g_fix[index].small_size);
            app(g_report, sizeof g_report, &len, " | ");
            app_u64(g_report, sizeof g_report, &len, g_fix[index].vs_size);
            app(g_report, sizeof g_report, &len, " | ");
            g_report_len = len;
        }
        report_diff(difference, g_fix[index].vs_size);
    }
    report_cat("\n## Linker runtime\n\n");
    report_cat("Wall-clock measurements include process startup and output-file creation.\n\n");
    report_cat("| Input / linker | Mean ms | Median ms | Min ms | Max ms |\n");
    report_cat("|---|---:|---:|---:|---:|\n");
    /* Emit one runtime-table row from each of the four benchmark summaries. */
    for (index = 0; index < 4; index++) {
        size_t len;
        report_cat("| ");
        report_cat(g_bench[index].name);
        report_cat(" | ");
        len = g_report_len;
        app_ms(g_report, sizeof g_report, &len, g_bench[index].mean_us);
        app(g_report, sizeof g_report, &len, " | ");
        app_ms(g_report, sizeof g_report, &len, g_bench[index].median_us);
        app(g_report, sizeof g_report, &len, " | ");
        app_ms(g_report, sizeof g_report, &len, g_bench[index].min_us);
        app(g_report, sizeof g_report, &len, " | ");
        app_ms(g_report, sizeof g_report, &len, g_bench[index].max_us);
        app(g_report, sizeof g_report, &len, " |\n");
        g_report_len = len;
    }
    report_cat("\n## Method and interpretation\n\n");
    report_cat("Both linkers consumed the exact same object for each benchmark comparison. Visual Studio link.exe used `/NODEFAULTLIB /ENTRY:start /SUBSYSTEM:CONSOLE /MACHINE:X64 /ALIGN:4096 /FILEALIGN:512`. small_linker used `/entry:start /subsystem:console` and its fixed matching PE layout settings. Output was redirected during timing.\n\n");
    report_cat("The benchmark measures normal command-line latency, not only linking CPU time. Results are specific to this machine and run; antivirus, scheduler activity, filesystem cache state, and process startup introduce noise. Median is the preferred comparison statistic. Raw samples are retained below for auditability.\n\n");
    report_cat("## Imports\n\n");
    report_cat("| Fixture | File bytes | Exit code |\n");
    report_cat("|---|---:|---:|\n");
    /* Emit one import-table row for each recorded import image. */
    for (index = 0; index < 4; index++) {
        size_t len;
        report_cat("| ");
        report_cat(g_imports[index].name);
        report_cat(" | ");
        len = g_report_len;
        app_u64(g_report, sizeof g_report, &len, g_imports[index].bytes);
        app(g_report, sizeof g_report, &len, " | ");
        app_i64(g_report, sizeof g_report, &len, g_imports[index].exit_code);
        app(g_report, sizeof g_report, &len, " |\n");
        g_report_len = len;
    }
    report_cat("\nEach image contains kernel32.dll, ExitProcess, GetCurrentProcessId, GetTickCount, and GetLastError. Undeclared and malformed import options were rejected.\n\n");
    report_cat("## Header packing\n\n");
    report_cat("| Linker | File bytes | Data offset |\n");
    report_cat("|---|---:|---:|\n");
    /* Emit one header-packing row for each recorded pad image. */
    for (index = 0; index < 3; index++) {
        size_t len;
        report_cat("| ");
        report_cat(g_pads[index].name);
        report_cat(" | ");
        len = g_report_len;
        app_u64(g_report, sizeof g_report, &len, g_pads[index].bytes);
        app(g_report, sizeof g_report, &len, " | ");
        app_hex(g_report, sizeof g_report, &len, g_pads[index].offset);
        app(g_report, sizeof g_report, &len, " |\n");
        g_report_len = len;
    }
    report_cat("\nThe 144-byte sequence of little-endian values 1 through 36 is present in each image. Each program exited 0.\n\n");
    report_cat("## Raw timing samples (ms)\n\n");
    /* Emit one bullet of raw samples for each benchmark row. */
    for (index = 0; index < 4; index++) {
        report_cat("- **");
        report_cat(g_bench[index].name);
        report_cat(":** ");
        /* Append every timed sample, in milliseconds, for this benchmark row. */
        for (sample = 0; sample < BENCH_ITERS; sample++) {
            size_t len = g_report_len;
            /* Separate samples with a comma after the first one. */
            if (sample > 0) app(g_report, sizeof g_report, &len, ", ");
            app_ms(g_report, sizeof g_report, &len, g_bench[index].samples[sample]);
            g_report_len = len;
        }
        report_cat("\n");
    }
    write_entire(g_report_path, g_report, (DWORD)g_report_len);
}

/* Links object with the kernel32 import list, runs the image, and records one g_imports row at index.
 * It resolves exe_rel, demands exit code exit_code, checks that the import names are present in the file bytes, and stores name, exit_code, and the file length.
 * A failed check calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void run_import(int index, const char *name, const wchar_t *object, int exit_code, const wchar_t *exe_rel) {
    wchar_t exe[1024];
    say("import ");
    say(name);
    say("\n");
    under(exe, exe_rel);
    small_cmd(object, exe, L"/entry:start /subsystem:console /import:kernel32.dll,ExitProcess,GetCurrentProcessId /import:kernel32.dll,GetTickCount,GetLastError");
    require_ok(60000, name);
    require_exit(exe, (DWORD)exit_code, name);
    read_entire(exe);
    require_bytes(name);
    g_imports[index].name = name;
    g_imports[index].exit_code = exit_code;
    g_imports[index].bytes = g_file_len;
}

/* Links object, runs the image, and records the file size and the offset of the 1-through-36 sequence in g_pads at index.
 * When use_vs is nonzero it invokes link.exe; otherwise it invokes small_linker with tail. The image must exit 0.
 * A failed check calls fail() or fail_child(), which calls ExitProcess(1).
 */
static void run_pad(int index, const char *name, const wchar_t *object, const wchar_t *exe, const wchar_t *tail, int use_vs) {
    uint32_t offset = 0;
    say(name);
    say("\n");
    /* Build a Visual Studio link command when this row compares link.exe. */
    if (use_vs) vs_cmd(g_link, L"x64", object, exe);
    /* Build a small_linker command, including tail, when this row does not use link.exe. */
    else small_cmd(object, exe, tail);
    require_ok(60000, name);
    require_exit(exe, 0, name);
    read_entire(exe);
    /* Fail when the 1-through-36 byte sequence is not present in the linked image. */
    if (!find_needle(&offset)) {
        note_clear();
        note_cat("number sequence was not found in ");
        note_cat(name);
        fail(g_note);
    }
    g_pads[index].name = name;
    g_pads[index].bytes = g_file_len;
    g_pads[index].offset = offset;
}

/* How far commit_stack_pages walks. 180 chunks of 2500 bytes is about half of
 * the default 1 MiB reserve: enough for the locals in this file, and inside
 * the reserve so the probe never steps off the stack.
 */
#define STACK_PROBE_STEPS 180
/* Each step is smaller than one 4096-byte page, so the touch lands on the
 * guard page instead of past it.
 */
#define STACK_PROBE_CHUNK 2500

/* commit_stack_pages touches the stack one chunk at a time so each touch lands on the guard page.
 * It reads stepsLeft and writes two volatile bytes in a local array of STACK_PROBE_CHUNK bytes, then calls itself until one step remains.
 * This build links /NODEFAULTLIB and compiles with /Gs262144, so there is no __chkstk and the compiler does not probe a frame smaller than 256 KiB.
 * Windows starts the thread with one committed page and a guard page below it. A local larger than the gap down to that guard is allocated in one step and can skip it.
 * The pages stay committed after this returns. The function does not call fail() and does not return early.
 */
static void commit_stack_pages(int stepsLeft) {
    volatile unsigned char page[STACK_PROBE_CHUNK];
    /* Touch the low end of this frame so the guard page commits before the next step. */
    page[0] = (unsigned char)stepsLeft;
    /* Walk one chunk farther. The call keeps this frame on the stack. */
    if (stepsLeft > 1) commit_stack_pages(stepsLeft - 1);
    /* Touch the high end after the call so the compiler cannot drop the array or turn the recursion into a loop that never walks downward. */
    page[STACK_PROBE_CHUNK - 1] = page[0];
}

/* Process entry for the link test driver. It commits stack pages, prepares tools and fixtures, runs the correctness and negative tests, times four links, and writes the report.
 * commit_stack_pages runs first, while this frame is still small, because this build has no __chkstk.
 * It reads cl.exe and link.exe from PATH, emits the assembled objects, compiles the Visual C objects, and writes the executables plus g_fix, g_imports, g_pads, g_bench, and the report file.
 * A failed check calls fail() or fail_child(), which calls ExitProcess(1). When every check has passed it calls ExitProcess(0).
 */
void mainCRTStartup(void) {
    int index;
    /* Commit the stack while this frame is only the call itself. */
    commit_stack_pages(STACK_PROBE_STEPS);
    init_needle();
    init_paths();
    say("link_tests\n");
    find_tool(g_cl, L"cl.exe", "cl.exe was not found. Run build.bat test so the Visual Studio environment is configured.");
    find_tool(g_link, L"link.exe", "link.exe was not found. Run build.bat test so the Visual Studio environment is configured.");
    to_x86_tool(g_cl_x86, g_cl);
    to_x86_tool(g_link_x86, g_link);
    /* Fail when the Hostx64 x86 compiler path does not exist. */
    if (GetFileAttributesW(g_cl_x86) == INVALID_FILE_ATTRIBUTES) fail("the Visual Studio Hostx64\\x86 compiler was not found");
    /* Fail when the Hostx64 x86 linker path does not exist. */
    if (GetFileAttributesW(g_link_x86) == INVALID_FILE_ATTRIBUTES) fail("the Visual Studio Hostx64\\x86 linker was not found");
    check_tool_image(g_small, "small_linker.exe");
    check_tool_image(g_exe, "link_tests.exe");
    write_generated_objects();

    say("compiling objects\n");
    under(g_path_a, L"tests\\cl_exit.c");
    under(g_path_b, L"out\\test\\cl_exit.obj");
    compile_cmd(g_cl, NULL, g_path_b, g_path_a);
    require_ok(60000, "cl.exe cl_exit.c");
    wcopy(g_path_c, 1024, g_path_b);
    under(g_path_b, L"out\\test\\cl_exit_x86.obj");
    compile_cmd(g_cl_x86, NULL, g_path_b, g_path_a);
    require_ok(60000, "x86 cl.exe cl_exit.c");
    under(g_path_a, L"out\\test\\return.obj");
    setup_fixture(0, "Return", "link_tests.c", 37, 0x8664, 0x20b, 0, g_path_a, L"out\\test\\return-small.exe", L"out\\test\\return-vs.exe");
    setup_fixture(1, "Visual C x64", "cl.exe", 42, 0x8664, 0x20b, 0, g_path_c, L"out\\test\\visual_c_x64-small.exe", L"out\\test\\visual_c_x64-vs.exe");
    wcopy(g_path_c, 1024, g_path_b);
    setup_fixture(2, "Visual C x86", "cl.exe", 42, 0x014c, 0x10b, 1, g_path_c, L"out\\test\\visual_c_x86-small.exe", L"out\\test\\visual_c_x86-vs.exe");
    under(g_path_a, L"out\\test\\relocations.obj");
    setup_fixture(3, "Relocations", "link_tests.c", 23, 0x8664, 0x20b, 0, g_path_a, L"out\\test\\relocations-small.exe", L"out\\test\\relocations-vs.exe");
    /* Link and check each of the four prepared fixtures. */
    for (index = 0; index < 4; index++) link_pair(&g_fix[index]);

    say("checking default subsystem\n");
    under(g_path_b, L"out\\test\\windows-default.exe");
    small_cmd(g_fix[1].object, g_path_b, L"/entry:start");
    require_ok(60000, "default subsystem");
    read_entire(g_path_b);
    locate_pe("default subsystem");
    assert_header(0x8664, 0x20b, 2, "default subsystem");

    say("checking rejected inputs\n");
    under(g_path_a, L"tests\\app_state_sim.c");
    under(g_path_b, L"out\\test\\unresolved.obj");
    compile_cmd(g_cl, L"/DUNRESOLVED_TEST", g_path_b, g_path_a);
    require_ok(60000, "cl.exe unresolved");
    reject_link(g_path_b, NULL, "unresolved external");
    reject_link(g_fix[0].object, L"/entry:not_present", "missing entry");
    under(g_path_b, L"out\\test\\invalid.obj");
    write_entire(g_path_b, "\x01\x02\x03\x04", 4);
    reject_link(g_path_b, NULL, "truncated input");
    read_entire(g_fix[0].object);
    /* Fail when the return object is too short to patch its machine field. */
    if (g_file_len < 2) fail("masm fixture is too small to patch");
    g_file[0] = 0x64;
    g_file[1] = 0xaa;
    under(g_path_b, L"out\\test\\wrong_machine.obj");
    write_entire(g_path_b, g_file, (DWORD)g_file_len);
    reject_link(g_path_b, NULL, "unsupported machine");

    say("checking imports\n");
    under(g_path_a, L"tests\\app_state_sim.c");
    under(g_path_b, L"out\\test\\import_cl.obj");
    compile_cmd(g_cl, L"/DIMPORT_TEST", g_path_b, g_path_a);
    require_ok(60000, "cl.exe import");
    wcopy(g_path_c, 1024, g_path_b);
    under(g_path_b, L"out\\test\\import_cl_x86.obj");
    compile_cmd(g_cl_x86, L"/DIMPORT_TEST", g_path_b, g_path_a);
    require_ok(60000, "x86 cl.exe import");
    under(g_path_a, L"out\\test\\direct_import.obj");
    run_import(0, "Direct call", g_path_a, 73, L"out\\test\\direct_call-import.exe");
    run_import(1, "Visual C x64 __imp_", g_path_c, 74, L"out\\test\\visual_c_x64_imp-import.exe");
    wcopy(g_path_c, 1024, g_path_b);
    run_import(2, "Visual C x86 __imp_ stdcall", g_path_c, 74, L"out\\test\\visual_c_x86_imp-import.exe");
    under(g_path_a, L"tests\\app_state_sim.c");
    under(g_path_b, L"out\\test\\import_direct_x86.obj");
    compile_cmd(g_cl_x86, L"/DDIRECT_IMPORT_TEST", g_path_b, g_path_a);
    require_ok(60000, "x86 cl.exe direct import");
    run_import(3, "Visual C x86 direct stdcall", g_path_b, 75, L"out\\test\\visual_c_x86_direct-import.exe");
    under(g_path_a, L"out\\test\\direct_import.obj");
    reject_link(g_path_a, L"/entry:start /subsystem:console", "undeclared import");
    reject_link(g_path_a, L"/entry:start /subsystem:console /import:bad", "malformed import");

    say("checking packed headers\n");
    under(g_path_a, L"out\\test\\header_padding_sum.obj");
    under(g_path_b, L"out\\test\\padding-packed.exe");
    run_pad(0, "small_linker /pack-headers", g_path_a, g_path_b, L"/entry:start /subsystem:console /pack-headers", 0);
    under(g_path_b, L"out\\test\\padding-normal.exe");
    run_pad(1, "small_linker normal", g_path_a, g_path_b, L"/entry:start /subsystem:console", 0);
    under(g_path_b, L"out\\test\\padding-vs.exe");
    run_pad(2, "Visual Studio link.exe", g_path_a, g_path_b, NULL, 1);

    benchmark_row(0, "Return / small_linker", 1, &g_fix[0]);
    benchmark_row(1, "Return / link.exe", 0, &g_fix[0]);
    benchmark_row(2, "Visual C x64 / small_linker", 1, &g_fix[1]);
    benchmark_row(3, "Visual C x64 / link.exe", 0, &g_fix[1]);

    say("writing report\n");
    load_registry();
    read_environment();
    capture_version(g_cl, g_cl_ver, sizeof g_cl_ver);
    capture_version(g_link, g_link_ver, sizeof g_link_ver);
    write_report();
    say("All functional and negative tests passed.\nReport written to ");
    wide_to_ascii(g_note, sizeof g_note, g_report_path);
    say(g_note);
    say("\n");
    ExitProcess(0);
}
