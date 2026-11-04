/* small_linker links one I386 or AMD64 COFF object into a compact PE executable.
   The program is freestanding: it does not use the C runtime and its only
   import is kernel32.dll. Run with --self-test to check the linker.
   A rejected object prints the diagnostic and exits. The self-test runs those
   cases in a separate process so this process can keep going. */

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed __int64 int64_t;
typedef unsigned __int64 size_t;
typedef unsigned char bool;
typedef unsigned short wchar_t;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;

#define true 1
#define false 0
#define NULL ((void *)0)
#define UINT16_MAX 0xffffU
#define UINT32_MAX 0xffffffffU
#define SIZE_MAX 0xffffffffffffffffULL
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_C(value) value##U
#define UINT64_C(value) value##ULL

#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001u
#define OPEN_EXISTING 3u
#define CREATE_ALWAYS 2u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define PAGE_READWRITE 0x04u

/* LARGE_INTEGER is the Win32 signed 64-bit integer GetFileSizeEx fills in.
   The linker treats QuadPart as a file length in bytes and rejects a negative value before reading the object.
   This layout matches the single-member Win32 struct so the system call can write it directly. */
typedef struct {
    /* QuadPart stores a signed byte count, here the size of the input file.
       GetFileSizeEx writes it, and the loader rejects values below zero or above 64 MiB before allocate copies the file. */
    long long QuadPart;
} LARGE_INTEGER;

extern void ExitProcess(DWORD code);
extern HANDLE GetStdHandle(DWORD standard_handle);
extern BOOL WriteFile(HANDLE file, const void *buffer, DWORD bytes, DWORD *written, void *overlapped);
extern HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE template_file);
extern BOOL ReadFile(HANDLE file, void *buffer, DWORD bytes, DWORD *read_count, void *overlapped);
extern BOOL CloseHandle(HANDLE handle);
extern BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size);
extern void *VirtualAlloc(void *address, size_t size, DWORD type, DWORD protect);
extern const wchar_t *GetCommandLineW(void);
extern DWORD GetModuleFileNameW(HANDLE module, wchar_t *filename, DWORD size);
extern DWORD GetLastError(void);

// TODO: these fields should have comprehensive documentation.
static char g_error_text[1024];
static uint8_t *g_arena = NULL;
static size_t g_arena_used = 0;
static size_t g_arena_cap = 0;

/* memset fills destination with the low eight bits of value and returns destination.
   It writes size bytes and does not read the previous contents of the buffer.
   It does not call fail() and has no error return; the caller must pass a writable region. */
void *memset(void *destination, int value, size_t size) {
    unsigned char *bytes = (unsigned char *)destination;
    size_t index;
    /* Store the fill byte at every index in the destination. */
    for (index = 0; index < size; index++) bytes[index] = (unsigned char)value;
    return destination;
}

/* memcpy copies size bytes from source to destination and returns destination.
   It reads the source buffer and writes the same bytes in order, including any embedded NULs.
   It does not call fail() and has no error return; the ranges must not need special overlap handling. */
void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *out_bytes = (unsigned char *)destination;
    const unsigned char *in_bytes = (const unsigned char *)source;
    size_t index;
    /* Copy one byte per index from the source into the destination. */
    for (index = 0; index < size; index++) out_bytes[index] = in_bytes[index];
    return destination;
}

/* memchr searches the first size bytes of buffer for the low eight bits of value.
   It reads the buffer and returns a pointer to the first match, or NULL when the byte is absent.
   It does not call fail() and reports a miss by returning NULL. */
void *memchr(const void *buffer, int value, size_t size) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t index;
    /* Examine each byte until the buffer is exhausted. */
    for (index = 0; index < size; index++) {
        /* Return the address of the first byte that equals the searched value. */
        if (bytes[index] == (unsigned char)value) return (void *)(bytes + index);
    }
    return NULL;
}

/* strlen counts bytes before the terminating NUL and returns that count.
   It reads text and writes nothing. A NULL pointer is treated as an empty string.
   It does not call fail(); a missing string is reported by returning 0. */
size_t strlen(const char *text) {
    size_t length = 0;
    /* Treat a null pointer as an empty string instead of dereferencing it. */
    if (text == NULL) return 0;
    /* Count bytes until the terminating NUL. */
    while (text[length] != 0) length++;
    return length;
}

/* strcmp compares two NUL-terminated strings as unsigned bytes.
   It reads both strings and returns a negative, zero, or positive difference of the first mismatch.
   It does not call fail() and does not report a separate error. */
int strcmp(const char *left, const char *right) {
    /* Advance while the strings share a non-zero character. */
    while (*left != 0 && *left == *right) {
        left++;
        right++;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

/* strncmp compares at most count bytes of two strings as unsigned bytes.
   It reads both strings, stops at the first difference or a NUL, and returns the byte difference.
   It does not call fail(). Equal prefixes within count bytes return 0. */
int strncmp(const char *left, const char *right, size_t count) {
    size_t index;
    /* Compare the next pair of bytes until count is exhausted. */
    for (index = 0; index < count; index++) {
        unsigned char l = (unsigned char)left[index];
        unsigned char r = (unsigned char)right[index];
        /* Return the unsigned difference when the bytes differ or a string ends. */
        if (l != r || l == 0) return (int)l - (int)r;
    }
    return 0;
}

/* strcpy copies source, including its NUL, onto destination and returns the start.
   It reads source and writes destination. The destination must already be large enough.
   It does not call fail() and has no error return. */
char *strcpy(char *destination, const char *source) {
    char *start = destination;
    /* Copy each byte, including the NUL that ends the loop. */
    while ((*destination++ = *source++) != 0) {
    }
    return start;
}

/* strchr finds the first occurrence of character in a NUL-terminated string.
   It reads text and returns a pointer to the match, or NULL when the character is absent.
   A search for the NUL itself returns the terminator. It does not call fail(). */
char *strchr(const char *text, int character) {
    char wanted = (char)character;
    /* Walk every character before the terminator. */
    while (*text != 0) {
        /* Return the address of the first matching character. */
        if (*text == wanted) return (char *)text;
        text++;
    }
    /* A search for NUL matches the terminator that stopped the scan. */
    if (wanted == 0) return (char *)text;
    return NULL;
}

/* _stricmp compares two NUL-terminated strings without regard to ASCII case.
   It reads both strings, folds A-Z to a-z, and returns the unsigned difference of the first mismatch.
   It does not call fail() and does not report a separate error. */
int _stricmp(const char *left, const char *right) {
    /* Compare characters until either string ends. */
    while (*left != 0 && *right != 0) {
        unsigned char l = (unsigned char)*left++;
        unsigned char r = (unsigned char)*right++;
        /* Fold an ASCII uppercase letter on the left to lowercase. */
        if (l >= 'A' && l <= 'Z') l = (unsigned char)(l - 'A' + 'a');
        /* Fold an ASCII uppercase letter on the right to lowercase. */
        if (r >= 'A' && r <= 'Z') r = (unsigned char)(r - 'A' + 'a');
        /* Return as soon as the folded characters differ. */
        if (l != r) return (int)l - (int)r;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

/* _strnicmp compares at most count bytes without regard to ASCII case.
   It reads both strings, folds A-Z to a-z, and returns the unsigned difference.
   It does not call fail(). Equal folded prefixes within count bytes return 0. */
int _strnicmp(const char *left, const char *right, size_t count) {
    size_t index;
    /* Compare the next pair of bytes until count is exhausted. */
    for (index = 0; index < count; index++) {
        unsigned char l = (unsigned char)left[index];
        unsigned char r = (unsigned char)right[index];
        /* Fold an ASCII uppercase letter on the left to lowercase. */
        if (l >= 'A' && l <= 'Z') l = (unsigned char)(l - 'A' + 'a');
        /* Fold an ASCII uppercase letter on the right to lowercase. */
        if (r >= 'A' && r <= 'Z') r = (unsigned char)(r - 'A' + 'a');
        /* Return when the folded bytes differ or a string ends. */
        if (l != r || l == 0) return (int)l - (int)r;
    }
    return 0;
}

/* strtoul parses a base-10 unsigned value from text and optionally reports where parsing stopped.
   It reads text, skips ASCII spaces and tabs, accepts one leading plus, and writes *end when end is not NULL.
   Only radix 10 is supported. Overflow saturates at 0xffffffff. It does not call fail(); a bad radix or no digits returns 0 and leaves *end at the start. */
unsigned long strtoul(const char *text, char **end, int radix) {
    const char *cursor = text;
    unsigned long value = 0;
    /* Reject any radix other than decimal without consuming characters. */
    if (radix != 10) {
        /* Point end at the original text so the caller sees that nothing was parsed. */
        if (end != NULL) *end = (char *)text;
        return 0;
    }
    /* Skip leading spaces and tabs before the number. */
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    /* Accept a single leading plus sign. */
    if (*cursor == '+') cursor++;
    /* Fail the parse when the first character is not a decimal digit. */
    if (*cursor < '0' || *cursor > '9') {
        /* Point end back at the start because no number was consumed. */
        if (end != NULL) *end = (char *)text;
        return 0;
    }
    /* Consume each remaining decimal digit. */
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned long digit = (unsigned long)(*cursor - '0');
        /* Saturate at 32 bits when another digit would overflow unsigned long. */
        if (value > (0xfffffffful - digit) / 10ul) value = 0xfffffffful;
        /* Otherwise fold this digit into the running decimal value. */
        else value = value * 10ul + digit;
        cursor++;
    }
    /* Record the first unparsed character when the caller asked for an end pointer. */
    if (end != NULL) *end = (char *)cursor;
    return value;
}

/* write_char stores one byte at used and keeps a NUL after it.
   capacity is the full buffer size, including the room for that NUL.
   A full buffer is left unchanged. The return value is the new used count, not counting the NUL.
   It does not call fail(). */
static size_t write_char(char *destination, size_t capacity, size_t used, char ch) {
    /* There is no buffer to write. */
    if (destination == NULL || capacity == 0) return 0;
    /* The previous text already fills every byte except the terminator. */
    if (used + 1 >= capacity) {
        destination[capacity - 1] = 0;
        /* Keep the earlier used count when it still points inside the buffer. */
        if (used >= capacity) return capacity - 1;
        return used;
    }
    destination[used] = ch;
    destination[used + 1] = 0;
    return used + 1;
}

/* write_text copies text into destination starting at used.
   A NULL text is treated as an empty string. Copying stops before the last byte so the buffer stays terminated.
   The return value is the new used count. It does not call fail(). */
static size_t write_text(char *destination, size_t capacity, size_t used, const char *text) {
    /* There is no buffer to write. */
    if (destination == NULL || capacity == 0) return 0;
    /* The previous text already fills the buffer. */
    if (used >= capacity) {
        destination[capacity - 1] = 0;
        return capacity - 1;
    }
    destination[used] = 0;
    /* A missing string adds no characters. */
    if (text == NULL) return used;
    /* Copy each source byte. write_char stops at the end of the buffer. */
    while (*text != 0) {
        size_t next = write_char(destination, capacity, used, *text);
        /* The buffer is full, so later source bytes are dropped. */
        if (next == used) return used;
        used = next;
        text++;
    }
    return used;
}

/* write_u writes value as unsigned decimal with no leading zeros except for zero itself.
   Digits are collected least-significant first and then appended from the other end.
   The return value is the new used count. It does not call fail(). */
static size_t write_u(char *destination, size_t capacity, size_t used, unsigned long long value) {
    char digits[20];
    int count = 0;
    /* Zero has no remainders, so store a single digit. */
    if (value == 0) digits[count++] = '0';
    /* Peel off decimal digits from least significant to most significant. */
    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    }
    /* Append those digits most significant first. */
    while (count > 0) used = write_char(destination, capacity, used, digits[--count]);
    return used;
}

/* write_d writes value as signed decimal.
   A negative value gets a leading minus, then the same digits as write_u.
   The most negative long long is negated without overflow. It does not call fail(). */
static size_t write_d(char *destination, size_t capacity, size_t used, long long value) {
    unsigned long long magnitude;
    /* Negative values are written with a minus sign and a positive magnitude. */
    if (value < 0) {
        used = write_char(destination, capacity, used, '-');
        magnitude = (unsigned long long)(-(value + 1)) + 1ull;
    /* A non-negative value is its own magnitude. */
    } else {
        magnitude = (unsigned long long)value;
    }
    return write_u(destination, capacity, used, magnitude);
}

/* write_x writes value as lowercase hexadecimal.
   min_digits is the smallest number of digits, filled with leading zeros. Extra digits are kept when the value needs them.
   Zero with min_digits 1 is a single 0. The return value is the new used count. It does not call fail(). */
static size_t write_x(char *destination, size_t capacity, size_t used, unsigned long long value, int min_digits) {
    char digits[16];
    int count = 0;
    /* A missing minimum still produces one digit for zero. */
    if (min_digits < 1) min_digits = 1;
    /* The digit stack holds at most 16 nibbles of a 64-bit value. */
    if (min_digits > 16) min_digits = 16;
    /* Zero has no nibbles, so store a single digit. */
    if (value == 0) digits[count++] = '0';
    /* Peel off nibbles from least significant to most significant. */
    while (value > 0) {
        unsigned int digit = (unsigned int)(value & 0xfull);
        digits[count++] = (char)(digit < 10u ? '0' + digit : 'a' + (digit - 10u));
        value >>= 4;
    }
    /* Pad the high end until the requested width is met. */
    while (count < min_digits) digits[count++] = '0';
    /* Append those digits most significant first. */
    while (count > 0) used = write_char(destination, capacity, used, digits[--count]);
    return used;
}

/* console_write sends a NUL-terminated string to a standard handle.
   It reads standard_handle and text, queries the handle, and calls WriteFile with the byte length.
   It does not call fail(). A null string, a null handle, or an invalid handle returns without writing, and WriteFile errors are ignored. */
static void console_write(DWORD standard_handle, const char *text) {
    HANDLE handle = GetStdHandle(standard_handle);
    DWORD length = 0;
    DWORD written = 0;
    /* Skip the write when there is no text or the standard handle is unusable. */
    if (text == NULL || handle == NULL || handle == INVALID_HANDLE_VALUE) return;
    /* Count bytes up to the terminating NUL. */
    while (text[length] != 0) length++;
    /* Do not call WriteFile for an empty string. */
    if (length == 0) return;
    WriteFile(handle, text, length, &written, NULL);
}

/* fail copies text into g_error_text, prints it to standard error, and exits.
   The printed line is "small_linker: error: " plus that text and a newline.
   A caller that already built the message in g_error_text can pass that buffer, and it is not copied over itself.
   ExitProcess(1) ends the process, so fail() does not return to its caller. */
static void fail(const char *text) {
    /* Keep a message that was composed directly in g_error_text. */
    if (text != g_error_text) write_text(g_error_text, sizeof g_error_text, 0, text);
    /* Show the diagnostic on standard error before the process ends. */
    console_write(STD_ERROR_HANDLE, "small_linker: error: ");
    console_write(STD_ERROR_HANDLE, g_error_text);
    console_write(STD_ERROR_HANDLE, "\n");
    ExitProcess(1);
}

/* crash_if exits when condition is true.
   The printed line is the same as fail(): "small_linker: error: " plus message and a newline.
   A false condition returns. A true condition does not return, because fail() calls ExitProcess(1).
   The self-test still has a one-argument crash_if macro, defined later, which exits with the source line instead of printing message. */
static void crash_if(int condition, const char *message) {
    /* A passing check leaves the process running. */
    if (!condition) return;
    fail(message);
}

/* arena_init reserves the process-lifetime bump allocator used by every later allocation.
   It writes g_arena, g_arena_cap, and leaves g_arena_used unchanged. The block is 32 MiB of committed read-write pages.
   If VirtualAlloc fails, crash_if prints the out-of-memory diagnostic and exits. The arena is not used to format that message. */
static void arena_init(void) {
    /* Keep the existing arena when initialization already succeeded. */
    if (g_arena != NULL) return;
    g_arena_cap = 32ull * 1024ull * 1024ull;
    g_arena = (uint8_t *)VirtualAlloc(NULL, g_arena_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    /* Give up when the operating system cannot commit the arena. */
    crash_if(g_arena == NULL, "out of memory");
}

/* arena_reset drops every bump allocation by rewinding the used watermark to zero.
   It writes g_arena_used and does not free the underlying VirtualAlloc block.
   It does not call fail() and has no error return. */
static void arena_reset(void) {
    g_arena_used = 0;
}

/* allocate returns a zeroed block of exactly size bytes from the bump arena.
   It reads g_arena, g_arena_used, and g_arena_cap, writes the bytes at the current cursor, and advances g_arena_used by size.
   There is no header and no alignment gap before or after the block. A zero size returns the cursor and leaves it unchanged.
   On exhaustion it calls fail(), which prints the error and exits, and does not return. */
static void *allocate(size_t size) {
    uint8_t *block;
    /* Reject a missing arena or a request that does not fit in the bytes still unused. */
    if (g_arena == NULL || size > g_arena_cap - g_arena_used) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "out of memory allocating ");
        used = write_u(g_error_text, sizeof g_error_text, used, size);
        write_text(g_error_text, sizeof g_error_text, used, " bytes");
        fail(g_error_text);
    }
    block = g_arena + g_arena_used;
    /* Clear the new block. A zero size clears nothing. */
    memset(block, 0, size);
    g_arena_used += size;
    return block;
}

/* _strdup allocates a NUL-terminated copy of text and returns it.
   It reads text, including the terminator, and writes a new arena block of length plus one.
   Allocation failure calls fail() inside allocate and does not return, so this function does not return NULL. */
static char *_strdup(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)allocate(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}
/* IMAGE_FILE_HEADER is the 20-byte COFF header at the start of an object and again inside a PE image.
   parse_object reads Machine, section count, the symbol-table location, and SizeOfOptionalHeader from the input.
   When the image is written, the same struct carries the output machine, the emitted section count, and the executable characteristics. */
typedef struct {
    /* Machine stores the COFF architecture code, 0x014c for I386 or 0x8664 for AMD64.
       The linker copies it from the object and rejects any other value before choosing pointer size and image base. */
    uint16_t Machine;
    /* NumberOfSections is the count of IMAGE_SECTION_HEADER entries that follow this header.
       On input it sizes the section table; on output it is the number of sections actually emitted after packing. */
    uint16_t NumberOfSections;
    /* TimeDateStamp is the Unix-time stamp COFF stores in seconds since 1970, or zero.
       This linker never reads the input stamp and leaves the output stamp zero because the image buffer is cleared. */
    uint32_t TimeDateStamp;
    /* PointerToSymbolTable is the byte offset from the start of the object to the first IMAGE_SYMBOL.
       parse_object requires a non-zero offset and checks that the symbol records fit in the file. */
    uint32_t PointerToSymbolTable;
    /* NumberOfSymbols counts COFF symbol records, including auxiliary records that follow a symbol.
       The linker requires a non-zero count, uses it to bound the symbol table, and skips aux records with NumberOfAuxSymbols. */
    uint32_t NumberOfSymbols;
    /* SizeOfOptionalHeader is the byte size of the PE optional header that follows, or zero in a relocatable object.
       A non-zero value on input is rejected. On output it is sizeof the PE32 or PE32+ optional header. */
    uint16_t SizeOfOptionalHeader;
    /* Characteristics is a bit mask of IMAGE_FILE_* flags describing the image.
       The input mask is ignored. The output sets EXECUTABLE_IMAGE plus LARGE_ADDRESS_AWARE or 32BIT_MACHINE. */
    uint16_t Characteristics;
} IMAGE_FILE_HEADER;

/* IMAGE_SECTION_HEADER is the 40-byte COFF section header used both for input objects and the emitted PE.
   parse_object reads the name, raw size, raw pointer, relocation pointer, line-number count, and characteristic bits.
   Image emission writes the merged name, virtual size, RVA, raw size, raw file offset, and combined flags. */
typedef struct {
    /* Name holds an 8-byte section name that is not necessarily NUL-terminated.
       A leading slash and decimal offset means the real name is in the COFF string table; otherwise the bytes are the short name. */
    uint8_t Name[8];
    /* Misc overlays two 32-bit views of the same header slot, selected by context rather than by a tag.
       Object files traditionally store a physical address here, while PE images store the virtual size. This linker writes VirtualSize and does not read the input slot. */
    union {
        /* PhysicalAddress is the object-file view of Misc, a byte address COFF relocation once used.
           This linker never reads it; the slot is occupied by VirtualSize when a PE section header is written. */
        uint32_t PhysicalAddress;
        /* VirtualSize is the in-memory size of the section in bytes, before file alignment.
           The linker stores the merged output virtual size here when it emits the PE section table. */
        uint32_t VirtualSize;
    } Misc;
    /* VirtualAddress is the section RVA in bytes from the image base.
       Input objects leave it unused. Output headers receive the laid-out RVA, including a header-packed .rdata. */
    uint32_t VirtualAddress;
    /* SizeOfRawData is the on-disk payload size in bytes.
       On input it is the initialized byte count parse_object copies. On output it is the file-aligned size, or zero for BSS. */
    uint32_t SizeOfRawData;
    /* PointerToRawData is the byte offset from the start of the file to the section payload.
       The linker range-checks it against the object and later writes the output file offset of each emitted section. */
    uint32_t PointerToRawData;
    /* PointerToRelocations is the byte offset of this section's IMAGE_RELOCATION array, or zero when there are none.
       parse_object checks that the array fits, and relocation application reads fixups from that offset. */
    uint32_t PointerToRelocations;
    /* PointerToLinenumbers is the byte offset of COFF line-number records, which this linker does not consume.
       The field is retained so the 40-byte header layout stays intact; a non-zero NumberOfLinenumbers is rejected instead. */
    uint32_t PointerToLinenumbers;
    /* NumberOfRelocations counts IMAGE_RELOCATION entries at PointerToRelocations.
       Zero means the section has no fixups. A non-zero count is range-checked before relocations are applied. */
    uint16_t NumberOfRelocations;
    /* NumberOfLinenumbers counts COFF line-number records attached to the section.
       Any non-zero count fails the link because line numbers are not supported. */
    uint16_t NumberOfLinenumbers;
    /* Characteristics is the IMAGE_SCN_* bit mask: contents, alignment, link removal, and memory protection.
       The linker uses it to discard sections, compute alignment, and merge code, data, and permission bits into the output. */
    uint32_t Characteristics;
} IMAGE_SECTION_HEADER;

/* #pragma pack(push, 2) saves the current packing and caps member alignment at 2 bytes
   until the matching pack(pop). This file is compiled as x64, so the packing in force
   here is the compiler default of 8 (/Zp8). The saved value is that default.
   The Windows SDK does the same thing in winnt.h. Around IMAGE_SYMBOL it includes
   pshpack2.h under the comment "Symbols, relocs, and linenumbers are 2 byte packed",
   and it defines IMAGE_SIZEOF_SYMBOL as 18. This file includes no SDK headers, so the
   pragma is written out in full. The line-number record stays outside the region:
   a non-zero NumberOfLinenumbers is rejected, and there is no IMAGE_LINENUMBER struct.
   Under the default packing the compiler rounds a struct size up to a multiple of its
   strictest member alignment. IMAGE_SYMBOL's fields add to 18 bytes and its strictest
   member is a uint32, so the natural size is 20. IMAGE_RELOCATION's fields add to 10
   bytes and the natural size is 12. COFF stores neither trailing pad. A symbol record
   on disk is 18 bytes and a relocation record is 10. Symbol and relocation walks use
   sizeof as the stride, and the _Static_asserts below lock those two sizes.
   A 20-byte stride reads every symbol after the first from the wrong file offset.
   The second record starts 18 bytes after a table that itself may be 4-byte aligned,
   so its uint32 fields are only 2-byte aligned. pack(2) is the alignment the compiler
   may assume for those loads. Only these two structs belong between the push and the
   pop. IMAGE_FILE_HEADER (20) and IMAGE_SECTION_HEADER (40) already match their natural
   sizes and stay in front of the push. */
#pragma pack(push, 2)
/* IMAGE_SYMBOL is one 18-byte COFF symbol-table record, packed to 2-byte alignment.
   The linker reads the name overlay, Value, SectionNumber, and NumberOfAuxSymbols while resolving addresses and the entry point.
   Type and StorageClass are not interpreted; auxiliary records are skipped by count rather than decoded. */
typedef struct {
    /* N is the 8-byte name overlay at the start of the symbol record.
       A non-zero first dword means the name is inline. A zero first dword means the second dword is a string-table offset. */
    union {
        /* ShortName is the inline name as up to eight raw bytes, padded and not necessarily NUL-terminated.
           symbol_name copies these bytes into a nine-byte buffer when the short-name dword is non-zero. */
        uint8_t ShortName[8];
        /* Name splits the same eight bytes into the COFF short/long pair used to detect string-table names.
           Short is tested as an integer, and Long is passed to coff_string only when Short is zero. */
        struct {
            /* Short is the first four bytes of the symbol name viewed as a little-endian dword.
               A non-zero value selects the inline ShortName. Zero selects the string table, even if later name bytes are non-zero. */
            uint32_t Short;
            /* Long is the second four bytes of the name overlay.
               When Short is zero, Long is the byte offset from the start of the COFF string table to a NUL-terminated name. */
            uint32_t Long;
        } Name;
        /* LongName views the same eight name bytes as two little-endian dwords.
           The linker does not read this array directly; it is the third legal COFF overlay of the name slot. */
        uint32_t LongName[2];
    } N;
    /* Value is a section-relative byte offset for a defined symbol, or the absolute value when SectionNumber is -1.
       symbol_rva adds it to the output section RVA and rejects an offset past the input section size. */
    uint32_t Value;
    /* SectionNumber selects the symbol's section: 0 is undefined, -1 is absolute, and a positive value is a 1-based section index.
       The linker uses it to tell imports, absolute symbols, and real sections apart. */
    signed short SectionNumber;
    /* Type is the COFF symbol type, with the low nibble as the base type and the high byte as the derived type.
       This linker does not decode it; the field exists so each symbol record stays 18 bytes. */
    uint16_t Type;
    /* StorageClass is the COFF storage class byte, such as external or static.
       This linker does not read it and treats every defined symbol with a usable section as linkable. */
    uint8_t StorageClass;
    /* NumberOfAuxSymbols is how many 18-byte auxiliary records follow this symbol.
       Symbol walks add one plus this count to reach the next primary symbol and fail if that skips past the table. */
    uint8_t NumberOfAuxSymbols;
} IMAGE_SYMBOL;

/* IMAGE_RELOCATION is one 10-byte COFF fixup, packed with the symbol table.
   Each record names a byte offset in the section, a symbol-table index, and a relocation type code.
   The linker reads the array from PointerToRelocations and patches the output section bytes in place. */
typedef struct {
    /* VirtualAddress is the byte offset of the fixup within the section payload, not an image RVA.
       The linker adds it to the section's output data and rejects a patch that would extend past the section. */
    uint32_t VirtualAddress;
    /* SymbolTableIndex selects the IMAGE_SYMBOL this relocation refers to.
       The linker bounds it by NumberOfSymbols and then resolves that symbol to an RVA, an absolute value, or an import. */
    uint32_t SymbolTableIndex;
    /* Type is the machine-specific relocation code, such as REL32, ADDR64, DIR32, SECREL, or ABSOLUTE.
       A known type selects the patch width and calculation. An unknown type calls fail() and does not return. */
    uint16_t Type;
} IMAGE_RELOCATION;
/* #pragma pack(pop) restores the packing saved by pack(push, 2): the x64 default of 8.
   Packing is a file-wide setting, so everything from here to the end of the translation
   unit is laid out with that default. The pop has to sit on the next line after
   IMAGE_RELOCATION. Another struct placed before the pop would be capped at 2 as well.
   IMAGE_OPTIONAL_HEADER64 (240) and IMAGE_OPTIONAL_HEADER32 (224) happen to contain no
   padding that a cap of 2 would delete, so those two _Static_asserts would still pass
   if this pop were missing. Later structs would not. OutputSection begins with
   char name[9]. The next field, characteristics, is a uint32. Default packing inserts
   3 bytes and puts characteristics at offset 12; the data pointer then sits at offset 40
   and the struct is 56 bytes. A lingering cap of 2 inserts 1 byte, puts characteristics
   at offset 10, puts the data pointer at offset 34, and makes the struct 46 bytes.
   The same shift hits any pointer or uint64 that follows a smaller field, including
   the ImportDll pointer that follows the one-byte pack_headers flag in Linker.
   Those layouts are in-memory only, and they are still wrong if the cap leaks. */
#pragma pack(pop)

/* IMAGE_DOS_HEADER is the 64-byte DOS stub the linker places at the start of every PE image.
   Only e_magic and e_lfanew are stored. Every other field stays zero because the image is allocated cleared.
   The loader uses the magic and e_lfanew to find the PE signature and ignores the unused DOS relocation fields. */
typedef struct {
    /* e_magic is the DOS signature, stored little-endian as the bytes M and Z.
       The linker writes IMAGE_DOS_SIGNATURE (0x5A4D) so the loader accepts the image. */
    uint16_t e_magic;
    /* e_cblp is the DOS byte count of the last page of the file.
       The linker leaves it zero; a PE loader does not use the DOS page size. */
    uint16_t e_cblp;
    /* e_cp is the DOS page count of the file.
       The linker leaves it zero because no DOS stub program is emitted. */
    uint16_t e_cp;
    /* e_crlc is the number of DOS relocation entries.
       The linker leaves it zero because the stub has no DOS relocations. */
    uint16_t e_crlc;
    /* e_cparhdr is the DOS header size in 16-byte paragraphs.
       The linker leaves it zero; e_lfanew, not this field, locates the PE header. */
    uint16_t e_cparhdr;
    /* e_minalloc is the minimum extra memory a DOS program wants, in paragraphs.
       The linker leaves it zero because the image is not run as a DOS program. */
    uint16_t e_minalloc;
    /* e_maxalloc is the maximum extra memory a DOS program wants, in paragraphs.
       The linker leaves it zero along with the rest of the unused stub. */
    uint16_t e_maxalloc;
    /* e_ss is the initial DOS stack segment.
       The linker leaves it zero; the PE optional header describes the real stack. */
    uint16_t e_ss;
    /* e_sp is the initial DOS stack pointer.
       The linker leaves it zero because the DOS stub is not executed. */
    uint16_t e_sp;
    /* e_csum is the DOS checksum word.
       The linker leaves it zero and also leaves the PE checksum zero. */
    uint16_t e_csum;
    /* e_ip is the initial DOS instruction pointer.
       The linker leaves it zero; AddressOfEntryPoint in the optional header is the real entry. */
    uint16_t e_ip;
    /* e_cs is the initial DOS code segment.
       The linker leaves it zero because there is no DOS entry stub. */
    uint16_t e_cs;
    /* e_lfarlc is the file offset of the DOS relocation table.
       The linker leaves it zero because e_crlc is zero and no table is present. */
    uint16_t e_lfarlc;
    /* e_ovno is the DOS overlay number.
       The linker leaves it zero; overlays are not used. */
    uint16_t e_ovno;
    /* e_res is four reserved DOS words that must occupy bytes 28 through 35.
       The linker leaves all four zero so the header stays the standard 64-byte size. */
    uint16_t e_res[4];
    /* e_oemid is the DOS OEM identifier.
       The linker leaves it zero; no OEM extension is emitted. */
    uint16_t e_oemid;
    /* e_oeminfo is the DOS OEM information word paired with e_oemid.
       The linker leaves it zero. */
    uint16_t e_oeminfo;
    /* e_res2 is ten reserved words that pad the header out to the PE offset field.
       The linker leaves them zero so e_lfanew stays at file offset 60. */
    uint16_t e_res2[10];
    /* e_lfanew is the file offset in bytes of the PE signature.
       The linker sets it to DOS_HEADER_SIZE (64), which places the signature immediately after this header. */
    signed int e_lfanew;
} IMAGE_DOS_HEADER;

/* IMAGE_DATA_DIRECTORY is one PE data-directory entry: an RVA plus a size in bytes.
   The optional header carries sixteen of them. This linker fills the import directory and the IAT directory and leaves the rest zero. */
typedef struct {
    /* VirtualAddress is the RVA of the directory payload, in bytes from the image base.
       For imports it is the RVA of .idata. For the IAT it is the RVA of the IAT inside .idata. Zero means the directory is absent. */
    uint32_t VirtualAddress;
    /* Size is the directory payload length in bytes.
       The import directory size is the descriptor array including its null terminator, and the IAT size includes each DLL's null thunk. */
    uint32_t Size;
} IMAGE_DATA_DIRECTORY;

/* IMAGE_OPTIONAL_HEADER64 is the PE32+ optional header written for an AMD64 image.
   It is 240 bytes, including sixteen data directories. The linker fills magic, sizes, entry RVA, base, alignments, subsystem, stack, heap, and the import directories.
   Fields that are not assigned stay zero because the image buffer is cleared before the header is stored. */
typedef struct {
    /* Magic selects the optional-header format. PE32+ uses 0x020B.
       The linker writes IMAGE_NT_OPTIONAL_HDR64_MAGIC only on the AMD64 path. */
    uint16_t Magic;
    /* MajorLinkerVersion is the linker major version stored for tools that display it.
       This linker writes 1. */
    uint8_t MajorLinkerVersion;
    /* MinorLinkerVersion is the linker minor version.
       It stays zero, so the reported version is 1.0. */
    uint8_t MinorLinkerVersion;
    /* SizeOfCode is the sum, in bytes, of the file-aligned raw sizes of sections marked as code.
       The linker accumulates output raw_size for every section with IMAGE_SCN_CNT_CODE. */
    uint32_t SizeOfCode;
    /* SizeOfInitializedData is the sum, in bytes, of the raw sizes of initialized-data sections.
       The linker adds raw_size for each section with IMAGE_SCN_CNT_INITIALIZED_DATA. */
    uint32_t SizeOfInitializedData;
    /* SizeOfUninitializedData is the sum, in bytes, of the virtual sizes of BSS-style sections.
       The linker adds virtual_size for each section with IMAGE_SCN_CNT_UNINITIALIZED_DATA. */
    uint32_t SizeOfUninitializedData;
    /* AddressOfEntryPoint is the entry RVA in bytes from the image base.
       The linker stores entry_rva after resolving the requested symbol or a built-in default name. */
    uint32_t AddressOfEntryPoint;
    /* BaseOfCode is the RVA of the first section that has the code characteristic.
       It stays zero when the image has no code section. */
    uint32_t BaseOfCode;
    /* ImageBase is the preferred load address in bytes. AMD64 images use 0x140000000.
       Relocations that need an absolute VA add this base; the linker does not emit a relocation directory to rebase it. */
    uint64_t ImageBase;
    /* SectionAlignment is the multiple, in bytes, between section RVAs.
       The linker writes 4096 and lays every non-packed section out on that boundary. */
    uint32_t SectionAlignment;
    /* FileAlignment is the multiple, in bytes, of each section's raw file size and of SizeOfHeaders.
       The linker writes 512. */
    uint32_t FileAlignment;
    /* MajorOperatingSystemVersion is the minimum OS major version the image asks for.
       The linker writes 6. */
    uint16_t MajorOperatingSystemVersion;
    /* MinorOperatingSystemVersion is the minimum OS minor version.
       The linker leaves it zero, so the version is 6.0. */
    uint16_t MinorOperatingSystemVersion;
    /* MajorImageVersion is the user-defined image major version, not the OS version.
       The linker leaves it zero. */
    uint16_t MajorImageVersion;
    /* MinorImageVersion is the user-defined image minor version.
       The linker leaves it zero. */
    uint16_t MinorImageVersion;
    /* MajorSubsystemVersion is the subsystem major version the loader checks.
       The linker writes 6. */
    uint16_t MajorSubsystemVersion;
    /* MinorSubsystemVersion is the subsystem minor version.
       The linker leaves it zero. */
    uint16_t MinorSubsystemVersion;
    /* Win32VersionValue is a reserved dword that the PE specification requires to be zero.
       The linker leaves it zero. */
    uint32_t Win32VersionValue;
    /* SizeOfImage is the full in-memory image size in bytes, rounded up to SectionAlignment.
       The linker sets it from the highest section RVA plus virtual size, and never below one section alignment. */
    uint32_t SizeOfImage;
    /* SizeOfHeaders is the file-aligned size of the DOS, PE, optional, and section headers.
       Section raw data begins at this offset unless pack-headers places .rdata inside the padding. */
    uint32_t SizeOfHeaders;
    /* CheckSum is the PE checksum in the optional header.
       The linker leaves it zero; nothing in this program computes an image checksum. */
    uint32_t CheckSum;
    /* Subsystem tells the loader whether to use the Windows GUI (2) or console (3) subsystem.
       The linker copies Linker.subsystem, which the command line or a test harness sets. */
    uint16_t Subsystem;
    /* DllCharacteristics is a bit mask of image DLL characteristics, despite the name applying to EXEs too.
       The linker sets NX compatibility and terminal-server awareness. */
    uint16_t DllCharacteristics;
    /* SizeOfStackReserve is the reserved stack size in bytes.
       The linker writes 0x100000. */
    uint64_t SizeOfStackReserve;
    /* SizeOfStackCommit is the initially committed stack size in bytes.
       The linker writes 0x1000. */
    uint64_t SizeOfStackCommit;
    /* SizeOfHeapReserve is the reserved default-heap size in bytes.
       The linker writes 0x100000. */
    uint64_t SizeOfHeapReserve;
    /* SizeOfHeapCommit is the initially committed default-heap size in bytes.
       The linker writes 0x1000. */
    uint64_t SizeOfHeapCommit;
    /* LoaderFlags is an obsolete loader bitfield.
       The linker leaves it zero. */
    uint32_t LoaderFlags;
    /* NumberOfRvaAndSizes is the count of data-directory entries that follow.
       The linker writes 16, matching DataDirectory. */
    uint32_t NumberOfRvaAndSizes;
    /* DataDirectory holds sixteen RVA-and-size slots indexed by IMAGE_DIRECTORY_ENTRY_*.
       The linker fills entry 1 with the import descriptors and entry 12 with the IAT, and leaves the other slots zero. */
    IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER64;

/* IMAGE_OPTIONAL_HEADER32 is the PE32 optional header written for an I386 image.
   It is 224 bytes. Compared with PE32+ it has a 32-bit ImageBase, a BaseOfData RVA, and 32-bit stack and heap sizes.
   The linker fills the same logical fields as the 64-bit header, and unassigned fields stay zero. */
typedef struct {
    /* Magic selects the optional-header format. PE32 uses 0x010B.
       The linker writes IMAGE_NT_OPTIONAL_HDR32_MAGIC only on the I386 path. */
    uint16_t Magic;
    /* MajorLinkerVersion is the linker major version stored for tools that display it.
       This linker writes 1. */
    uint8_t MajorLinkerVersion;
    /* MinorLinkerVersion is the linker minor version.
       It stays zero, so the reported version is 1.0. */
    uint8_t MinorLinkerVersion;
    /* SizeOfCode is the sum, in bytes, of the file-aligned raw sizes of sections marked as code.
       The linker accumulates output raw_size for every section with IMAGE_SCN_CNT_CODE. */
    uint32_t SizeOfCode;
    /* SizeOfInitializedData is the sum, in bytes, of the raw sizes of initialized-data sections.
       The linker adds raw_size for each section with IMAGE_SCN_CNT_INITIALIZED_DATA. */
    uint32_t SizeOfInitializedData;
    /* SizeOfUninitializedData is the sum, in bytes, of the virtual sizes of BSS-style sections.
       The linker adds virtual_size for each section with IMAGE_SCN_CNT_UNINITIALIZED_DATA. */
    uint32_t SizeOfUninitializedData;
    /* AddressOfEntryPoint is the entry RVA in bytes from the image base.
       The linker stores entry_rva after resolving the requested symbol or a built-in default name. */
    uint32_t AddressOfEntryPoint;
    /* BaseOfCode is the RVA of the first section that has the code characteristic.
       It stays zero when the image has no code section. */
    uint32_t BaseOfCode;
    /* BaseOfData is the RVA of the first non-code section that holds initialized or uninitialized data.
       PE32+ has no such field. It stays zero when every section is code. */
    uint32_t BaseOfData;
    /* ImageBase is the preferred 32-bit load address. I386 images use 0x00400000.
       The linker narrows image_base to uint32_t when writing this field. */
    uint32_t ImageBase;
    /* SectionAlignment is the multiple, in bytes, between section RVAs.
       The linker writes 4096 and lays every non-packed section out on that boundary. */
    uint32_t SectionAlignment;
    /* FileAlignment is the multiple, in bytes, of each section's raw file size and of SizeOfHeaders.
       The linker writes 512. */
    uint32_t FileAlignment;
    /* MajorOperatingSystemVersion is the minimum OS major version the image asks for.
       The linker writes 6. */
    uint16_t MajorOperatingSystemVersion;
    /* MinorOperatingSystemVersion is the minimum OS minor version.
       The linker leaves it zero, so the version is 6.0. */
    uint16_t MinorOperatingSystemVersion;
    /* MajorImageVersion is the user-defined image major version, not the OS version.
       The linker leaves it zero. */
    uint16_t MajorImageVersion;
    /* MinorImageVersion is the user-defined image minor version.
       The linker leaves it zero. */
    uint16_t MinorImageVersion;
    /* MajorSubsystemVersion is the subsystem major version the loader checks.
       The linker writes 6. */
    uint16_t MajorSubsystemVersion;
    /* MinorSubsystemVersion is the subsystem minor version.
       The linker leaves it zero. */
    uint16_t MinorSubsystemVersion;
    /* Win32VersionValue is a reserved dword that the PE specification requires to be zero.
       The linker leaves it zero. */
    uint32_t Win32VersionValue;
    /* SizeOfImage is the full in-memory image size in bytes, rounded up to SectionAlignment.
       The linker sets it from the highest section RVA plus virtual size, and never below one section alignment. */
    uint32_t SizeOfImage;
    /* SizeOfHeaders is the file-aligned size of the DOS, PE, optional, and section headers.
       Section raw data begins at this offset unless pack-headers places .rdata inside the padding. */
    uint32_t SizeOfHeaders;
    /* CheckSum is the PE checksum in the optional header.
       The linker leaves it zero; nothing in this program computes an image checksum. */
    uint32_t CheckSum;
    /* Subsystem tells the loader whether to use the Windows GUI (2) or console (3) subsystem.
       The linker copies Linker.subsystem, which the command line or a test harness sets. */
    uint16_t Subsystem;
    /* DllCharacteristics is a bit mask of image DLL characteristics, despite the name applying to EXEs too.
       The linker sets NX compatibility and terminal-server awareness. */
    uint16_t DllCharacteristics;
    /* SizeOfStackReserve is the reserved stack size in bytes, stored as a 32-bit value in PE32.
       The linker writes 0x100000. */
    uint32_t SizeOfStackReserve;
    /* SizeOfStackCommit is the initially committed stack size in bytes.
       The linker writes 0x1000. */
    uint32_t SizeOfStackCommit;
    /* SizeOfHeapReserve is the reserved default-heap size in bytes.
       The linker writes 0x100000. */
    uint32_t SizeOfHeapReserve;
    /* SizeOfHeapCommit is the initially committed default-heap size in bytes.
       The linker writes 0x1000. */
    uint32_t SizeOfHeapCommit;
    /* LoaderFlags is an obsolete loader bitfield.
       The linker leaves it zero. */
    uint32_t LoaderFlags;
    /* NumberOfRvaAndSizes is the count of data-directory entries that follow.
       The linker writes 16, matching DataDirectory. */
    uint32_t NumberOfRvaAndSizes;
    /* DataDirectory holds sixteen RVA-and-size slots indexed by IMAGE_DIRECTORY_ENTRY_*.
       The linker fills entry 1 with the import descriptors and entry 12 with the IAT, and leaves the other slots zero. */
    IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER32;

/* IMAGE_IMPORT_DESCRIPTOR is one 20-byte import entry for a single DLL, followed by a zeroed sentinel.
   prepare_imports reserves one slot per DLL plus the terminator. build_import_data writes the ILT RVA, the DLL-name RVA, and the IAT RVA.
   TimeDateStamp and ForwarderChain stay zero, so the imports are unbound and not forwarded. */
typedef struct {
    /* OriginalFirstThunk is the RVA of this DLL's import lookup table.
       The linker writes the RVA of the ILT it laid out in .idata. The loader leaves that table intact while filling the IAT. */
    uint32_t OriginalFirstThunk;
    /* TimeDateStamp is zero for an unbound import, or a stamp when the image was bound.
       The linker leaves it zero because it does not bind imports. */
    uint32_t TimeDateStamp;
    /* ForwarderChain is the old bound-import forwarder index, or zero when unused.
       The linker leaves it zero. */
    uint32_t ForwarderChain;
    /* Name is the RVA of the NUL-terminated DLL name string inside .idata.
       The linker writes the RVA of the name it copied at name_offset. */
    uint32_t Name;
    /* FirstThunk is the RVA of this DLL's import address table.
       The linker writes the IAT RVA. At runtime the loader replaces those slots with function addresses. */
    uint32_t FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR;

_Static_assert(sizeof(IMAGE_FILE_HEADER) == 20, "invalid COFF file header size");
_Static_assert(sizeof(IMAGE_SECTION_HEADER) == 40,
               "invalid COFF section header size");
_Static_assert(sizeof(IMAGE_SYMBOL) == 18, "invalid COFF symbol size");
_Static_assert(sizeof(IMAGE_RELOCATION) == 10, "invalid COFF relocation size");
_Static_assert(sizeof(IMAGE_OPTIONAL_HEADER32) == 224,
               "invalid PE32 optional header size");
_Static_assert(sizeof(IMAGE_OPTIONAL_HEADER64) == 240,
               "invalid PE32+ optional header size");
_Static_assert(sizeof(IMAGE_IMPORT_DESCRIPTOR) == 20,
               "invalid import descriptor size");

#define IMAGE_FILE_MACHINE_I386 0x014c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_EXECUTABLE_IMAGE 0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020
#define IMAGE_FILE_32BIT_MACHINE 0x0100
#define IMAGE_SYM_UNDEFINED 0
#define IMAGE_SYM_ABSOLUTE (-1)
#define IMAGE_SCN_LNK_REMOVE 0x00000800
#define IMAGE_SCN_CNT_CODE 0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA 0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_ALIGN_MASK 0x00f00000
#define IMAGE_SCN_MEM_DISCARDABLE 0x02000000
#define IMAGE_SCN_MEM_NOT_CACHED 0x04000000
#define IMAGE_SCN_MEM_NOT_PAGED 0x08000000
#define IMAGE_SCN_MEM_SHARED 0x10000000
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000
#define IMAGE_REL_AMD64_ABSOLUTE 0x0000
#define IMAGE_REL_AMD64_ADDR64 0x0001
#define IMAGE_REL_AMD64_ADDR32 0x0002
#define IMAGE_REL_AMD64_ADDR32NB 0x0003
#define IMAGE_REL_AMD64_REL32 0x0004
#define IMAGE_REL_AMD64_REL32_1 0x0005
#define IMAGE_REL_AMD64_REL32_2 0x0006
#define IMAGE_REL_AMD64_REL32_3 0x0007
#define IMAGE_REL_AMD64_REL32_4 0x0008
#define IMAGE_REL_AMD64_REL32_5 0x0009
#define IMAGE_REL_AMD64_SECTION 0x000a
#define IMAGE_REL_AMD64_SECREL 0x000b
#define IMAGE_REL_I386_ABSOLUTE 0x0000
#define IMAGE_REL_I386_DIR32 0x0006
#define IMAGE_REL_I386_DIR32NB 0x0007
#define IMAGE_REL_I386_SECTION 0x000a
#define IMAGE_REL_I386_SECREL 0x000b
#define IMAGE_REL_I386_REL32 0x0014
#define IMAGE_DOS_SIGNATURE 0x5a4d
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x010b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x020b
#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3
#define IMAGE_DLLCHARACTERISTICS_NX_COMPAT 0x0100
#define IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE 0x8000
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_DIRECTORY_ENTRY_IAT 12

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define IMAGE_BASE32 UINT64_C(0x00400000)
#define IMAGE_BASE64 UINT64_C(0x140000000)
#define SECTION_ALIGNMENT 4096U
#define FILE_ALIGNMENT 512U
#define DOS_HEADER_SIZE 64U

typedef struct InputSection InputSection;
typedef struct OutputSection OutputSection;
typedef struct ImportDll ImportDll;
typedef struct ImportSymbol ImportSymbol;

/* InputSection describes one COFF section after parse_object has classified it.
   Discarded sections stay in the array so symbol section numbers still line up, but they contribute no output bytes.
   Live sections point at the merged OutputSection and record where their bytes sit inside it. */
struct InputSection {
    /* header points into the input file's section table for this section.
       Later passes read its raw pointer, characteristics, and relocation location through this pointer. */
    IMAGE_SECTION_HEADER *header;
    /* output points at the merged PE section that receives this input, or NULL when the section is discarded.
       Symbol RVAs add output_offset to this section's virtual address. */
    OutputSection *output;
    /* output_offset is the byte offset of this input's bytes within the output section's virtual image.
       It is aligned to the input section's COFF alignment before the output virtual size grows. */
    uint32_t output_offset;
    /* size is the input section's SizeOfRawData in bytes.
       A zero size discards the section. For BSS the size still reserves virtual space but no file bytes are copied. */
    uint32_t size;
    /* discarded is true when the section contributes nothing to the image.
       That is an empty section, a .drectve or debug or .llvm section, or any section with IMAGE_SCN_LNK_REMOVE. */
    bool discarded;
};

/* OutputSection is one merged PE section, keyed by the canonical name with the dollar suffix removed.
   parse_object appends input contributions. Later layout fills the RVA, file offset, and byte buffer.
   Import preparation may create .text and .idata entries that no object section contributed. */
struct OutputSection {
    /* name is the canonical section name, NUL-terminated, at most eight characters.
       The dollar suffix of a COFF group is stripped before the name is stored, and the PE header receives these bytes. */
    char name[9];
    /* characteristics is the merged IMAGE_SCN_* mask for the output section.
       Content and permission bits are OR-ed from inputs. Alignment bits from the inputs are not kept here; alignment is separate. */
    uint32_t characteristics;
    /* alignment is the power-of-two byte alignment required by the strictest contribution, or 1 when none was encoded.
       Contributions and import thunks are placed on this boundary inside the section. */
    uint32_t alignment;
    /* virtual_size is the number of bytes the section occupies in memory before section alignment.
       parse_object and prepare_imports grow it. Layout uses it to advance the next RVA. */
    uint32_t virtual_size;
    /* virtual_address is the section RVA in bytes from the image base.
       assign_layout writes it. Packed .rdata may receive an RVA inside the header instead of the next 4096-byte slot. */
    uint32_t virtual_address;
    /* raw_size is the file-aligned number of payload bytes written to the image, or zero for a pure BSS section.
       Layout sets it from virtual_size rounded up to 512, and header emission copies it to SizeOfRawData. */
    uint32_t raw_size;
    /* raw_offset is the byte offset of the payload in the output file.
       Normal sections follow SizeOfHeaders. A header-packed section uses the same offset as its RVA. */
    uint32_t raw_offset;
    /* data is the arena buffer of virtual_size bytes that holds the section image before it is copied out.
       Initialized input bytes, import names, thunks, and applied relocations are written here. BSS stays zero. */
    uint8_t *data;
    /* index is the 1-based index of this section in the emitted PE section table.
       SECTION relocations add it to the patched value. A header-packed section is not emitted and keeps index zero. */
    uint16_t index;
    /* in_headers is true when pack-headers placed this section in the padding of the PE headers.
       Layout then skips a normal RVA and omits a section-table entry, while still copying the bytes at raw_offset. */
    bool in_headers;
};

/* A link may name at most this many DLLs. The slots are allocated once, on the first /import. */
#define MAX_IMPORT_DLLS 64
/* Each DLL may name at most this many symbols. Those slots are allocated once with the DLL table. */
#define MAX_IMPORT_SYMBOLS 256

/* ImportSymbol is one function imported from a DLL and named on a /import option.
   prepare_imports assigns its hint/name, ILT, IAT, and jump-thunk offsets inside .idata and .text.
   A lookup by undecorated or x86-decorated name returns this record so relocations can target the thunk or the IAT. */
struct ImportSymbol {
    /* name is the arena copy of the imported function name, without a DLL prefix or decoration.
       It is matched against COFF symbols and copied into the hint/name table with a trailing NUL. */
    char *name;
    /* hint_name_offset is the byte offset within .idata of the hint/name entry.
       The entry is a zero hint word followed by the function name. ILT and IAT slots store its RVA. */
    uint32_t hint_name_offset;
    /* ilt_offset is the byte offset within .idata of this symbol's import-lookup-table slot.
       The slot is pointer-sized, 4 bytes on I386 and 8 on AMD64, and holds the hint/name RVA. */
    uint32_t ilt_offset;
    /* iat_offset is the byte offset within .idata of this symbol's import-address-table slot.
       The initial contents match the ILT. __imp_ symbols resolve to the slot's RVA, which the loader fills at runtime. */
    uint32_t iat_offset;
    /* thunk_offset is the byte offset within .text of the six-byte jump thunk for this import.
       An undecorated call relocates to that thunk, which jumps through the IAT slot. */
    uint32_t thunk_offset;
};

/* ImportDll is one DLL named by /import, together with the symbols taken from it.
   Symbols are appended in declaration order. prepare_imports assigns the DLL's ILT, IAT, descriptor, and name offsets.
   A duplicate symbol name across any DLL calls fail() and does not return. */
struct ImportDll {
    /* name is the arena copy of the DLL file name as written in the import string.
       Comparison while finding an existing DLL is case-insensitive, and the bytes are copied into .idata as the descriptor name. */
    char *name;
    /* symbols is the fixed arena array of imported functions belonging to this DLL.
       It has MAX_IMPORT_SYMBOLS slots, filled from the start in the order the names appeared in /import. */
    ImportSymbol *symbols;
    /* symbol_count is how many ImportSymbol entries are currently filled.
       Thunk, ILT, and IAT layout walk this many names, plus a null terminator per DLL. */
    uint16_t symbol_count;
    /* descriptor_offset is the byte offset of this DLL's IMAGE_IMPORT_DESCRIPTOR within .idata.
       It is the DLL index times 20. The bytes themselves are written later from the ILT, name, and IAT RVAs. */
    uint32_t descriptor_offset;
    /* ilt_offset is the byte offset within .idata where this DLL's import lookup table begins.
       The table holds one pointer per symbol plus a null pointer, aligned to the pointer size. */
    uint32_t ilt_offset;
    /* iat_offset is the byte offset within .idata where this DLL's import address table begins.
       It is the same shape as the ILT and is what FirstThunk points at. */
    uint32_t iat_offset;
    /* name_offset is the byte offset within .idata of this DLL's NUL-terminated name.
       The name is stored after every symbol's hint/name entry. */
    uint32_t name_offset;
};

/* Linker is the in-memory link of one COFF object into one PE image.
   It owns the input view, the merged sections, the import tables, and the entry-point choice.
   parse_object fills the input side. Later passes fill layout, imports, and entry_rva. Failure anywhere calls fail() and does not return. */
typedef struct {
    /* file_data is the entire input object as one arena buffer.
       Headers, symbols, and section bytes are addressed as offsets into this pointer. */
    uint8_t *file_data;
    /* file_size is the length of file_data in bytes.
       Every COFF range check uses it as the exclusive end of the file. */
    size_t file_size;
    /* file_header points at the COFF file header, which is the first 20 bytes of file_data.
       Machine, section count, and the symbol-table location are read through it. */
    IMAGE_FILE_HEADER *file_header;
    /* section_headers points at the section table immediately after the file header.
       There is one entry per NumberOfSections, still inside file_data. */
    IMAGE_SECTION_HEADER *section_headers;
    /* symbols points at the first COFF symbol record in file_data.
       Indexes from relocations and the entry search are offsets into this array. */
    IMAGE_SYMBOL *symbols;
    /* string_table points at the COFF string table that follows the symbol records.
       The first four bytes are the table size. Long symbol and section names are offsets into the rest. */
    uint8_t *string_table;
    /* string_table_size is the byte length stored in the first dword of the string table, including those four bytes.
       coff_string rejects offsets outside 4 inclusive to this size exclusive. */
    uint32_t string_table_size;
    /* inputs is the arena array of InputSection, one per COFF section, in file order.
       A discarded section still occupies its slot so section numbers remain 1-based indexes. */
    InputSection *inputs;
    /* outputs is the arena array of merged PE sections in creation order.
       find_or_add_output appends a slot when the canonical name has not been seen. */
    OutputSection *outputs;
    /* output_count is how many OutputSection entries have been created.
       Zero after parsing means the object had no linkable section and the link fails. */
    uint16_t output_count;
    /* output_capacity is the allocated length of outputs.
       It is the input section count plus two, leaving room for a synthesized .text and .idata. */
    uint16_t output_capacity;
    /* emitted_section_count is how many sections are written into the PE section table.
       It starts equal to output_count and drops by one when pack-headers absorbs .rdata into the header. */
    uint16_t emitted_section_count;
    /* machine is the COFF machine copied from the object, either I386 or AMD64.
       Pointer size, optional-header size, image base, and legal relocation types all follow from it. */
    uint16_t machine;
    /* subsystem is the PE subsystem stored in the optional header, 2 for GUI or 3 for console.
       The command line or the caller sets it before the image is built. */
    uint16_t subsystem;
    /* pack_headers is true when a read-only .rdata section should be placed in header padding.
       Layout then requires that section to fit between the structural headers and the 512-byte header alignment. */
    bool pack_headers;
    /* import_dlls is the fixed arena array of DLLs requested with /import.
       It stays NULL until the first import is added, then holds MAX_IMPORT_DLLS slots. */
    ImportDll *import_dlls;
    /* import_dll_count is how many ImportDll entries are in use.
       Zero skips import layout. Otherwise .idata is synthesized and an input .idata is rejected. */
    uint16_t import_dll_count;
    /* import_output points at the synthesized .idata section, or NULL when there are no imports.
       Its virtual address plus iat_start is the IAT directory RVA. */
    OutputSection *import_output;
    /* text_output points at the .text section that holds import jump thunks.
       prepare_imports creates .text if the object did not already have one. */
    OutputSection *text_output;
    /* import_descriptor_size is the byte size of the import-descriptor array, including the null sentinel.
       It is (DLL count + 1) times 20, and that size is what the import data directory reports. */
    uint32_t import_descriptor_size;
    /* iat_start is the byte offset of the first IAT within .idata.
       The IAT data directory RVA is the .idata RVA plus this offset. */
    uint32_t iat_start;
    /* iat_size is the byte length of every IAT, including each DLL's terminating null pointer.
       The IAT data directory uses this length. */
    uint32_t iat_size;
    /* entry_name is the symbol name chosen for AddressOfEntryPoint.
       It may point at caller storage, a static default, or into entry_storage. */
    const char *entry_name;
    /* entry_storage keeps the arena copy of a .drectve buffer when the entry name points into that buffer.
       It is NULL when the name is a caller string or one of the static default names. */
    char *entry_storage;
    /* entry_rva is the entry point as an RVA in bytes from the image base.
       select_entry writes it, and the optional header copies it to AddressOfEntryPoint. */
    uint32_t entry_rva;
} Linker;

/* linker_is_64_bit reports whether the object is AMD64 rather than I386.
   It reads linker->machine and writes nothing.
   It does not call fail(). An unsupported machine has already been rejected by parse_object. */
static bool linker_is_64_bit(const Linker *linker)
{
    return linker->machine == IMAGE_FILE_MACHINE_AMD64;
}

/* optional_header_size returns the PE optional-header size in bytes for this machine.
   It reads the machine through linker_is_64_bit and returns sizeof IMAGE_OPTIONAL_HEADER64 or IMAGE_OPTIONAL_HEADER32.
   It does not call fail() and has no error return. */
static uint32_t optional_header_size(const Linker *linker)
{
    return linker_is_64_bit(linker)
               ? (uint32_t)sizeof(IMAGE_OPTIONAL_HEADER64)
               : (uint32_t)sizeof(IMAGE_OPTIONAL_HEADER32);
}

/* import_pointer_size returns the ILT and IAT slot size in bytes.
   AMD64 uses 8 and I386 uses 4. It reads only the machine and does not call fail(). */
static uint32_t import_pointer_size(const Linker *linker)
{
    return linker_is_64_bit(linker) ? 8U : 4U;
}

/* image_base returns the preferred load address in bytes for this machine.
   AMD64 uses 0x140000000 and I386 uses 0x00400000. It does not call fail(). */
static uint64_t image_base(const Linker *linker)
{
    return linker_is_64_bit(linker) ? IMAGE_BASE64 : IMAGE_BASE32;
}

/* range_valid tests whether a half-open byte range sits inside a buffer of total bytes.
   It reads offset, size, and total, and returns false on wrap or on a range that passes the end.
   It does not call fail(). Callers turn a false result into fail(), which does not return. */
static bool range_valid(size_t offset, size_t size, size_t total)
{
    return offset <= total && size <= total - offset;
}

/* align_up_u32 rounds value up to a power-of-two alignment and returns the result.
   It reads value and alignment. The result is computed in 64 bits so the mask cannot wrap a uint32_t.
   A zero or non-power-of-two alignment, or a result past 4 GiB minus one, calls fail() and does not return. */
static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    uint64_t result;

    /* Reject an alignment that is zero or not a power of two. */
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "internal error: invalid alignment ");
        write_u(g_error_text, sizeof g_error_text, used, alignment);
        fail(g_error_text);
    }
    result = ((uint64_t)value + alignment - 1) & ~((uint64_t)alignment - 1);
    /* Reject a rounded value that does not fit in a 32-bit RVA or file offset. */
    crash_if(result > UINT32_MAX, "image layout exceeds 4 GiB");
    return (uint32_t)result;
}

/* read_u16 loads a little-endian uint16_t from an address that may be unaligned.
   It reads two bytes through memcpy and returns them in host order, which on this target is little-endian.
   It does not call fail() and does not check that the address is in range. */
static uint16_t read_u16(const void *address)
{
    uint16_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

/* read_u32 loads a little-endian uint32_t from an address that may be unaligned.
   It reads four bytes through memcpy and returns them. It does not call fail(). */
static uint32_t read_u32(const void *address)
{
    uint32_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

/* read_u64 loads a little-endian uint64_t from an address that may be unaligned.
   It reads eight bytes through memcpy and returns them. It does not call fail(). */
static uint64_t read_u64(const void *address)
{
    uint64_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

/* write_u16 stores a uint16_t at an address that may be unaligned.
   It writes two little-endian bytes through memcpy. It does not call fail(). */
static void write_u16(void *address, uint16_t value)
{
    memcpy(address, &value, sizeof(value));
}

/* write_u32 stores a uint32_t at an address that may be unaligned.
   It writes four little-endian bytes through memcpy. It does not call fail(). */
static void write_u32(void *address, uint32_t value)
{
    memcpy(address, &value, sizeof(value));
}

/* write_u64 stores a uint64_t at an address that may be unaligned.
   It writes eight little-endian bytes through memcpy. It does not call fail(). */
static void write_u64(void *address, uint64_t value)
{
    memcpy(address, &value, sizeof(value));
}

/* coff_string returns a NUL-terminated name from the COFF string table.
   It reads linker->string_table and string_table_size at the given byte offset. kind is only used in diagnostics.
   An offset outside the table, or a name with no NUL before the table ends, calls fail() and does not return. */
static const char *coff_string(Linker *linker, uint32_t offset, const char *kind)
{
    const char *text;
    size_t remaining;

    /* Reject an offset that lands in the size dword or past the table. */
    if (offset < 4 || offset >= linker->string_table_size) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, kind);
        used = write_text(g_error_text, sizeof g_error_text, used, " string-table offset ");
        used = write_u(g_error_text, sizeof g_error_text, used, offset);
        write_text(g_error_text, sizeof g_error_text, used, " is invalid");
        fail(g_error_text);
    }
    text = (const char *)linker->string_table + offset;
    remaining = linker->string_table_size - offset;
    /* Reject a name that is not terminated inside the remaining table bytes. */
    if (memchr(text, '\0', remaining) == NULL) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, kind);
        used = write_text(g_error_text, sizeof g_error_text, used, " name at string-table offset ");
        used = write_u(g_error_text, sizeof g_error_text, used, offset);
        write_text(g_error_text, sizeof g_error_text, used, " is not terminated");
        fail(g_error_text);
    }
    return text;
}

/* section_name decodes one COFF section header name into a NUL-terminated buffer.
   It reads the eight name bytes and, when they start with a slash, the decimal string-table offset that follows.
   A bad offset, a missing string, or a name that does not fit the buffer calls fail() and does not return. */
static void section_name(Linker *linker, IMAGE_SECTION_HEADER *header,
                         char *buffer, size_t buffer_size)
{
    char short_name[9];

    memcpy(short_name, header->Name, 8);
    short_name[8] = '\0';
    /* A leading slash means the rest of the short name is a string-table offset. */
    if (short_name[0] == '/') {
        char *end;
        unsigned long offset = strtoul(short_name + 1, &end, 10);
        const char *long_name;

        /* Reject a suffix that is not a complete decimal offset in range. */
        if (*end != '\0' || offset > UINT32_MAX) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "invalid long section name '");
            used = write_text(g_error_text, sizeof g_error_text, used, short_name);
            write_text(g_error_text, sizeof g_error_text, used, "'");
            fail(g_error_text);
        }
        long_name = coff_string(linker, (uint32_t)offset, "section");
        /* Reject a long name that would not fit in the caller buffer with its NUL. */
        if (strlen(long_name) >= buffer_size) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "section name '");
            used = write_text(g_error_text, sizeof g_error_text, used, long_name);
            write_text(g_error_text, sizeof g_error_text, used, "' is too long");
            fail(g_error_text);
        }
        strcpy(buffer, long_name);
    /* Otherwise the eight header bytes are the section name itself. */
    } else {
        /* Reject a short name that would not fit in the caller buffer with its NUL. */
        crash_if(strlen(short_name) >= buffer_size, "section name is too long");
        strcpy(buffer, short_name);
    }
}

/* symbol_name returns the NUL-terminated name of one COFF symbol.
   An inline name is copied into the nine-byte buffer. A string-table name is returned directly from coff_string.
   A bad string-table offset calls fail() and does not return. The inline path does not fail. */
static const char *symbol_name(Linker *linker, IMAGE_SYMBOL *symbol,
                               char buffer[9])
{
    /* Copy the eight inline name bytes when the symbol does not use the string table. */
    if (symbol->N.Name.Short != 0) {
        memcpy(buffer, symbol->N.ShortName, 8);
        buffer[8] = '\0';
        return buffer;
    }
    return coff_string(linker, symbol->N.Name.Long, "symbol");
}

/* starts_with reports whether text begins with prefix, comparing raw bytes.
   It reads both strings through strncmp and strlen. It does not call fail() and returns false on a mismatch. */
static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* section_is_discarded decides whether a COFF section is dropped before linking.
   It reads the section name and the characteristic mask. Directive, debug, LLVM, and IMAGE_SCN_LNK_REMOVE sections are discarded.
   It does not call fail() and does not modify the linker. */
static bool section_is_discarded(const char *name, uint32_t characteristics)
{
    return strcmp(name, ".drectve") == 0 ||
           starts_with(name, ".debug") ||
           starts_with(name, ".llvm") ||
           (characteristics & IMAGE_SCN_LNK_REMOVE) != 0;
}

/* canonical_section_name strips a COFF dollar suffix so grouped sections merge.
   It reads input, writes at most eight characters plus a NUL into output, and stops at the first dollar sign.
   An empty stem or a stem longer than eight characters calls fail() and does not return, because a PE name cannot hold it. */
static void canonical_section_name(const char *input, char output[9])
{
    const char *dollar = strchr(input, '$');
    size_t length = dollar == NULL ? strlen(input) : (size_t)(dollar - input);

    /* Reject a name that is empty or longer than the PE section-name field. */
    if (length == 0 || length > 8) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "section name '");
        used = write_text(g_error_text, sizeof g_error_text, used, input);
        write_text(g_error_text, sizeof g_error_text, used, "' cannot be represented in a PE image");
        fail(g_error_text);
    }
    memcpy(output, input, length);
    output[length] = '\0';
}

/* section_alignment decodes the COFF alignment nibble in a section characteristic mask.
   The mask is shifted down by 20. Zero means 1-byte alignment. Values 1 through 14 mean 2 to the power of encoded-1.
   An encoding above 14 calls fail() and does not return. */
static uint32_t section_alignment(uint32_t characteristics)
{
    uint32_t encoded = (characteristics & IMAGE_SCN_ALIGN_MASK) >> 20;

    /* COFF uses zero to mean the default one-byte alignment. */
    if (encoded == 0) {
        return 1;
    }
    /* Reject an alignment code that is outside the defined COFF range. */
    if (encoded > 14) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "invalid COFF section alignment encoding ");
        write_u(g_error_text, sizeof g_error_text, used, encoded);
        fail(g_error_text);
    }
    return UINT32_C(1) << (encoded - 1);
}

/* find_or_add_output returns the merged section with this canonical name, creating it if needed.
   It reads the output list and, on a miss, writes the name into the next free slot and increments output_count.
   The new slot is already zeroed by allocate. Running out of capacity calls fail() and does not return. */
static OutputSection *find_or_add_output(Linker *linker, const char name[9])
{
    uint16_t index;

    /* Search the sections that have already been created. */
    for (index = 0; index < linker->output_count; ++index) {
        /* Return the existing section when the canonical name matches. */
        if (strcmp(linker->outputs[index].name, name) == 0) {
            return &linker->outputs[index];
        }
    }
    /* Refuse to create a section past the array reserved at parse time. */
    crash_if(linker->output_count == linker->output_capacity, "internal error: output section capacity exceeded");
    strcpy(linker->outputs[linker->output_count].name, name);
    ++linker->output_count;
    return &linker->outputs[linker->output_count - 1];
}

/* find_output looks up a merged section by its canonical name.
   It reads the output list and returns the matching slot, or NULL when the name was never created.
   It does not call fail() and does not add a section. */
static OutputSection *find_output(Linker *linker, const char *name)
{
    uint16_t index;

    /* Search the sections that have already been created. */
    for (index = 0; index < linker->output_count; ++index) {
        /* Return the existing section when the name matches. */
        if (strcmp(linker->outputs[index].name, name) == 0) {
            return &linker->outputs[index];
        }
    }
    return NULL;
}

/* append_import_symbol adds one imported function name to a DLL.
   It reads every import already recorded and rejects a duplicate name.
   The name is copied with _strdup. A duplicate, a full symbol table, or a failed copy calls fail() and does not return. */
static void append_import_symbol(Linker *linker, ImportDll *dll,
                                 const char *name)
{
    uint16_t dll_index;
    uint16_t symbol_index;

    /* Scan every DLL so the same symbol cannot be imported twice. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        ImportDll *candidate = &linker->import_dlls[dll_index];

        /* Scan the symbols already recorded for this DLL. */
        for (symbol_index = 0; symbol_index < candidate->symbol_count;
             ++symbol_index) {
            /* Reject a function name that was already declared. */
            if (strcmp(candidate->symbols[symbol_index].name, name) == 0) {
                size_t used = 0;
                used = write_text(g_error_text, sizeof g_error_text, used, "import symbol '");
                used = write_text(g_error_text, sizeof g_error_text, used, name);
                write_text(g_error_text, sizeof g_error_text, used, "' was declared more than once");
                fail(g_error_text);
            }
        }
    }
    /* Reject another symbol once this DLL's fixed table is full. */
    if (dll->symbol_count == MAX_IMPORT_SYMBOLS) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "too many symbols imported from '");
        used = write_text(g_error_text, sizeof g_error_text, used, dll->name);
        write_text(g_error_text, sizeof g_error_text, used, "'");
        fail(g_error_text);
    }
    dll->symbols[dll->symbol_count].name = _strdup(name);
    /* Reject a symbol whose name copy was not allocated. */
    crash_if(dll->symbols[dll->symbol_count].name == NULL, "out of memory reading import symbol");
    ++dll->symbol_count;
}

/* ensure_import_tables allocates the fixed DLL and symbol tables the first time an import is added.
   It writes linker->import_dlls and each DLL's symbols pointer. A later call finds the tables and returns.
   The DLL table has MAX_IMPORT_DLLS slots, and each symbols pointer addresses MAX_IMPORT_SYMBOLS entries in one shared block.
   allocate exits if the arena cannot hold the tables, so this function does not return on failure. */
static void ensure_import_tables(Linker *linker)
{
    ImportSymbol *symbols;
    uint16_t index;

    /* The tables were allocated by an earlier import. */
    if (linker->import_dlls != NULL) return;
    linker->import_dlls = allocate((size_t)MAX_IMPORT_DLLS * sizeof(*linker->import_dlls));
    symbols = allocate((size_t)MAX_IMPORT_DLLS * (size_t)MAX_IMPORT_SYMBOLS * sizeof(*symbols));
    /* Give each DLL its own slice of the symbol block. */
    for (index = 0; index < MAX_IMPORT_DLLS; ++index) {
        linker->import_dlls[index].symbols =
            symbols + (size_t)index * MAX_IMPORT_SYMBOLS;
    }
}

/* add_import parses one /import:dll,symbol[,symbol...] specification and records it.
   It copies the text, splits on commas, finds or creates the DLL case-insensitively, and appends each symbol.
   A missing comma, an empty symbol, too many DLLs, or an allocation failure calls fail() and does not return. The specification copy stays in the arena until the next reset. */
static void add_import(Linker *linker, const char *specification)
{
    char *copy = _strdup(specification);
    char *comma;
    char *symbol;
    ImportDll *dll = NULL;
    uint16_t dll_index;

    /* Reserve the fixed import tables before the first DLL is recorded. */
    ensure_import_tables(linker);
    /* Stop when the specification could not be copied. */
    crash_if(copy == NULL, "out of memory reading /import");
    comma = strchr(copy, ',');
    /* Reject a specification that has no DLL, no comma, or nothing after the comma. */
    if (comma == NULL || comma == copy || comma[1] == '\0') {
        fail("/import must have the form /import:<dll>,<symbol>[,<symbol>...]");
    }
    *comma = '\0';

    /* Look for a DLL that was already imported under the same name. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        /* Reuse the existing DLL when the names match regardless of case. */
        if (_stricmp(linker->import_dlls[dll_index].name, copy) == 0) {
            dll = &linker->import_dlls[dll_index];
            break;
        }
    }
    /* Create a new DLL entry when this name has not been seen. */
    if (dll == NULL) {
        /* Reject another DLL once the fixed table is full. */
        if (linker->import_dll_count == MAX_IMPORT_DLLS) {
            fail("too many imported DLLs");
        }
        dll = &linker->import_dlls[linker->import_dll_count++];
        dll->name = _strdup(copy);
        /* Stop when the DLL name could not be copied. */
        if (dll->name == NULL) {
            fail("out of memory reading import DLL name");
        }
    }

    symbol = comma + 1;
    /* Walk each comma-separated symbol that follows the DLL name. */
    for (;;) {
        char *next = strchr(symbol, ',');

        /* Cut the current symbol off at the next comma. */
        if (next != NULL) {
            *next = '\0';
        }
        /* Reject an empty symbol between commas or at the end of the list. */
        if (*symbol == '\0') {
            fail("/import contains an empty symbol name");
        }
        append_import_symbol(linker, dll, symbol);
        /* Stop after the last symbol, which has no trailing comma. */
        if (next == NULL) {
            break;
        }
        symbol = next + 1;
    }
}

/* find_import searches every DLL for an exact imported function name.
   It reads the import tables and returns the matching ImportSymbol, or NULL when the name was not declared.
   It does not call fail() and does not apply x86 decoration rules. */
static ImportSymbol *find_import(Linker *linker, const char *name)
{
    uint16_t dll_index;
    uint16_t symbol_index;

    /* Visit each imported DLL. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        ImportDll *dll = &linker->import_dlls[dll_index];

        /* Visit each symbol declared for this DLL. */
        for (symbol_index = 0; symbol_index < dll->symbol_count; ++symbol_index) {
            /* Return the symbol whose declared name matches exactly. */
            if (strcmp(dll->symbols[symbol_index].name, name) == 0) {
                return &dll->symbols[symbol_index];
            }
        }
    }
    return NULL;
}

/* x86_decorated_name_matches compares a decorated COFF name with an import name.
   It ignores one leading underscore and stops the decorated name at an @ stdcall suffix, then compares that stem.
   It reads both strings and returns true on an equal stem. It does not call fail(). */
static bool x86_decorated_name_matches(const char *decorated,
                                       const char *import_name)
{
    const char *name = decorated;
    const char *suffix;
    size_t length;

    /* Skip one leading underscore used by 32-bit cdecl and stdcall decoration. */
    if (*name == '_') {
        ++name;
    }
    suffix = strchr(name, '@');
    length = suffix == NULL ? strlen(name) : (size_t)(suffix - name);
    return strlen(import_name) == length &&
           strncmp(name, import_name, length) == 0;
}

/* find_symbol_import resolves a COFF symbol name to a declared import.
   A __imp_ prefix selects the IAT and is stripped before the search. AMD64 requires an exact name. I386 also tries decorated stems.
   It writes *uses_iat. It returns NULL when nothing matches and does not call fail(). */
static ImportSymbol *find_symbol_import(Linker *linker, const char *name,
                                        bool *uses_iat)
{
    ImportSymbol *import;
    const char *candidate = name;

    *uses_iat = false;
    /* Treat a __imp_ prefix as a request for the IAT slot rather than the jump thunk. */
    if (starts_with(name, "__imp_")) {
        *uses_iat = true;
        candidate = name + 6;
    }
    import = find_import(linker, candidate);
    /* Keep an exact match, and do not try 32-bit decoration on AMD64. */
    if (import != NULL || linker_is_64_bit(linker)) {
        return import;
    }

    {
        uint16_t dll_index;
        uint16_t symbol_index;

        /* Scan each DLL for a decorated 32-bit match. */
        for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
            ImportDll *dll = &linker->import_dlls[dll_index];

            /* Scan each declared import in this DLL. */
            for (symbol_index = 0; symbol_index < dll->symbol_count;
                 ++symbol_index) {
                /* Return the import whose plain name matches the decorated COFF stem. */
                if (x86_decorated_name_matches(
                        candidate, dll->symbols[symbol_index].name)) {
                    return &dll->symbols[symbol_index];
                }
            }
        }
    }
    return NULL;
}

/* prepare_imports lays out jump thunks, import descriptors, lookup tables, the IAT, and name strings.
   It reads the import list and writes offsets into each DLL and symbol, plus .text and .idata output sections.
   No imports returns immediately. An input .idata, or a size that does not fit in 32 bits, calls fail() and does not return. */
static void prepare_imports(Linker *linker)
{
    uint32_t cursor;
    uint32_t pointer_size = import_pointer_size(linker);
    uint32_t thunk_cursor;
    uint16_t dll_index;
    uint16_t symbol_index;

    /* Skip import layout when the user declared no imports. */
    if (linker->import_dll_count == 0) {
        return;
    }
    /* Refuse to synthesize imports on top of an object-defined .idata section. */
    crash_if(find_output(linker, ".idata") != NULL, "input-defined .idata cannot be combined with /import");
    linker->text_output = find_or_add_output(linker, ".text");
    linker->text_output->characteristics |=
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    /* Force .text alignment to at least 16 before placing thunks. */
    if (linker->text_output->alignment < 16) {
        linker->text_output->alignment = 16;
    }
    thunk_cursor = align_up_u32(linker->text_output->virtual_size, 16);
    /* Assign a six-byte thunk in .text to every imported symbol. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        ImportDll *dll = &linker->import_dlls[dll_index];

        /* Place the next thunk immediately after the previous one. */
        for (symbol_index = 0; symbol_index < dll->symbol_count; ++symbol_index) {
            dll->symbols[symbol_index].thunk_offset = thunk_cursor;
            /* Reject a thunk list that would pass the end of a 32-bit section. */
            crash_if(thunk_cursor > UINT32_MAX - 6, "import thunks are too large");
            thunk_cursor += 6;
        }
    }
    linker->text_output->virtual_size = thunk_cursor;

    linker->import_output = find_or_add_output(linker, ".idata");
    linker->import_output->characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    linker->import_output->alignment = pointer_size;
    linker->import_descriptor_size =
        (uint32_t)(linker->import_dll_count + 1) *
        (uint32_t)sizeof(IMAGE_IMPORT_DESCRIPTOR);
    cursor = linker->import_descriptor_size;

    /* Lay out each DLL's import lookup table after the descriptor array. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        ImportDll *dll = &linker->import_dlls[dll_index];

        cursor = align_up_u32(cursor, pointer_size);
        dll->ilt_offset = cursor;
        /* Give each symbol an ILT slot at its index times the pointer size. */
        for (symbol_index = 0; symbol_index < dll->symbol_count; ++symbol_index) {
            dll->symbols[symbol_index].ilt_offset =
                cursor + (uint32_t)symbol_index * pointer_size;
        }
        cursor += ((uint32_t)dll->symbol_count + 1) * pointer_size;
    }
    cursor = align_up_u32(cursor, pointer_size);
    linker->iat_start = cursor;
    /* Lay out each DLL's import address table after the lookup tables. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        ImportDll *dll = &linker->import_dlls[dll_index];

        dll->iat_offset = cursor;
        /* Give each symbol an IAT slot at its index times the pointer size. */
        for (symbol_index = 0; symbol_index < dll->symbol_count; ++symbol_index) {
            dll->symbols[symbol_index].iat_offset =
                cursor + (uint32_t)symbol_index * pointer_size;
        }
        cursor += ((uint32_t)dll->symbol_count + 1) * pointer_size;
    }
    linker->iat_size = cursor - linker->iat_start;

    /* Place hint/name entries and the DLL name after the address tables. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        ImportDll *dll = &linker->import_dlls[dll_index];

        /* Reserve a 2-byte-aligned hint word plus the symbol name and its NUL. */
        for (symbol_index = 0; symbol_index < dll->symbol_count; ++symbol_index) {
            size_t name_length = strlen(dll->symbols[symbol_index].name);

            cursor = align_up_u32(cursor, 2);
            dll->symbols[symbol_index].hint_name_offset = cursor;
            /* Reject a hint/name entry that would overflow a 32-bit offset. */
            crash_if(name_length > UINT32_MAX - cursor - 3, "import names are too large");
            cursor += (uint32_t)name_length + 3;
        }
        dll->name_offset = cursor;
        /* Reject a DLL name that would overflow a 32-bit offset. */
        crash_if(strlen(dll->name) > UINT32_MAX - cursor - 1, "import DLL names are too large");
        cursor += (uint32_t)strlen(dll->name) + 1;
    }
    linker->import_output->virtual_size = cursor;
}

/* parse_object reads one COFF object and merges its live sections into output sections.
   It reads the file header, section headers, symbol table, and string table from linker->file_data, then fills inputs and outputs.
   A truncated header, an unsupported machine, an optional header, missing symbols, a bad string table, or an oversized section calls fail() and does not return. */
static void parse_object(Linker *linker)
{
    size_t section_offset;
    size_t symbol_bytes;
    size_t string_offset;
    uint16_t index;

    /* Reject a file too small to hold the COFF header. */
    crash_if(!range_valid(0, sizeof(IMAGE_FILE_HEADER), linker->file_size), "input is too small to be a COFF object");
    linker->file_header = (IMAGE_FILE_HEADER *)linker->file_data;
    linker->machine = linker->file_header->Machine;
    /* Reject a machine other than I386 or AMD64. */
    if (linker->machine != IMAGE_FILE_MACHINE_I386 &&
        linker->machine != IMAGE_FILE_MACHINE_AMD64) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "unsupported COFF machine 0x");
        used = write_x(g_error_text, sizeof g_error_text, used, linker->file_header->Machine, 4);
        write_text(g_error_text, sizeof g_error_text, used, "; only I386 and AMD64 are supported");
        fail(g_error_text);
    }
    /* Reject an image or import library that is not a relocatable COFF object. */
    crash_if(linker->file_header->SizeOfOptionalHeader != 0, "input has an optional header and is not a COFF object");
    /* Reject an object that declares no sections. */
    crash_if(linker->file_header->NumberOfSections == 0, "input contains no sections");

    section_offset = sizeof(IMAGE_FILE_HEADER);
    /* Reject a section table that extends past the end of the file. */
    crash_if(!range_valid(section_offset,
                     (size_t)linker->file_header->NumberOfSections *
                         sizeof(IMAGE_SECTION_HEADER),
                     linker->file_size), "COFF section table extends beyond the input");
    linker->section_headers =
        (IMAGE_SECTION_HEADER *)(linker->file_data + section_offset);

    /* Reject an object that has no symbol table. */
    crash_if(linker->file_header->PointerToSymbolTable == 0 ||
        linker->file_header->NumberOfSymbols == 0, "input has no COFF symbol table");
    symbol_bytes = (size_t)linker->file_header->NumberOfSymbols *
                   sizeof(IMAGE_SYMBOL);
    /* Reject a symbol table that extends past the end of the file. */
    crash_if(!range_valid(linker->file_header->PointerToSymbolTable, symbol_bytes,
                     linker->file_size), "COFF symbol table extends beyond the input");
    linker->symbols = (IMAGE_SYMBOL *)(linker->file_data +
                                      linker->file_header->PointerToSymbolTable);
    string_offset = linker->file_header->PointerToSymbolTable + symbol_bytes;
    /* Reject an object whose string table has no leading size dword. */
    crash_if(!range_valid(string_offset, sizeof(uint32_t), linker->file_size), "COFF string table is missing");
    linker->string_table = linker->file_data + string_offset;
    linker->string_table_size = read_u32(linker->string_table);
    /* Reject a string-table size that is below four bytes or past the end of the file. */
    crash_if(linker->string_table_size < 4 ||
        !range_valid(string_offset, linker->string_table_size, linker->file_size), "COFF string table has an invalid size");

    linker->inputs = allocate((size_t)linker->file_header->NumberOfSections *
                              sizeof(*linker->inputs));
    linker->output_capacity =
        (uint16_t)(linker->file_header->NumberOfSections + 2);
    /* Reject a section count whose plus-two capacity wrapped uint16_t. */
    crash_if(linker->output_capacity < linker->file_header->NumberOfSections, "too many COFF sections");
    linker->outputs = allocate((size_t)linker->output_capacity *
                               sizeof(*linker->outputs));

    /* Classify each input section and append live ones to a merged output section. */
    for (index = 0; index < linker->file_header->NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER *header = &linker->section_headers[index];
        InputSection *input = &linker->inputs[index];
        char name[256];
        char canonical[9];
        uint32_t alignment;

        input->header = header;
        input->size = header->SizeOfRawData;
        section_name(linker, header, name, sizeof(name));
        input->discarded = input->size == 0 ||
                           section_is_discarded(name, header->Characteristics);
        /* Leave a discarded section out of the output image. */
        if (input->discarded) {
            continue;
        }
        /* Reject a section that carries COFF line-number records. */
        if (header->NumberOfLinenumbers != 0) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "COFF line-number records in section '");
            used = write_text(g_error_text, sizeof g_error_text, used, name);
            write_text(g_error_text, sizeof g_error_text, used, "' are not supported");
            fail(g_error_text);
        }
        /* Reject initialized raw bytes that do not fit in the file. */
        if ((header->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) == 0 &&
            input->size != 0 &&
            !range_valid(header->PointerToRawData, input->size,
                         linker->file_size)) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "raw data for section '");
            used = write_text(g_error_text, sizeof g_error_text, used, name);
            write_text(g_error_text, sizeof g_error_text, used, "' extends beyond the input");
            fail(g_error_text);
        }
        /* Reject a relocation array that does not fit in the file. */
        if (header->NumberOfRelocations != 0 &&
            !range_valid(header->PointerToRelocations,
                         (size_t)header->NumberOfRelocations *
                             sizeof(IMAGE_RELOCATION),
                         linker->file_size)) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "relocations for section '");
            used = write_text(g_error_text, sizeof g_error_text, used, name);
            write_text(g_error_text, sizeof g_error_text, used, "' extend beyond the input");
            fail(g_error_text);
        }
        canonical_section_name(name, canonical);
        input->output = find_or_add_output(linker, canonical);
        alignment = section_alignment(header->Characteristics);
        /* Raise the output alignment to the strictest live contribution. */
        if (alignment > input->output->alignment) {
            input->output->alignment = alignment;
        }
        input->output_offset =
            align_up_u32(input->output->virtual_size, alignment);
        /* Reject a contribution that would make the output section larger than 4 GiB minus one. */
        if (input->size > UINT32_MAX - input->output_offset) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "section '");
            used = write_text(g_error_text, sizeof g_error_text, used, name);
            write_text(g_error_text, sizeof g_error_text, used, "' is too large");
            fail(g_error_text);
        }
        input->output->virtual_size = input->output_offset + input->size;
        input->output->characteristics |=
            header->Characteristics &
            (IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA |
             IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_DISCARDABLE |
             IMAGE_SCN_MEM_NOT_CACHED | IMAGE_SCN_MEM_NOT_PAGED |
             IMAGE_SCN_MEM_SHARED | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ |
             IMAGE_SCN_MEM_WRITE);
    }
    /* Reject an object whose every section was discarded. */
    crash_if(linker->output_count == 0, "input contains no linkable sections");
}

/*
 * Resolve one COFF symbol to an RVA in the output image. The symbol-table
 * entry at symbol_index is read, and its section number selects the result.
 * When target_output is not null, the output section that holds the symbol
 * is written there; null is written for an absolute symbol. A defined symbol
 * yields its output RVA plus the contribution offset plus the symbol value.
 * An undefined name that matches an import yields the IAT slot when the
 * symbol uses the __imp_ form, and the .text thunk RVA otherwise. An absolute
 * symbol yields its raw value. fail() reports a bad index, an unresolved
 * external, an unsupported section number, a discarded section, or a value
 * past the contribution. fail() prints the error and exits.
 */
static uint32_t symbol_rva(Linker *linker, uint32_t symbol_index,
                           OutputSection **target_output)
{
    IMAGE_SYMBOL *symbol;
    InputSection *input;
    char short_name[9];
    const char *name;

    /* Reject a symbol index past the COFF symbol table. */
    if (symbol_index >= linker->file_header->NumberOfSymbols) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "relocation references invalid symbol index ");
        write_u(g_error_text, sizeof g_error_text, used, symbol_index);
        fail(g_error_text);
    }
    symbol = &linker->symbols[symbol_index];
    name = symbol_name(linker, symbol, short_name);
    /* Resolve an undefined symbol through the import tables. */
    if (symbol->SectionNumber == IMAGE_SYM_UNDEFINED) {
        ImportSymbol *import;
        bool uses_iat;

        import = find_symbol_import(linker, name, &uses_iat);
        /* This name is an import, so its address lives in the image. */
        if (import != NULL) {
            /* An __imp_ symbol refers to the IAT slot rather than the jump thunk. */
            if (uses_iat) {
                /* Report the import section when the caller asked for a target. */
                if (target_output != NULL) {
                    *target_output = linker->import_output;
                }
                return linker->import_output->virtual_address +
                       import->iat_offset;
            }
            /* A direct reference uses the ff 25 thunk in .text. */
            if (target_output != NULL) {
                *target_output = linker->text_output;
            }
            return linker->text_output->virtual_address +
                   import->thunk_offset;
        }
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "unresolved external symbol '");
        used = write_text(g_error_text, sizeof g_error_text, used, name);
        write_text(g_error_text, sizeof g_error_text, used, "'");
        fail(g_error_text);
    }
    /* An absolute symbol value is already final and is not an RVA. */
    if (symbol->SectionNumber == IMAGE_SYM_ABSOLUTE) {
        /* Absolute symbols do not belong to an output section. */
        if (target_output != NULL) {
            *target_output = NULL;
        }
        return symbol->Value;
    }
    /* Section numbers are 1-based and must name a real input section. */
    if (symbol->SectionNumber <= 0 ||
        (uint16_t)symbol->SectionNumber > linker->file_header->NumberOfSections) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "symbol '");
        used = write_text(g_error_text, sizeof g_error_text, used, name);
        used = write_text(g_error_text, sizeof g_error_text, used, "' has unsupported section number ");
        write_d(g_error_text, sizeof g_error_text, used, symbol->SectionNumber);
        fail(g_error_text);
    }
    input = &linker->inputs[symbol->SectionNumber - 1];
    /* A discarded contribution has no bytes in the image. */
    if (input->discarded || input->output == NULL) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "symbol '");
        used = write_text(g_error_text, sizeof g_error_text, used, name);
        write_text(g_error_text, sizeof g_error_text, used, "' refers to a discarded section");
        fail(g_error_text);
    }
    /* The symbol value is an offset and must fall inside the contribution. */
    if (symbol->Value > input->size) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "symbol '");
        used = write_text(g_error_text, sizeof g_error_text, used, name);
        write_text(g_error_text, sizeof g_error_text, used, "' lies beyond its section");
        fail(g_error_text);
    }
    /* Report the output section that received this contribution. */
    if (target_output != NULL) {
        *target_output = input->output;
    }
    return input->output->virtual_address + input->output_offset + symbol->Value;
}

/*
 * Find a defined COFF symbol whose name matches wanted. The symbol table
 * is walked one primary symbol at a time, and auxiliary records are skipped
 * using NumberOfAuxSymbols. On I386 a leading underscore and a trailing @N
 * decoration still count as a match. When index_out is not null, the matching
 * symbol's table index is written there. The symbol is returned, or null when
 * no defined match exists. fail() reports auxiliary records that run past the
 * symbol table. fail() prints the error and exits.
 */
static IMAGE_SYMBOL *find_defined_symbol(Linker *linker, const char *wanted,
                                         uint32_t *index_out)
{
    uint32_t index = 0;

    /* Visit each primary symbol, stepping over its auxiliary records. */
    while (index < linker->file_header->NumberOfSymbols) {
        IMAGE_SYMBOL *symbol = &linker->symbols[index];
        char short_name[9];
        const char *name = symbol_name(linker, symbol, short_name);
        uint32_t next = index + 1U + symbol->NumberOfAuxSymbols;

        /* Auxiliary records must stay inside the symbol table. */
        if (next > linker->file_header->NumberOfSymbols) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "symbol '");
            used = write_text(g_error_text, sizeof g_error_text, used, name);
            write_text(g_error_text, sizeof g_error_text, used, "' has auxiliary records beyond the symbol table");
            fail(g_error_text);
        }
        /* Accept an exact name, or an x86 decorated form, of a defined symbol. */
        if ((strcmp(name, wanted) == 0 ||
             (!linker_is_64_bit(linker) &&
              x86_decorated_name_matches(name, wanted))) &&
            symbol->SectionNumber != IMAGE_SYM_UNDEFINED) {
            /* Hand the table index back when the caller asked for it. */
            if (index_out != NULL) {
                *index_out = index;
            }
            return symbol;
        }
        index = next;
    }
    return NULL;
}

/*
 * Read the first /entry: name from a .drectve section. Each input section
 * name is checked, and a non-empty .drectve payload is copied out of the
 * object and split on spaces and tabs. The first token that starts with
 * /entry: and has a name after the colon is duplicated and returned. The
 * section copy stays in the arena. Other tokens and sections are ignored. Null
 * is returned when no such directive exists. fail() reports directive bytes
 * outside the object or a failed duplicate. fail() prints the error and exits.
 */
static char *directive_entry(Linker *linker)
{
    uint16_t index;

    /* Search every input section for a .drectve payload. */
    for (index = 0; index < linker->file_header->NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER *header = &linker->section_headers[index];
        char name[256];
        char *copy;
        char *cursor;

        section_name(linker, header, name, sizeof(name));
        /* Skip sections that are not a non-empty directive section. */
        if (strcmp(name, ".drectve") != 0 || header->SizeOfRawData == 0) {
            continue;
        }
        /* The directive bytes must lie inside the object file. */
        crash_if(!range_valid(header->PointerToRawData, header->SizeOfRawData,
                         linker->file_size), ".drectve data extends beyond the input");
        copy = allocate((size_t)header->SizeOfRawData + 1);
        memcpy(copy, linker->file_data + header->PointerToRawData,
               header->SizeOfRawData);
        cursor = copy;
        /* Split the directive text into whitespace-separated tokens. */
        while (*cursor != '\0') {
            char *token;
            char *end;

            /* Skip spaces and tabs before the next token. */
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            token = cursor;
            /* Read until the next space, tab, or end of the text. */
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
                ++cursor;
            }
            end = cursor;
            /* Terminate this token in the copy when more text follows. */
            if (*cursor != '\0') {
                *cursor++ = '\0';
            }
            /* A /entry: token with a name is the entry symbol. */
            if (_strnicmp(token, "/entry:", 7) == 0 && token[7] != '\0') {
                char *result = _strdup(token + 7);
                /* Duplicating the entry name failed. */
                crash_if(result == NULL, "out of memory reading /ENTRY directive");
                return result;
            }
            (void)end;
        }
    }
    return NULL;
}

/*
 * Assign RVAs, file offsets, and emitted section indexes, and allocate each
 * output section's byte buffer. Every output section's name, characteristics,
 * alignment, and virtual size are read. virtual_address, raw_offset, raw_size,
 * index, in_headers, and a zero-filled data pointer are written.
 * emitted_section_count starts as the output count. When pack_headers is set,
 * the first .rdata that is not writable, executable, or uninitialized is
 * placed in the padding between the unaligned header bytes and the
 * file-aligned SizeOfHeaders. It
 * is omitted from the section table, and that header offset becomes both its
 * RVA and its raw offset. Every other section begins at RVA 4096 and then
 * advances by the section alignment. Uninitialized-only sections receive an
 * RVA but no raw file bytes. Emitted indexes are 1-based and skip the packed
 * section. fail() reports a missing packable .rdata, a .rdata that does not
 * fit in the header padding, or an allocation failure. fail() prints the error and exits.
 */
static void assign_layout(Linker *linker)
{
    uint32_t next_rva = SECTION_ALIGNMENT;
    uint32_t structural_header_size;
    uint32_t header_size;
    uint32_t next_raw;
    OutputSection *packed = NULL;
    uint16_t emitted_index = 0;
    uint16_t index;

    linker->emitted_section_count = linker->output_count;
    /* Pack one read-only .rdata into the header padding when requested. */
    if (linker->pack_headers) {
        /* Stop at the first .rdata that is not writable, executable, or uninitialized. */
        for (index = 0; index < linker->output_count; ++index) {
            OutputSection *output = &linker->outputs[index];

            /* This section is eligible to be stored inside the headers. */
            if (strcmp(output->name, ".rdata") == 0 &&
                (output->characteristics &
                 (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE |
                  IMAGE_SCN_CNT_UNINITIALIZED_DATA)) == 0) {
                packed = output;
                break;
            }
        }
        /* Packing was requested but no suitable .rdata exists. */
        crash_if(packed == NULL, "/pack-headers requires a read-only .rdata section");
        --linker->emitted_section_count;
    }
    structural_header_size =
        DOS_HEADER_SIZE + sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER) +
        optional_header_size(linker) +
        (uint32_t)linker->emitted_section_count * sizeof(IMAGE_SECTION_HEADER);
    header_size =
        align_up_u32(structural_header_size, FILE_ALIGNMENT);
    /* Place the chosen .rdata in the unused bytes before SizeOfHeaders. */
    if (packed != NULL) {
        uint32_t packed_offset =
            align_up_u32(structural_header_size, packed->alignment);

        /* The aligned .rdata does not fit in that header padding. */
        if (packed_offset > header_size ||
            packed->virtual_size > header_size - packed_offset) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, ".rdata needs ");
            used = write_u(g_error_text, sizeof g_error_text, used, packed->virtual_size);
            used = write_text(g_error_text, sizeof g_error_text, used, " bytes but only ");
            used = write_u(g_error_text, sizeof g_error_text, used, packed_offset > header_size ? 0 : header_size - packed_offset);
            write_text(g_error_text, sizeof g_error_text, used, " bytes of header padding are available");
            fail(g_error_text);
        }
        packed->in_headers = true;
        packed->virtual_address = packed_offset;
        packed->raw_offset = packed_offset;
        packed->raw_size = packed->virtual_size;
    }
    next_raw = header_size;
    /* Allocate each buffer, then assign an RVA and raw range to sections that are not packed. */
    for (index = 0; index < linker->output_count; ++index) {
        OutputSection *output = &linker->outputs[index];
        bool uninitialized_only;

        output->data = allocate(output->virtual_size);
        /* The packed section already has its address and is not emitted. */
        if (output->in_headers) {
            continue;
        }
        output->index = ++emitted_index;
        output->virtual_address = next_rva;
        next_rva = align_up_u32(next_rva + output->virtual_size,
                                SECTION_ALIGNMENT);
        uninitialized_only =
            (output->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0 &&
            (output->characteristics &
             (IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA)) == 0;
        /* Initialized content occupies file space aligned to the file alignment. */
        if (!uninitialized_only && output->virtual_size != 0) {
            output->raw_size = align_up_u32(output->virtual_size, FILE_ALIGNMENT);
            output->raw_offset = next_raw;
            next_raw += output->raw_size;
        }
    }
}

/*
 * Copy live input contributions into the output buffers from assign_layout.
 * Each input section's raw pointer and size are read from its COFF header,
 * and that many bytes are written at output_offset in the output data.
 * Discarded sections, empty sections, and uninitialized data are skipped, so
 * those buffers stay zero. This function does not call fail(). A raw range
 * outside the object was already rejected while the object was parsed.
 */
static void copy_section_data(Linker *linker)
{
    uint16_t index;

    /* Copy raw bytes from each kept contribution into its output buffer. */
    for (index = 0; index < linker->file_header->NumberOfSections; ++index) {
        InputSection *input = &linker->inputs[index];

        /* Skip discarded, empty, and uninitialized contributions. */
        if (input->discarded || input->size == 0 ||
            (input->header->Characteristics &
             IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0) {
            continue;
        }
        memcpy(input->output->data + input->output_offset,
               linker->file_data + input->header->PointerToRawData,
               input->size);
    }
}

/*
 * Fill the .idata bytes and the .text import thunks reserved earlier. When
 * no DLLs were imported, the function returns without writing. For each DLL
 * it writes an import descriptor: the ILT RVA at offset 0, the DLL-name RVA
 * at offset 12, and the IAT RVA at offset 16, then copies the DLL name.
 * TimeDateStamp and ForwarderChain stay zero. For each symbol, the hint/name
 * RVA is stored in both the ILT and the IAT, using 8-byte slots on AMD64 and
 * 4-byte slots on I386. A zero hint and the symbol name are copied into the
 * hint/name table. The six-byte thunk in .text is the indirect jump ff 25.
 * On AMD64 the immediate is the RIP-relative displacement from the end of
 * that instruction to the IAT slot. On I386 it is the absolute virtual
 * address of the IAT slot. fail() reports a displacement or address that
 * does not fit in 32 bits. fail() prints the error and exits.
 */
static void build_import_data(Linker *linker)
{
    uint16_t dll_index;
    uint16_t symbol_index;

    /* Nothing to write when the object imports no DLLs. */
    if (linker->import_dll_count == 0) {
        return;
    }
    /* Write one import descriptor, DLL name, and symbol group per DLL. */
    for (dll_index = 0; dll_index < linker->import_dll_count; ++dll_index) {
        ImportDll *dll = &linker->import_dlls[dll_index];
        uint8_t *descriptor =
            linker->import_output->data +
            (uint32_t)dll_index * sizeof(IMAGE_IMPORT_DESCRIPTOR);

        dll->descriptor_offset =
            (uint32_t)dll_index * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        write_u32(descriptor,
                  linker->import_output->virtual_address + dll->ilt_offset);
        write_u32(descriptor + 12,
                  linker->import_output->virtual_address + dll->name_offset);
        write_u32(descriptor + 16,
                  linker->import_output->virtual_address + dll->iat_offset);
        memcpy(linker->import_output->data + dll->name_offset, dll->name,
               strlen(dll->name) + 1);

        /* Fill each symbol's ILT and IAT entries, hint/name, and .text thunk. */
        for (symbol_index = 0; symbol_index < dll->symbol_count; ++symbol_index) {
            ImportSymbol *symbol = &dll->symbols[symbol_index];
            uint32_t hint_name_rva =
                linker->import_output->virtual_address +
                symbol->hint_name_offset;
            uint8_t *thunk =
                linker->text_output->data + symbol->thunk_offset;

            /* AMD64 import slots are 8-byte hint/name RVAs. */
            if (linker_is_64_bit(linker)) {
                write_u64(linker->import_output->data + symbol->ilt_offset,
                          hint_name_rva);
                write_u64(linker->import_output->data + symbol->iat_offset,
                          hint_name_rva);
            /* I386 import slots are 4-byte hint/name RVAs. */
            } else {
                write_u32(linker->import_output->data + symbol->ilt_offset,
                          hint_name_rva);
                write_u32(linker->import_output->data + symbol->iat_offset,
                          hint_name_rva);
            }
            write_u16(linker->import_output->data + symbol->hint_name_offset, 0);
            memcpy(linker->import_output->data + symbol->hint_name_offset + 2,
                   symbol->name, strlen(symbol->name) + 1);
            thunk[0] = 0xff;
            thunk[1] = 0x25;
            /* AMD64 encodes a RIP-relative displacement from the thunk to the IAT slot. */
            if (linker_is_64_bit(linker)) {
                int64_t displacement =
                    (int64_t)(linker->import_output->virtual_address +
                              symbol->iat_offset) -
                    (int64_t)(linker->text_output->virtual_address +
                              symbol->thunk_offset + 6);

                /* The displacement must fit in the thunk's 32-bit immediate. */
                if (displacement < INT32_MIN || displacement > INT32_MAX) {
                    size_t used = 0;
                    used = write_text(g_error_text, sizeof g_error_text, used, "import thunk for '");
                    used = write_text(g_error_text, sizeof g_error_text, used, symbol->name);
                    write_text(g_error_text, sizeof g_error_text, used, "' is out of range");
                    fail(g_error_text);
                }
                write_u32(thunk + 2, (uint32_t)(int32_t)displacement);
            /* I386 stores the absolute virtual address of the IAT slot. */
            } else {
                uint64_t iat_address =
                    image_base(linker) +
                    linker->import_output->virtual_address +
                    symbol->iat_offset;

                /* That address must fit in a 32-bit immediate. */
                if (iat_address > UINT32_MAX) {
                    size_t used = 0;
                    used = write_text(g_error_text, sizeof g_error_text, used, "import thunk address for '");
                    used = write_text(g_error_text, sizeof g_error_text, used, symbol->name);
                    write_text(g_error_text, sizeof g_error_text, used, "' overflows");
                    fail(g_error_text);
                }
                write_u32(thunk + 2, (uint32_t)iat_address);
            }
        }
    }
}

/*
 * Require that a relocation patch lies entirely inside its input section.
 * The relocation virtual address and the caller's width are compared with
 * the contribution size. fail() reports a patch that starts past the section
 * or whose width runs past the end. fail() prints the error and exits.
 */
static void require_patch(InputSection *input, IMAGE_RELOCATION *relocation,
                          uint32_t width)
{
    /* Reject a patch that starts past the section or extends past its end. */
    if (relocation->VirtualAddress > input->size ||
        width > input->size - relocation->VirtualAddress) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "relocation at offset 0x");
        used = write_x(g_error_text, sizeof g_error_text, used, relocation->VirtualAddress, 1);
        write_text(g_error_text, sizeof g_error_text, used, " extends beyond its input section");
        fail(g_error_text);
    }
}

/*
 * Apply every COFF relocation in every live input section to the output
 * bytes. The relocation table at PointerToRelocations is read, each symbol
 * is resolved with symbol_rva, and the patched value is written back into
 * the output buffer at the relocation virtual address. Type 0 is ABSOLUTE
 * on both machines and is skipped, so those bytes stay as they were copied.
 * AMD64 ADDR64 stores a 64-bit virtual address. ADDR32 and ADDR32NB store a
 * 32-bit virtual address or an RVA. REL32 and REL32_1 through REL32_5 store
 * a signed 32-bit displacement from the end of the field, with an extra bias
 * equal to the difference between the type and REL32. I386 DIR32, DIR32NB,
 * and REL32 are the matching 32-bit forms. SECTION adds the 1-based emitted
 * section index to a 16-bit field, and SECREL adds the offset within the
 * target output section to a 32-bit field. Those two type codes are the same
 * on I386 and AMD64, so both machines are patched after the per-machine
 * switch. A symbol whose section was packed into the headers has no emitted
 * index, and SECTION or SECREL against it is rejected. fail() reports an
 * unsupported type, an overflowing result, a patch past the section, or a
 * section-relative target with no output section. fail() prints the error and exits.
 */
static void apply_relocations(Linker *linker)
{
    uint16_t section_index;

    /* Walk every input section that might contain relocations. */
    for (section_index = 0;
         section_index < linker->file_header->NumberOfSections;
         ++section_index) {
        InputSection *input = &linker->inputs[section_index];
        IMAGE_RELOCATION *relocations;
        uint16_t relocation_index;

        /* Skip discarded sections and sections that have no relocations. */
        if (input->discarded || input->header->NumberOfRelocations == 0) {
            continue;
        }
        relocations = (IMAGE_RELOCATION *)(linker->file_data +
                                          input->header->PointerToRelocations);
        /* Apply each relocation record in this section. */
        for (relocation_index = 0;
             relocation_index < input->header->NumberOfRelocations;
             ++relocation_index) {
            IMAGE_RELOCATION *relocation = &relocations[relocation_index];

            /* Leave ABSOLUTE relocations unchanged. Type 0 is absolute on both machines. */
            if (relocation->Type == IMAGE_REL_AMD64_ABSOLUTE) {
                continue;
            }

            uint8_t *patch = input->output->data + input->output_offset +
                             relocation->VirtualAddress;
            OutputSection *target_output = NULL;
            uint32_t target_rva =
                symbol_rva(linker, relocation->SymbolTableIndex, &target_output);
            uint32_t place_rva = input->output->virtual_address +
                                 input->output_offset +
                                 relocation->VirtualAddress;

            /* AMD64 relocation types are applied on this path. */
            if (linker_is_64_bit(linker)) {
                /* Select the AMD64 patch from the relocation type. */
                switch (relocation->Type) {
                /* Store a 64-bit virtual address. */
                case IMAGE_REL_AMD64_ADDR64: {
                    uint64_t value;
                    require_patch(input, relocation, 8);
                    value = read_u64(patch) + image_base(linker) + target_rva;
                    write_u64(patch, value);
                    break;
                }
                /* Store a 32-bit virtual address. */
                case IMAGE_REL_AMD64_ADDR32: {
                    uint64_t value;
                    require_patch(input, relocation, 4);
                    value = (uint64_t)read_u32(patch) + image_base(linker) +
                            target_rva;
                    /* Reject an address that does not fit in the 32-bit slot. */
                    if (value > UINT32_MAX) {
                        size_t used = 0;
                        used = write_text(g_error_text, sizeof g_error_text, used, "ADDR32 relocation overflows at section ");
                        used = write_u(g_error_text, sizeof g_error_text, used, (unsigned)(section_index + 1));
                        used = write_text(g_error_text, sizeof g_error_text, used, " offset 0x");
                        write_x(g_error_text, sizeof g_error_text, used, relocation->VirtualAddress, 1);
                        fail(g_error_text);
                    }
                    write_u32(patch, (uint32_t)value);
                    break;
                }
                /* Store a 32-bit RVA, without adding the image base. */
                case IMAGE_REL_AMD64_ADDR32NB: {
                    uint64_t value;
                    require_patch(input, relocation, 4);
                    value = (uint64_t)read_u32(patch) + target_rva;
                    /* Reject an RVA that does not fit in the 32-bit slot. */
                    crash_if(value > UINT32_MAX, "ADDR32NB relocation overflows");
                    write_u32(patch, (uint32_t)value);
                    break;
                }
                /* Store a signed 32-bit PC-relative displacement, including the REL32_1 through REL32_5 bias. */
                case IMAGE_REL_AMD64_REL32:
                case IMAGE_REL_AMD64_REL32_1:
                case IMAGE_REL_AMD64_REL32_2:
                case IMAGE_REL_AMD64_REL32_3:
                case IMAGE_REL_AMD64_REL32_4:
                case IMAGE_REL_AMD64_REL32_5: {
                    int64_t displacement;
                    uint32_t extra =
                        relocation->Type - IMAGE_REL_AMD64_REL32;
                    require_patch(input, relocation, 4);
                    displacement = (int64_t)(int32_t)read_u32(patch) +
                                   (int64_t)target_rva -
                                   (int64_t)place_rva - 4 - extra;
                    /* Reject a displacement that does not fit in 32 bits. */
                    crash_if(displacement < INT32_MIN ||
                        displacement > INT32_MAX, "REL32 relocation overflows");
                    write_u32(patch, (uint32_t)(int32_t)displacement);
                    break;
                }
                /* SECTION and SECREL are patched after this switch. Their type codes match I386. */
                case IMAGE_REL_AMD64_SECTION:
                case IMAGE_REL_AMD64_SECREL:
                    break;
                /* Any other AMD64 relocation type is not supported. */
                default: {
                    size_t used = 0;
                    used = write_text(g_error_text, sizeof g_error_text, used, "unsupported AMD64 relocation type 0x");
                    used = write_x(g_error_text, sizeof g_error_text, used, relocation->Type, 4);
                    used = write_text(g_error_text, sizeof g_error_text, used, " in section ");
                    write_u(g_error_text, sizeof g_error_text, used, (unsigned)(section_index + 1));
                    fail(g_error_text);
                }
                }
            /* I386 relocation types are applied on the 32-bit path. */
            } else {
                /* Select the I386 patch from the relocation type. */
                switch (relocation->Type) {
                /* Store a 32-bit virtual address. */
                case IMAGE_REL_I386_DIR32: {
                    uint64_t value;
                    require_patch(input, relocation, 4);
                    value = (uint64_t)read_u32(patch) + image_base(linker) +
                            target_rva;
                    /* Reject an address that does not fit in the 32-bit slot. */
                    crash_if(value > UINT32_MAX, "DIR32 relocation overflows");
                    write_u32(patch, (uint32_t)value);
                    break;
                }
                /* Store a 32-bit RVA, without adding the image base. */
                case IMAGE_REL_I386_DIR32NB: {
                    uint64_t value;
                    require_patch(input, relocation, 4);
                    value = (uint64_t)read_u32(patch) + target_rva;
                    /* Reject an RVA that does not fit in the 32-bit slot. */
                    crash_if(value > UINT32_MAX, "DIR32NB relocation overflows");
                    write_u32(patch, (uint32_t)value);
                    break;
                }
                /* Store a signed 32-bit displacement from the end of the field. */
                case IMAGE_REL_I386_REL32: {
                    int64_t displacement;
                    require_patch(input, relocation, 4);
                    displacement = (int64_t)(int32_t)read_u32(patch) +
                                   (int64_t)target_rva -
                                   (int64_t)place_rva - 4;
                    /* Reject a displacement that does not fit in 32 bits. */
                    crash_if(displacement < INT32_MIN ||
                        displacement > INT32_MAX, "REL32 relocation overflows");
                    write_u32(patch, (uint32_t)(int32_t)displacement);
                    break;
                }
                /* SECTION and SECREL are patched after this switch. Their type codes match AMD64. */
                case IMAGE_REL_I386_SECTION:
                case IMAGE_REL_I386_SECREL:
                    break;
                /* Any other I386 relocation type is not supported. */
                default: {
                    size_t used = 0;
                    used = write_text(g_error_text, sizeof g_error_text, used, "unsupported I386 relocation type 0x");
                    used = write_x(g_error_text, sizeof g_error_text, used, relocation->Type, 4);
                    used = write_text(g_error_text, sizeof g_error_text, used, " in section ");
                    write_u(g_error_text, sizeof g_error_text, used, (unsigned)(section_index + 1));
                    fail(g_error_text);
                }
                }
            }

            /* Add the emitted section index for a SECTION relocation on either machine. */
            if (relocation->Type == IMAGE_REL_AMD64_SECTION) {
                uint32_t value;
                require_patch(input, relocation, 2);
                /* Reject a target with no emitted section, including .rdata packed into the headers. */
                crash_if(target_output == NULL || target_output->in_headers, "SECTION relocation refers to a symbol without an output section");
                value = (uint32_t)read_u16(patch) + target_output->index;
                /* Reject a section index that does not fit in 16 bits. */
                crash_if(value > UINT16_MAX, "SECTION relocation overflows");
                write_u16(patch, (uint16_t)value);
            /* Add the offset within the target section for a SECREL relocation on either machine. */
            } else if (relocation->Type == IMAGE_REL_AMD64_SECREL) {
                uint64_t value;
                require_patch(input, relocation, 4);
                /* Reject a target with no emitted section, including .rdata packed into the headers. */
                crash_if(target_output == NULL || target_output->in_headers, "SECREL relocation refers to a symbol without an output section");
                value = (uint64_t)read_u32(patch) +
                        (target_rva - target_output->virtual_address);
                /* Reject a section offset that does not fit in 32 bits. */
                crash_if(value > UINT32_MAX, "SECREL relocation overflows");
                write_u32(patch, (uint32_t)value);
            }
        }
    }
}

/*
 * Choose the image entry point and store its name and RVA on the linker.
 * If requested is null, a /entry: name is taken from .drectve and used in
 * its place. An explicit name must already be a defined symbol. Its text is
 * kept in entry_storage when it came from the directive, and entry_rva is
 * that symbol's RVA. When no name was requested and no directive supplied
 * one, the first defined symbol among mainCRTStartup, WinMainCRTStartup,
 * wWinMainCRTStartup, _start, start, and main is used. fail() reports a
 * missing explicit symbol or the absence of every default. fail() prints the error and exits.
 */
static void select_entry(Linker *linker, const char *requested)
{
    static const char *defaults[] = {
        "mainCRTStartup", "WinMainCRTStartup", "wWinMainCRTStartup",
        "_start", "start", "main"
    };
    char *directive = NULL;
    uint32_t symbol_index;
    size_t index;

    /* An omitted request falls back to the /entry: name in .drectve, if any. */
    if (requested == NULL) {
        directive = directive_entry(linker);
        requested = directive;
    }
    /* An explicit name, from the caller or from .drectve, must be defined. */
    if (requested != NULL) {
        /* The requested entry symbol is not defined in this object. */
        if (find_defined_symbol(linker, requested, &symbol_index) == NULL) {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "entry symbol '");
            used = write_text(g_error_text, sizeof g_error_text, used, requested);
            write_text(g_error_text, sizeof g_error_text, used, "' is not defined");
            fail(g_error_text);
        }
        linker->entry_name = requested;
        linker->entry_storage = directive;
        linker->entry_rva = symbol_rva(linker, symbol_index, NULL);
        return;
    }
    /* Otherwise take the first defined startup symbol in the default order. */
    for (index = 0; index < ARRAY_COUNT(defaults); ++index) {
        /* This default is defined, so it becomes the entry point. */
        if (find_defined_symbol(linker, defaults[index], &symbol_index) != NULL) {
            linker->entry_name = defaults[index];
            linker->entry_rva = symbol_rva(linker, symbol_index, NULL);
            return;
        }
    }
    fail("no entry point found; use /entry:<symbol>");
}

/*
 * Serialize the laid-out image into a new PE file and return that buffer.
 * The file length is written through image_size. The buffer begins with a
 * DOS header whose e_lfanew is 64, then the PE signature, the COFF file
 * header, a PE32 or PE32+ optional header, and one section header for each
 * emitted section. Section bytes, including .rdata packed into the header
 * gap, are copied to their raw offsets. SizeOfImage is the highest
 * section-aligned RVA, and the file size is the highest raw end. Code,
 * initialized data, and uninitialized data totals are summed from section
 * characteristics, and the first code and non-code sections supply BaseOfCode
 * and BaseOfData. The optional header records the entry RVA, the image base,
 * both alignments, the subsystem, NX compatibility, terminal-server awareness,
 * and the default stack and heap sizes. When imports exist, the import and
 * IAT data directories point at .idata. A packed section is copied but does
 * not receive a section header. This function does not call fail() itself.
 * align_up_u32 and allocate report overflow and out-of-memory through fail(),
 * which prints the error and exits.
 */
static uint8_t *build_image(Linker *linker, size_t *image_size)
{
    uint32_t optional_size = optional_header_size(linker);
    uint32_t header_size;
    uint32_t file_size;
    uint32_t size_of_image = SECTION_ALIGNMENT;
    uint32_t size_of_code = 0;
    uint32_t size_of_initialized_data = 0;
    uint32_t size_of_uninitialized_data = 0;
    uint32_t base_of_code = 0;
    uint32_t base_of_data = 0;
    uint8_t *image;
    IMAGE_DOS_HEADER *dos;
    uint8_t *pe;
    IMAGE_FILE_HEADER *file_header;
    uint8_t *optional;
    IMAGE_SECTION_HEADER *section_headers;
    uint16_t index;

    header_size =
        align_up_u32(DOS_HEADER_SIZE + sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER) +
                         optional_size +
                         (uint32_t)linker->emitted_section_count *
                             sizeof(IMAGE_SECTION_HEADER),
                     FILE_ALIGNMENT);
    file_size = header_size;
    /* Grow SizeOfImage and the file size to cover every output section. */
    for (index = 0; index < linker->output_count; ++index) {
        OutputSection *output = &linker->outputs[index];
        uint32_t end_rva = align_up_u32(output->virtual_address +
                                           output->virtual_size,
                                       SECTION_ALIGNMENT);
        /* This section's aligned end is the new image high-water mark. */
        if (end_rva > size_of_image) {
            size_of_image = end_rva;
        }
        /* Raw bytes past the headers extend the file. */
        if (output->raw_size != 0 &&
            output->raw_offset + output->raw_size > file_size) {
            file_size = output->raw_offset + output->raw_size;
        }
    }
    image = allocate(file_size);
    dos = (IMAGE_DOS_HEADER *)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = DOS_HEADER_SIZE;

    pe = image + DOS_HEADER_SIZE;
    write_u32(pe, IMAGE_NT_SIGNATURE);
    file_header = (IMAGE_FILE_HEADER *)(pe + sizeof(uint32_t));
    file_header->Machine = linker->machine;
    file_header->NumberOfSections = linker->emitted_section_count;
    file_header->SizeOfOptionalHeader = (uint16_t)optional_size;
    file_header->Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
    /* AMD64 images are marked large-address aware. */
    if (linker_is_64_bit(linker)) {
        file_header->Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
    /* I386 images are marked as 32-bit machines. */
    } else {
        file_header->Characteristics |= IMAGE_FILE_32BIT_MACHINE;
    }

    optional = (uint8_t *)(file_header + 1);
    section_headers = (IMAGE_SECTION_HEADER *)(optional + optional_size);
    /* Copy section bytes and emit a header for every section that is not packed. */
    for (index = 0; index < linker->output_count; ++index) {
        OutputSection *output = &linker->outputs[index];
        IMAGE_SECTION_HEADER *header;

        /* Copy initialized bytes, including .rdata packed into the headers. */
        if (output->raw_size != 0) {
            memcpy(image + output->raw_offset, output->data,
                   output->virtual_size);
        }
        /* A packed section has no section header of its own. */
        if (output->in_headers) {
            continue;
        }
        header = &section_headers[output->index - 1];
        memcpy(header->Name, output->name, strlen(output->name));
        header->Misc.VirtualSize = output->virtual_size;
        header->VirtualAddress = output->virtual_address;
        header->SizeOfRawData = output->raw_size;
        header->PointerToRawData = output->raw_offset;
        header->Characteristics = output->characteristics;
        /* Code contributes its raw size to SizeOfCode and can supply BaseOfCode. */
        if ((output->characteristics & IMAGE_SCN_CNT_CODE) != 0) {
            size_of_code += output->raw_size;
            /* The first code section becomes BaseOfCode. */
            if (base_of_code == 0) {
                base_of_code = output->virtual_address;
            }
        }
        /* Initialized data contributes to SizeOfInitializedData and can supply BaseOfData. */
        if ((output->characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) != 0) {
            size_of_initialized_data += output->raw_size;
            /* The first non-code section among these becomes BaseOfData. */
            if (base_of_data == 0 &&
                (output->characteristics & IMAGE_SCN_CNT_CODE) == 0) {
                base_of_data = output->virtual_address;
            }
        }
        /* Uninitialized data contributes its virtual size and can also supply BaseOfData. */
        if ((output->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0) {
            size_of_uninitialized_data += output->virtual_size;
            /* A BSS section becomes BaseOfData when no earlier data section claimed it. */
            if (base_of_data == 0 &&
                (output->characteristics & IMAGE_SCN_CNT_CODE) == 0) {
                base_of_data = output->virtual_address;
            }
        }
    }

    /* Write the PE32+ optional header and its import directories. */
    if (linker_is_64_bit(linker)) {
        IMAGE_OPTIONAL_HEADER64 *header = (IMAGE_OPTIONAL_HEADER64 *)optional;

        header->Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        header->MajorLinkerVersion = 1;
        header->SizeOfCode = size_of_code;
        header->SizeOfInitializedData = size_of_initialized_data;
        header->SizeOfUninitializedData = size_of_uninitialized_data;
        header->AddressOfEntryPoint = linker->entry_rva;
        header->BaseOfCode = base_of_code;
        header->ImageBase = image_base(linker);
        header->SectionAlignment = SECTION_ALIGNMENT;
        header->FileAlignment = FILE_ALIGNMENT;
        header->MajorOperatingSystemVersion = 6;
        header->MajorSubsystemVersion = 6;
        header->SizeOfImage = size_of_image;
        header->SizeOfHeaders = header_size;
        header->Subsystem = linker->subsystem;
        header->DllCharacteristics =
            IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
            IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE;
        header->SizeOfStackReserve = UINT64_C(0x100000);
        header->SizeOfStackCommit = UINT64_C(0x1000);
        header->SizeOfHeapReserve = UINT64_C(0x100000);
        header->SizeOfHeapCommit = UINT64_C(0x1000);
        header->NumberOfRvaAndSizes = 16;
        /* Point the PE32+ import and IAT data directories at .idata. */
        if (linker->import_output != NULL) {
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress =
                linker->import_output->virtual_address;
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
                linker->import_descriptor_size;
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress =
                linker->import_output->virtual_address + linker->iat_start;
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size =
                linker->iat_size;
        }
    /* Write the PE32 optional header, including BaseOfData and import directories. */
    } else {
        IMAGE_OPTIONAL_HEADER32 *header = (IMAGE_OPTIONAL_HEADER32 *)optional;

        header->Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        header->MajorLinkerVersion = 1;
        header->SizeOfCode = size_of_code;
        header->SizeOfInitializedData = size_of_initialized_data;
        header->SizeOfUninitializedData = size_of_uninitialized_data;
        header->AddressOfEntryPoint = linker->entry_rva;
        header->BaseOfCode = base_of_code;
        header->BaseOfData = base_of_data;
        header->ImageBase = (uint32_t)image_base(linker);
        header->SectionAlignment = SECTION_ALIGNMENT;
        header->FileAlignment = FILE_ALIGNMENT;
        header->MajorOperatingSystemVersion = 6;
        header->MajorSubsystemVersion = 6;
        header->SizeOfImage = size_of_image;
        header->SizeOfHeaders = header_size;
        header->Subsystem = linker->subsystem;
        header->DllCharacteristics =
            IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
            IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE;
        header->SizeOfStackReserve = UINT32_C(0x100000);
        header->SizeOfStackCommit = UINT32_C(0x1000);
        header->SizeOfHeapReserve = UINT32_C(0x100000);
        header->SizeOfHeapCommit = UINT32_C(0x1000);
        header->NumberOfRvaAndSizes = 16;
        /* Point the PE32 import and IAT data directories at .idata. */
        if (linker->import_output != NULL) {
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress =
                linker->import_output->virtual_address;
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
                linker->import_descriptor_size;
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress =
                linker->import_output->virtual_address + linker->iat_start;
            header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size =
                linker->iat_size;
        }
    }
    *image_size = file_size;
    return image;
}
/* Command line, file I/O, and the in-process fuzzer. The link engine above is unchanged. */

/*
 * Declare the kernel32 conversion from UTF-16 to a multi-byte string. The
 * fuzzer calls it with code page 0 to measure a narrow copy of wide text and
 * then to fill that copy. The caller treats a non-positive length or a short
 * write as failure. This is only a declaration; the function itself lives in
 * kernel32 and is not defined in this file.
 */
extern int WideCharToMultiByte(
    unsigned int code_page, DWORD flags, const wchar_t *source, int source_length,
    char *destination, int destination_bytes, const char *default_char, int *used_default);

/* Characteristics the fuzzer puts on synthetic code: code, execute, and read. */
#define TEXT_CHARS (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)
/* Characteristics the fuzzer puts on synthetic read-only initialized data. */
#define RDATA_CHARS (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ)
/* Characteristics the fuzzer puts on synthetic writable uninitialized data. */
#define BSS_CHARS (IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE)
/* Maximum number of sections a synthetic COFF object spec can hold. */
#define SPEC_SECTIONS 6
/* Maximum number of symbols a synthetic COFF object spec can hold. */
#define SPEC_SYMBOLS 8
/* Maximum number of relocations stored on one synthetic section. */
#define SPEC_RELOCS 8
/* Capacity of a synthetic section or symbol name, including the terminator. */
#define SPEC_NAME 40
/* Capacity of the raw-byte buffer stored on one synthetic section. */
#define SPEC_BYTES 512
/* How many distinct well-formed object shapes the valid fuzzer builds. */
#define VALID_KINDS 24
/* How many times each well-formed shape is rebuilt with varying details. */
#define VALID_ITERS 10
/* How many distinct rejected inputs the invalid fuzzer accounts for. */
#define INVALID_KINDS 21
/* How many times each rejected shape is rebuilt with varying details. */
#define INVALID_ITERS 8

/*
 * Ways the synthetic COFF writer damages an object so the linker must reject
 * it. emit_coff reads this value from the spec after the normal bytes have
 * been written. CORRUPT_NONE leaves those bytes alone. The other values
 * change the machine, section count, optional-header size, symbol table,
 * or string-table size, or they shorten the file.
 */
enum {
    /* No damage. Zero so a cleared spec is emitted exactly as the writer built it. */
    CORRUPT_NONE = 0,
    /* Cut the emitted file down to truncate_to bytes so later headers and tables are missing. */
    CORRUPT_TRUNCATE,
    /* Force NumberOfSections to zero after a real section was otherwise written. */
    CORRUPT_ZERO_SECTIONS,
    /* Overwrite the COFF machine with force_machine, or with 0xAAAA when that field is zero. */
    CORRUPT_MACHINE,
    /* Set SizeOfOptionalHeader to 32 so the file no longer looks like a plain COFF object. */
    CORRUPT_OPTIONAL,
    /* Set the string-table size to 0x0fffffff, which does not fit in the emitted file. */
    CORRUPT_STRINGS,
    /* Clear PointerToSymbolTable and NumberOfSymbols so the object has no symbol table. */
    CORRUPT_SYMBOLS
};

/*
 * Coverage ids for the in-process self-test. Each enumerator below COV_COUNT
 * names one situation the tests must reach. cov_set records that id in
 * g_cov, and cov_require fails the run if any of those ids was never set.
 * The values are dense from zero so they can index that array directly.
 */
enum {
    /* A one-byte AMD64 object containing only a return was linked, and that code byte survived. */
    COV_MIN_AMD64 = 0,
    /* The same one-byte return object was linked for I386 and kept its code byte. */
    COV_MIN_I386,
    /* An image kept a real .rdata section header because header packing was off. */
    COV_UNPACKED,
    /* Header packing placed read-only .rdata in the headers and omitted its section header. */
    COV_PACK,
    /* The packed image was exactly one file-alignment unit smaller than the unpacked image. */
    COV_PACK_DIFF,
    /* An uninitialized .bss section added a section header without growing the file. */
    COV_BSS,
    /* AMD64 REL32, REL32_1, ADDR64, ADDR32NB, SECTION, and SECREL patches matched the oracle. */
    COV_RELOC_AMD64,
    /* I386 REL32, DIR32, DIR32NB, SECTION, and SECREL patches matched the oracle. */
    COV_RELOC_I386,
    /* A REL32 displacement that does not fit in 32 bits was rejected. */
    COV_REL32_OVF,
    /* An ADDR32 virtual address above 4 GiB was rejected. */
    COV_ADDR32,
    /* An ABSOLUTE relocation left the original section bytes unchanged. */
    COV_ABSOLUTE,
    /* The contributions .text$a and .text$b were concatenated into one .text section in order. */
    COV_MERGE,
    /* A .drectve /ENTRY:start directive selected the entry while discardable sections were dropped. */
    COV_DIRECTIVE,
    /* With no explicit entry, mainCRTStartup was chosen and its RVA was not the section start. */
    COV_ENTRY_ORDER,
    /* An I386 symbol spelled _start satisfied an entry request for the undecorated name start. */
    COV_X86_DECOR,
    /* An AMD64 reference to ExitProcess was bound to the jump thunk rather than the IAT slot. */
    COV_IMP_DIR64,
    /* An AMD64 __imp_ExitProcess reference was bound to the IAT slot. */
    COV_IMP_IAT64,
    /* An I386 decorated ExitProcess reference was bound to the jump thunk. */
    COV_IMP_DIR32,
    /* An I386 __imp_ decorated ExitProcess reference was bound to the IAT slot. */
    COV_IMP_IAT32,
    /* Header packing was rejected because the object had no read-only .rdata section. */
    COV_PACK_NONE,
    /* Header packing was rejected because .rdata did not fit in the header padding. */
    COV_PACK_HUGE,
    /* A SECTION relocation targeting a symbol in packed .rdata was rejected. */
    COV_PACK_SECTION,
    /* A SECREL relocation targeting a symbol in packed .rdata was rejected. */
    COV_PACK_SECREL,
    /* A COFF machine other than I386 or AMD64 was rejected. */
    COV_BAD_MACHINE,
    /* An object cut down to a few bytes was rejected as too small to be COFF. */
    COV_TRUNC,
    /* An object whose section count was forced to zero was rejected. */
    COV_ZERO,
    /* An object with a non-zero optional-header size was rejected as not a plain COFF object. */
    COV_OPTIONAL,
    /* An object whose COFF string-table size does not fit the file was rejected. */
    COV_STRINGS,
    /* An object whose symbol-table pointer and count were cleared was rejected. */
    COV_SYMBOLS,
    /* A relocation to an undefined symbol that is not an import was rejected. */
    COV_UNRESOLVED,
    /* A defined symbol whose value lies past its section was rejected. */
    COV_PAST,
    /* A section that carries COFF line-number records was rejected. */
    COV_LINES,
    /* An object whose only symbol is not a known entry name was rejected. */
    COV_NO_ENTRY,
    /* The option spellings --help, /?, and -? were accepted as help requests. */
    COV_HELP,
    /* A command line with an option but no input file was reported as usage. */
    COV_USAGE,
    /* A command line that names two input files was rejected. */
    COV_ONE_INPUT,
    /* Separate /entry and /subsystem arguments were parsed, and a bare /entry was rejected. */
    COV_TWO_TOKEN,
    /* The -out: form with packing and a subsystem was parsed, and an empty /out: was rejected. */
    COV_OUT_PREFIX,
    /* An unrecognized command-line option was rejected. */
    COV_UNKNOWN,
    /* Two imports of one DLL that differ only by case shared a single DLL record. */
    COV_CASEFOLD,
    /* Importing the same symbol from two different DLLs was rejected. */
    COV_DUP_IMPORT,
    /* An import specification that has no comma between the DLL and the symbol was rejected. */
    COV_IMPORT_FORM,
    /* An empty symbol name in an import list was rejected. */
    COV_EMPTY_IMPORT,
    /* Every well-formed synthetic object kind linked successfully across its iterations. */
    COV_VALID_FUZZ,
    /* Every invalid-object and invalid-option kind was exercised and rejected. */
    COV_INVALID_FUZZ,
    /* Well-formed import strings with one symbol and with two symbols were accepted. */
    COV_IMPORT_OK,
    /* The malformed import shapes, including duplicates and empty names, were rejected. */
    COV_IMPORT_BAD,
    /* Random combinations of the known linker options parsed successfully. */
    COV_OPTION_OK,
    /* Random unknown options were rejected as unrecognized. */
    COV_OPTION_BAD,
    /* One past the last coverage id. This is the length of g_cov, not a situation the tests mark. */
    COV_COUNT
};

/* One COFF section the self-test wants emit_coff to write.
   The fuzzer fills this, then the emitter turns it into a 40-byte section header plus raw bytes and relocation records. */
typedef struct {
    /* NUL-terminated section name, at most SPEC_NAME minus one byte.
       Names longer than eight bytes are emitted as a slash and a string-table offset.
       Tests use .text, .rdata, .bss, .text$a, .drectve, .debug$S, and .llvm. */
    char name[SPEC_NAME];
    /* IMAGE_SCN bitfield copied into the section header.
       TEXT_CHARS, RDATA_CHARS, and BSS_CHARS are the usual combinations.
       Pack-headers accepts only a read-only initialized .rdata, so this word is what makes a packed section legal or not. */
    uint32_t characteristics;
    /* Payload length in bytes.
       Initialized sections copy this many bytes from the bytes array.
       Uninitialized sections keep this as the virtual size and emit no raw data.
       The builder rejects a size above SPEC_BYTES. */
    uint32_t size;
    /* Initialized raw contents, capacity SPEC_BYTES.
       Ignored when uninit is set.
       The emitter copies size bytes into the COFF raw-data region.
       Oracles plant 0xC3, 0xE8, and fill patterns here and then look for them in the PE. */
    uint8_t bytes[SPEC_BYTES];
    /* Nonzero when the section is BSS-like and must not carry raw bytes.
       spec_add_section stores 1 or 0.
       The emitter then writes a zero file pointer while still recording size. */
    uint8_t uninit;
    /* COFF line-number count written into the section header.
       A nonzero count is rejected by the linker.
       The invalid oracle sets this to 1 and expects the line-number diagnostic. */
    uint16_t line_numbers;
    /* How many relocations are stored in the three parallel arrays.
       The emitter writes that many 10-byte COFF relocation records.
       SPEC_RELOCS is the capacity, and spec_add_reloc aborts the test past it. */
    uint16_t reloc_count;
    /* Byte offset of each relocation within the section, not an image RVA.
       It is written into the COFF relocation VirtualAddress field.
       spec_add_reloc appends the caller's offset here. */
    uint32_t reloc_va[SPEC_RELOCS];
    /* Zero-based index into Spec.symbols for each relocation.
       The emitter stores it as the COFF SymbolTableIndex.
       Tests use 0 for the section symbol and 1 for a second symbol such as an import. */
    uint16_t reloc_sym[SPEC_RELOCS];
    /* COFF relocation type, for example IMAGE_REL_AMD64_REL32 or IMAGE_REL_I386_DIR32.
       The linker applies this type when it patches the image.
       An unknown type is an invalid-object case and must fail the link. */
    uint16_t reloc_type[SPEC_RELOCS];
} SpecSection;

/* One COFF symbol the self-test emits.
   Symbols name the entry point, relocation targets, and imports.
   The emitter writes an 18-byte external symbol for each live entry. */
typedef struct {
    /* NUL-terminated symbol name, at most SPEC_NAME minus one byte.
       Names longer than eight bytes are placed in the COFF string table.
       Entry names, decorated stdcall imports, and the unresolved symbol missing live here. */
    char name[SPEC_NAME];
    /* Symbol value in bytes from the start of its section, or zero for an undefined external.
       The linker rejects a value that falls past the section.
       The past-section oracle stores 2 or 4 here on a one-byte .text. */
    uint32_t value;
    /* COFF section number: a 1-based index for a definition, or 0 for an undefined external.
       Import thunks and unresolved externals use 0.
       The emitter writes this integer into the symbol record. */
    int16_t section_number;
} SpecSymbol;

/* Whole synthetic COFF object the in-process fuzzer links.
   spec_reset clears it, the add helpers fill sections and symbols, and emit_coff serializes it.
   corrupt and truncate_to then deliberately damage an otherwise valid object. */
typedef struct {
    /* COFF machine stored in the file header, IMAGE_FILE_MACHINE_AMD64 or IMAGE_FILE_MACHINE_I386.
       emit_coff writes it unless CORRUPT_MACHINE overwrites the field.
       The fuzzer alternates machines with the iteration bit. */
    uint16_t machine;
    /* Number of live entries in sections.
       spec_add_section increments it and returns the previous index.
       emit_coff requires at least one section, and CORRUPT_ZERO_SECTIONS clears the on-disk count after the headers are built. */
    int section_count;
    /* Number of live entries in symbols.
       spec_add_symbol increments it.
       emit_coff requires at least one symbol, and CORRUPT_SYMBOLS zeros the on-disk symbol pointer and count. */
    int symbol_count;
    /* Input sections in file order, capacity SPEC_SECTIONS.
       The emitter walks this array for headers, raw data, and relocations.
       Merge tests place .text$a and .text$b in successive slots. */
    SpecSection sections[SPEC_SECTIONS];
    /* Symbol table in file order, capacity SPEC_SYMBOLS.
       Index 0 is usually the entry symbol.
       Relocations refer to these entries by the index stored in reloc_sym. */
    SpecSymbol symbols[SPEC_SYMBOLS];
    /* Which deliberate header damage emit_coff applies after a well-formed image is built.
       CORRUPT_NONE, the zero value left by spec_reset, leaves the object intact.
       The other CORRUPT constants select a machine, section count, optional header, symbol table, string table, or short file. */
    int corrupt;
    /* Byte length reported for the object when corrupt is CORRUPT_TRUNCATE.
       The allocation is still the full image; only Emitted.size shrinks.
       The invalid fuzzer uses a length from 1 through 12 so the parser sees a short file. */
    uint32_t truncate_to;
    /* Machine word written when corrupt is CORRUPT_MACHINE.
       Zero tells the emitter to substitute 0xAAAA instead.
       The fuzzer stores 0x1000 plus the iteration so each rejected machine differs. */
    uint16_t force_machine;
} Spec;

/* PE image produced by one self-test link, plus the linker fields the oracles compare.
   try_link_bytes fills it only when the link finishes.
   A failed link leaves the struct zeroed and returns failure to the caller. */
typedef struct {
    /* Arena pointer to the PE bytes from build_image, or NULL when no image was produced.
       Checkers read the DOS stub, NT headers, and section bytes through it.
       The next link_spec call resets the arena, so the pointer dies with the next case. */
    uint8_t *image;
    /* Length of image in bytes.
       expect_ok compares it with the expected file size, usually 1024 or 1536.
       It stays zero when the link fails before build_image. */
    size_t image_size;
    /* Section headers the linker actually wrote.
       Packing .rdata into header padding omits that section, so the count drops by one.
       check_pe requires this to match the PE NumberOfSections field. */
    uint16_t emitted_section_count;
    /* COFF machine copied from the linker after a successful link.
       check_pe compares it with the machine the spec requested.
       It is not read when try_link_bytes reports failure. */
    uint16_t machine;
    /* Subsystem stored on the linker, Windows GUI or console.
       check_pe compares it with the optional-header subsystem and with the value the case expected.
       The CLI default is the Windows GUI subsystem. */
    uint16_t subsystem;
    /* Entry symbol name the linker selected, such as start or mainCRTStartup.
       The string is owned by the linker, not by this struct.
       check_pe requires it to be non-empty, and the entry-order cases strcmp it. */
    const char *entry_name;
} LinkInfo;

/* Command-line result shared by the CLI and the option fuzzer.
   parse_wide fills the paths, the entry override, and the linker flags.
   parse_ascii wraps it so narrow test argv can exercise the same parser. */
typedef struct {
    /* Wide path of the single input COFF object.
       NULL until a non-option argument is seen.
       parse_wide returns 1 when it stays NULL, and the CLI then prints usage and exits with status 1.
       The pointer aliases an argv token; it is not a copy. */
    const wchar_t *input_path;
    /* Wide path of the PE file to write.
       parse_wide defaults it to the literal a.exe.
       A /out: or -out: argument replaces it with the suffix text.
       The CLI passes this pointer to write_file_w. */
    const wchar_t *output_path;
    /* Narrow entry-symbol override from /entry, or NULL so the linker picks the entry.
       The text is allocated by wide_to_ascii from the arena.
       The option tests require the text start, and a missing value makes parse_wide call fail. */
    char *entry;
    /* Linker object that receives subsystem, pack_headers, and imports while options are parsed.
       The CLI and parse_ascii both point this at their stack Linker before parse_wide runs.
       The object file itself is not loaded until after parsing succeeds. */
    Linker *linker;
} ParsedArgs;

/* COFF object buffer returned by emit_coff.
   try_link_bytes reads data for size bytes.
   string_off records where the string table was placed and is not consumed by the fuzzer. */
typedef struct {
    /* Arena pointer to the serialized COFF object, including headers, raw data, relocations, symbols, and the string table.
       allocate supplies the bytes and zero-fills them.
       Truncation does not shrink this allocation; only the reported size changes. */
    uint8_t *data;
    /* Number of bytes the linker should treat as the file.
       Normally that is the full layout cursor.
       CORRUPT_TRUNCATE replaces it with truncate_to so parse_object sees a short input. */
    size_t size;
    /* Byte offset of the COFF string table inside data.
       The first dword at that offset is the table size, including the size field itself.
       The fuzzer does not read this; emit_coff keeps it so the corrupt-string path can poke the size. */
    size_t string_off;
} Emitted;

/* Self-test globals.
   g_spec is the object under construction.
   g_sym_str and g_sec_str hold string-table offsets filled by string_table_size, or zero when the name is stored inline.
   g_testLog is the self_test.txt handle, or INVALID_HANDLE_VALUE when the log is closed.
   g_case and g_case_buf name the active oracle for test_fail_at_line.
   g_cov records which COV_ ids have been hit, and cov_require demands every slot.
   g_valid_mask and g_invalid_mask accumulate one bit per fuzzer kind.
   g_imp_store, g_imp_ptr, and g_imp_count are the import strings attached to the next link_spec call.
   g_rngState and g_rngInc are the PCG generator used to vary valid and invalid objects. */
static Spec g_spec;
static uint32_t g_sym_str[SPEC_SYMBOLS];
static uint32_t g_sec_str[SPEC_SECTIONS];
static HANDLE g_testLog = INVALID_HANDLE_VALUE;
static const char *g_case = "";
static char g_case_buf[80];
static uint8_t g_cov[COV_COUNT];
static uint32_t g_valid_mask = 0;
static uint32_t g_invalid_mask = 0;
static char g_imp_store[4][96];
static const char *g_imp_ptr[4];
static int g_imp_count = 0;
static uint64_t g_rngState = 0x853c49e6748fea9bULL;
static uint64_t g_rngInc = 0xda3e39cb94b95bdbULL;

/* console_text writes a narrow C string to a standard-handle id.
   It forwards to console_write, which resolves GetStdHandle and ignores a missing handle or a short write.
   Usage text, the CLI status line, and self-test log echoes all go through this wrapper.
   There is no failure return; a dropped console write does not stop the process. */
static void console_text(DWORD handle, const char *text) {
    console_write(handle, text);
}

/* test_log echoes a diagnostic to the console and appends the same bytes to the self-test log.
   A NULL pointer is ignored.
   A failed or short WriteFile closes the log and crash_if prints a fixed error, then exits.
   That exit is not test_fail_at_line; the source line is not the process code. */
static void test_log(const char *text) {
    DWORD length = 0;
    DWORD written = 0;
    /* A missing message is not a test failure; skip the write. */
    if (text == NULL) return;
    /* Count bytes up to the terminating NUL. The length excludes that NUL. */
    while (text[length] != 0) length++;
    console_text(STD_OUTPUT_HANDLE, text);
    /* Echo already happened. Skip the file when the log is closed or the text is empty. */
    if (g_testLog == INVALID_HANDLE_VALUE || length == 0) return;
    /* A short or failed log write is fatal to the self-test process. */
    if (!WriteFile(g_testLog, text, length, &written, NULL) || written != length) {
        /* Close the log handle before exiting, when it is still open. */
        if (g_testLog != INVALID_HANDLE_VALUE) CloseHandle(g_testLog);
        crash_if(1, "self-test log write failed");
    }
}

/* test_log_uint prints an unsigned value in decimal with no leading zeros except for zero itself.
   It builds digits least-significant first, reverses them into a local buffer, and passes that to test_log.
   The buffer holds a 64-bit value.
   Failure is whatever test_log does; this helper itself does not exit. */
static void test_log_uint(uint64_t value) {
    char digits[24];
    char out[25];
    int count = 0;
    int index;
    /* Zero has no remainders, so store a single digit character 0. */
    if (value == 0) digits[count++] = '0';
    /* Peel off decimal digits from least significant to most significant. */
    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    }
    /* Reverse the digit stack into the NUL-terminated string test_log prints. */
    for (index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
    test_log(out);
}

/* test_fail_at_line reports a self-test oracle miss and exits the process.
   It logs the source line, the active case name when one is set, and g_error_text when the linker stored a diagnostic.
   The log handle is closed when it is open.
   ExitProcess uses the source line as the process code, so the caller does not continue. */
static void test_fail_at_line(int line) {
    test_log("\r\n*** FAILED at source line ");
    test_log_uint((uint64_t)line);
    test_log(" ***\r\n");
    /* Include the case name when the fuzzer or an oracle stored one. */
    if (g_case != NULL && g_case[0] != 0) {
        test_log("case: ");
        test_log(g_case);
        test_log("\r\n");
    }
    /* Include the linker diagnostic when fail() stored one. */
    if (g_error_text[0] != 0) {
        test_log("linker: ");
        test_log(g_error_text);
        test_log("\r\n");
    }
    /* Close the log so the failure text is flushed before the process ends. */
    if (g_testLog != INVALID_HANDLE_VALUE) CloseHandle(g_testLog);
    ExitProcess((DWORD)line);
}

/* The one-argument crash_if aborts the self-test when its condition is true.
   A comma inside parentheses is still one argument, so an expression such as contains(text, snippet) stays intact.
   Two arguments call the crash_if function: the condition, then the message fail() prints.
   crash_at_line uses this source line as the process code. test_fail_at_line does not return. */
#define CRASH_DISPATCH(_1, _2, NAME, ...) NAME
#define crash_at_line(condition) do { /* End the self-test at this source line when the check fails. */ if (condition) test_fail_at_line(__LINE__); } while (0)
#define crash_if(...) CRASH_DISPATCH(__VA_ARGS__, (crash_if), crash_at_line)(__VA_ARGS__)

/* cov_set marks one coverage id as hit in g_cov.
   The id must lie in the COV_COUNT range or crash_if ends the process with the source line.
   cov_require later demands that every id was set.
   A repeated set of the same id stays 1. */
static void cov_set(int id) {
    /* An id outside the coverage table is a broken test, not a linker failure. */
    crash_if(id < 0 || id >= COV_COUNT);
    g_cov[id] = 1;
}

/* rng_reset restores the PCG state and increment to the fixed self-test seed.
   Valid and invalid fuzz loops call this so the same binary repeats the same objects.
   It has no failure path.
   The constants match the initializers on g_rngState and g_rngInc. */
static void rng_reset(void) {
    g_rngState = 0x853c49e6748fea9bULL;
    g_rngInc = 0xda3e39cb94b95bdbULL;
}

/* rng advances the PCG generator and returns the next 32-bit value.
   It reads g_rngState, stores the new state, and applies the xorshift-and-rotate output function.
   The increment g_rngInc stays constant between resets.
   There is no failure path; every call yields a defined word. */
static uint32_t rng(void) {
    uint64_t oldstate = g_rngState;
    uint32_t xorshifted;
    uint32_t rot;
    g_rngState = oldstate * 6364136223846793005ULL + g_rngInc;
    xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

/* rand_range returns an integer from first through last, inclusive.
   crash_if ends the test when the range is backwards.
   A one-value span returns that value without drawing, otherwise the result is first plus rng modulo the span.
   The helper is part of the fuzzer support even when a given build does not call it. */
static int rand_range(int first, int last) {
    /* A reversed span is a test bug; exit with this source line. */
    crash_if(first > last);
    /* No roll is needed when the span is a single integer. */
    if (first == last) return first;
    return (int)(rng() % (uint32_t)(last - first + 1)) + first;
}

/* contains reports whether needle occurs as a contiguous substring of haystack.
   An empty needle matches.
   A needle longer than the haystack does not.
   It returns 1 on a match and 0 otherwise, and it does not call fail.
   expect_fail uses it to check that g_error_text includes a distinctive snippet. */
static int contains(const char *haystack, const char *needle) {
    size_t needle_length = strlen(needle);
    size_t hay_length = strlen(haystack);
    size_t index;
    /* The empty string is a substring of every haystack. */
    if (needle_length == 0) return 1;
    /* A longer needle cannot fit, so this is not a match. */
    if (hay_length < needle_length) return 0;
    /* Slide the needle across every aligned start that still fits. */
    for (index = 0; index + needle_length <= hay_length; index++) {
        /* This alignment matches, so the search succeeds. */
        if (strncmp(haystack + index, needle, needle_length) == 0) return 1;
    }
    return 0;
}

/* image_has searches a PE image for an exact byte string, usually a DLL or import name.
   An empty needle or a needle longer than the image returns 0.
   A match returns 1.
   It does not call fail; callers pass the result to crash_if when the name must be present. */
static int image_has(const uint8_t *image, size_t size, const char *text) {
    size_t needle = strlen(text);
    size_t index;
    /* Nothing to find, or not enough bytes, so report absence. */
    if (needle == 0 || size < needle) return 0;
    /* Try every file offset where the whole needle still fits. */
    for (index = 0; index + needle <= size; index++) {
        /* The image bytes at this offset equal the text. */
        if (strncmp((const char *)image + index, text, needle) == 0) return 1;
    }
    return 0;
}

/* wide_to_ascii converts a NUL-terminated wide string into an arena-allocated narrow string.
   A NULL input calls fail with missing text and does not return.
   WideCharToMultiByte is asked for the length first; a non-positive length or a short conversion calls fail and does not return.
   The caller receives the buffer, including its terminating NUL, and the arena frees it on the next reset. */
static char *wide_to_ascii(const wchar_t *text) {
    int length;
    char *copy;
    /* There is no wide text to convert. fail() prints the error and exits. */
    crash_if(text == NULL, "missing text");
    length = WideCharToMultiByte(0, 0, text, -1, NULL, 0, NULL, NULL);
    /* The conversion length is unusable. fail() prints the error and exits. */
    crash_if(length <= 0, "cannot convert text");
    copy = (char *)allocate((size_t)length);
    /* The second conversion did not write the predicted size. fail() prints the error and exits. */
    crash_if(WideCharToMultiByte(0, 0, text, -1, copy, length, NULL, NULL) != length, "cannot convert text");
    return copy;
}

/* ascii_to_wide copies a narrow NUL-terminated string into an arena-allocated wide string.
   Each source byte becomes one wchar_t, so the result is not a general UTF-16 conversion.
   The copy is NUL-terminated.
   Allocation failure goes through allocate, which calls fail and does not return; this helper itself returns the pointer. */
static wchar_t *ascii_to_wide(const char *text) {
    size_t length = strlen(text);
    size_t index;
    wchar_t *copy = (wchar_t *)allocate((length + 1u) * sizeof(wchar_t));
    /* Copy each source byte into the next wide character. */
    for (index = 0; index < length; index++) copy[index] = (unsigned char)text[index];
    copy[length] = 0;
    return copy;
}

/* wide_eq_ascii compares a wide string with a narrow literal, case-sensitively.
   Each wide character must equal the unsigned byte of the narrow character.
   It returns 1 only when both sides end together.
   A mismatch returns 0. It does not call fail. */
static int wide_eq_ascii(const wchar_t *wide, const char *ascii) {
    /* Walk the narrow literal. The wide side must match every byte. */
    while (*ascii != 0) {
        /* This character differs, so the strings are not equal. */
        if (*wide != (unsigned char)*ascii) return 0;
        wide++;
        ascii++;
    }
    return *wide == 0;
}

/* wide_eq_ascii_i compares a wide argument with a narrow option, ignoring ASCII case.
   A-Z on either side is folded to a-z before the compare.
   It returns 1 when both strings end together and 0 on the first mismatch.
   The option parser uses it for switches such as /entry and --help. It does not call fail. */
static int wide_eq_ascii_i(const wchar_t *wide, const char *ascii) {
    /* Compare each narrow option character with the next wide character. */
    while (*ascii != 0) {
        wchar_t left = *wide;
        char right = *ascii;
        /* Fold a wide uppercase letter so the compare is case-insensitive. */
        if (left >= L'A' && left <= L'Z') left = (wchar_t)(left - L'A' + L'a');
        /* Fold a narrow uppercase letter the same way. */
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        /* After folding, this character does not match. */
        if (left != (wchar_t)(unsigned char)right) return 0;
        wide++;
        ascii++;
    }
    return *wide == 0;
}

/* wide_prefix tests whether text begins with an ASCII option such as /out: and returns the suffix.
   The compare folds ASCII case on both sides.
   On a match it stores the remaining wide characters in value and returns 1.
   A mismatch returns 0 and leaves value unchanged. It does not call fail. */
static int wide_prefix(const wchar_t *text, const char *option, const wchar_t **value) {
    /* Consume the option spelling. A leftover suffix is the value. */
    while (*option != 0) {
        wchar_t left = *text;
        char right = *option;
        /* Fold a wide uppercase letter before comparing this option character. */
        if (left >= L'A' && left <= L'Z') left = (wchar_t)(left - L'A' + L'a');
        /* Fold a narrow uppercase letter the same way. */
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        /* This option character does not match, so text is a different argument. */
        if (left != (wchar_t)(unsigned char)right) return 0;
        text++;
        option++;
    }
    *value = text;
    return 1;
}

/* align4 rounds a file offset up to the next multiple of four, leaving exact multiples unchanged.
   emit_coff uses it so raw data, relocation blocks, and the symbol table start on a COFF alignment.
   The operation is a pure bitwise round-up.
   There is no failure path and no overflow check; callers pass layout cursors that fit in 32 bits. */
static uint32_t align4(uint32_t value) {
    return (value + 3u) & ~3u;
}

/* spec_reset clears g_spec and stores the COFF machine the next object will claim.
   Every section, symbol, relocation, and corrupt flag goes back to zero.
   Later spec_add calls fill the same global.
   It does not allocate and cannot fail. */
static void spec_reset(uint16_t machine) {
    memset(&g_spec, 0, sizeof g_spec);
    g_spec.machine = machine;
}

/* spec_add_section appends one section to g_spec and returns its zero-based index.
   The name, characteristics, size, and uninit flag are stored.
   Initialized bytes are copied when uninit is clear, size is nonzero, and bytes is not NULL.
   crash_if ends the test if the section table, the byte buffer, or the name capacity would overflow.
   The returned index is the value before section_count is incremented, which is the COFF section number minus one. */
static int spec_add_section(const char *name, uint32_t characteristics, const void *bytes, uint32_t size, int uninit) {
    SpecSection *section;
    /* The synthetic object has no room for another section. */
    crash_if(g_spec.section_count >= SPEC_SECTIONS);
    /* The payload does not fit in the section's byte buffer. */
    crash_if(size > SPEC_BYTES);
    /* The section name does not fit in the fixed name field. */
    crash_if(strlen(name) >= SPEC_NAME);
    section = &g_spec.sections[g_spec.section_count];
    strcpy(section->name, name);
    section->characteristics = characteristics;
    section->size = size;
    section->uninit = (uint8_t)(uninit ? 1 : 0);
    /* Initialized data may need a payload copy. BSS skips this. */
    if (!uninit && size != 0) {
        /* Copy the caller's bytes. A NULL pointer leaves the zero-filled buffer. */
        if (bytes != NULL) memcpy(section->bytes, bytes, size);
    }
    return g_spec.section_count++;
}

/* spec_add_symbol appends one symbol and returns its zero-based index.
   The name, value, and section number are stored.
   Section number 0 means an undefined external; a positive number is a 1-based COFF section.
   crash_if ends the test when the symbol table or the name buffer would overflow.
   Relocations created later refer to this returned index. */
static int spec_add_symbol(const char *name, uint32_t value, int section_number) {
    SpecSymbol *symbol;
    /* The synthetic object has no room for another symbol. */
    crash_if(g_spec.symbol_count >= SPEC_SYMBOLS);
    /* The symbol name does not fit in the fixed name field. */
    crash_if(strlen(name) >= SPEC_NAME);
    symbol = &g_spec.symbols[g_spec.symbol_count];
    strcpy(symbol->name, name);
    symbol->value = value;
    symbol->section_number = (int16_t)section_number;
    return g_spec.symbol_count++;
}

/* spec_add_reloc appends one relocation to a section already added to g_spec.
   offset is the byte displacement stored as the relocation virtual address.
   symbol is the index from spec_add_symbol, and type is the COFF relocation type.
   crash_if ends the test when the section index is out of range or that section's relocation list is full.
   Nothing is written to disk here; emit_coff serializes the arrays later. */
static void spec_add_reloc(int section, uint32_t offset, int symbol, uint16_t type) {
    SpecSection *target;
    /* The relocation names a section the spec does not have. */
    crash_if(section < 0 || section >= g_spec.section_count);
    target = &g_spec.sections[section];
    /* That section has no room for another relocation. */
    crash_if(target->reloc_count >= SPEC_RELOCS);
    target->reloc_va[target->reloc_count] = offset;
    target->reloc_sym[target->reloc_count] = (uint16_t)symbol;
    target->reloc_type[target->reloc_count] = type;
    target->reloc_count++;
}

/* write_coff_symbol fills one 18-byte COFF symbol record inside the object being emitted.
   A nonzero string_offset selects the long-name form: the first dword is zero and the second is the string-table offset.
   Otherwise the name is copied into the eight-byte short-name field, truncated if it is longer.
   Value and section number follow, the type field stays zero, and storage class is set to 2, external.
   The record buffer is cleared first. This helper does not call fail; callers guarantee the name and offset. */
static void write_coff_symbol(uint8_t *record, const char *name, uint32_t string_offset, uint32_t value, int16_t section) {
    size_t length;
    memset(record, 0, 18);
    /* A long name lives in the string table. Point the record at that offset. */
    if (string_offset != 0) {
        write_u32(record, 0);
        write_u32(record + 4, string_offset);
    /* A short name is stored inline in the eight-byte field. */
    } else {
        length = strlen(name);
        /* Never copy more than the eight-byte COFF short-name field. */
        if (length > 8) length = 8;
        memcpy(record, name, length);
    }
    write_u32(record + 8, value);
    write_u16(record + 12, (uint16_t)section);
    record[16] = 2;
}

/* string_table_size assigns string-table offsets for symbol and section names that do not fit in eight bytes.
   Offsets land in g_sym_str and g_sec_str; a short name stores zero so the emitter uses the inline form.
   The running offset starts at 4 because the table begins with its own size dword.
   Each long name consumes its bytes plus a NUL.
   The returned size is that final offset and becomes the table's size field.
   No allocation and no failure path; the names already fit in the spec buffers. */
static uint32_t string_table_size(void) {
    uint32_t at = 4;
    int index;
    /* Give every long symbol name a string-table offset. */
    for (index = 0; index < g_spec.symbol_count; index++) {
        size_t length = strlen(g_spec.symbols[index].name);
        /* This symbol name does not fit inline, so reserve bytes after the current offset. */
        if (length > 8) {
            g_sym_str[index] = at;
            at += (uint32_t)length + 1u;
        /* A short symbol name stays inline, so its string offset is zero. */
        } else {
            g_sym_str[index] = 0;
        }
    }
    /* Give every long section name a string-table offset too. */
    for (index = 0; index < g_spec.section_count; index++) {
        size_t length = strlen(g_spec.sections[index].name);
        /* This section name is emitted as a slash offset, so reserve its text. */
        if (length > 8) {
            g_sec_str[index] = at;
            at += (uint32_t)length + 1u;
        /* A short section name is copied into the header, so its string offset is zero. */
        } else {
            g_sec_str[index] = 0;
        }
    }
    return at;
}

/* emit_coff serializes g_spec into a COFF object image and returns the buffer, the size, and the string-table offset.
   It first calls string_table_size, then lays out section headers, aligned raw data, aligned relocations, the symbol table, and the string table.
   Section and symbol names longer than eight bytes use the string table; short names are copied inline.
   Each relocation becomes a 10-byte COFF record.
   After a well-formed image exists, the corrupt flag may overwrite the machine, the section count, the optional-header size, the symbol table, or the string-table size.
   CORRUPT_TRUNCATE leaves the bytes intact and reports truncate_to as the size.
   crash_if ends the test when the spec has no section or no symbol, or when a long section-name scratch exceeds eight characters.
   The buffer comes from allocate, which calls fail and does not return if the arena is exhausted. */
static Emitted emit_coff(void) {
    Emitted emitted;
    uint32_t raw_at[SPEC_SECTIONS];
    uint32_t reloc_at[SPEC_SECTIONS];
    uint32_t cursor;
    uint32_t sym_at;
    uint32_t str_at;
    uint32_t str_size;
    uint8_t *data;
    int index;
    /* The emitter refuses an object with no section or no symbol. */
    crash_if(g_spec.section_count < 1 || g_spec.symbol_count < 1);
    str_size = string_table_size();
    cursor = 20u + 40u * (uint32_t)g_spec.section_count;
    /* Reserve file space for each section's raw bytes and relocations. */
    for (index = 0; index < g_spec.section_count; index++) {
        SpecSection *section = &g_spec.sections[index];
        cursor = align4(cursor);
        /* Initialized bytes occupy file space. Remember where they will sit. */
        if (!section->uninit && section->size != 0) {
            raw_at[index] = cursor;
            cursor += section->size;
        /* BSS, or an empty payload, has a zero raw-data pointer. */
        } else {
            raw_at[index] = 0;
        }
        /* Relocations occupy ten bytes each, starting on a four-byte boundary. */
        if (section->reloc_count != 0) {
            cursor = align4(cursor);
            reloc_at[index] = cursor;
            cursor += (uint32_t)section->reloc_count * 10u;
        /* A section with no relocations stores a zero relocation pointer. */
        } else {
            reloc_at[index] = 0;
        }
    }
    cursor = align4(cursor);
    sym_at = cursor;
    cursor += (uint32_t)g_spec.symbol_count * 18u;
    /* The string table is addressed as the bytes immediately after the symbols. */
    str_at = cursor;
    cursor += str_size;
    data = (uint8_t *)allocate(cursor);
    write_u16(data, g_spec.machine);
    write_u16(data + 2, (uint16_t)g_spec.section_count);
    write_u32(data + 8, sym_at);
    write_u32(data + 12, (uint32_t)g_spec.symbol_count);
    /* Write each section header, then its raw bytes and relocation records. */
    for (index = 0; index < g_spec.section_count; index++) {
        SpecSection *section = &g_spec.sections[index];
        uint8_t *header = data + 20 + 40 * index;
        /* A long section name is the decimal string-table offset with a leading slash. */
        if (g_sec_str[index] != 0) {
            char scratch[16];
            size_t used = 0;
            used = write_text(scratch, sizeof scratch, used, "/");
            write_u(scratch, sizeof scratch, used, g_sec_str[index]);
            /* The slash-offset form must still fit in the eight-byte name field. */
            crash_if(strlen(scratch) > 8);
            memcpy(header, scratch, strlen(scratch));
        /* A short section name is copied straight into the header. */
        } else {
            size_t length = strlen(section->name);
            /* Cap the inline copy at the eight-byte header field. */
            if (length > 8) length = 8;
            memcpy(header, section->name, length);
        }
        write_u32(header + 8, section->size);
        write_u32(header + 16, section->uninit ? section->size : section->size);
        write_u32(header + 20, section->uninit ? 0u : raw_at[index]);
        write_u32(header + 24, reloc_at[index]);
        write_u16(header + 32, section->reloc_count);
        write_u16(header + 34, section->line_numbers);
        write_u32(header + 36, section->characteristics);
        /* Copy initialized payload into the raw-data slot reserved above. */
        if (!section->uninit && section->size != 0) {
            memcpy(data + raw_at[index], section->bytes, section->size);
        }
        /* This section has relocation records to emit after its raw data. */
        if (section->reloc_count != 0) {
            uint16_t reloc;
            /* Write each relocation's offset, symbol index, and type. */
            for (reloc = 0; reloc < section->reloc_count; reloc++) {
                uint8_t *record = data + reloc_at[index] + (uint32_t)reloc * 10u;
                write_u32(record, section->reloc_va[reloc]);
                write_u32(record + 4, section->reloc_sym[reloc]);
                write_u16(record + 8, section->reloc_type[reloc]);
            }
        }
    }
    /* Emit one 18-byte symbol record per spec symbol. */
    for (index = 0; index < g_spec.symbol_count; index++) {
        write_coff_symbol(data + sym_at + (uint32_t)index * 18u, g_spec.symbols[index].name,
                          g_sym_str[index], g_spec.symbols[index].value, g_spec.symbols[index].section_number);
    }
    write_u32(data + str_at, str_size);
    /* Copy long symbol names into the string table. */
    for (index = 0; index < g_spec.symbol_count; index++) {
        /* Only names that did not fit inline have a string-table slot. */
        if (g_sym_str[index] != 0) {
            size_t length = strlen(g_spec.symbols[index].name);
            memcpy(data + str_at + g_sym_str[index], g_spec.symbols[index].name, length + 1u);
        }
    }
    /* Copy long section names into the same string table. */
    for (index = 0; index < g_spec.section_count; index++) {
        /* Only section names that used the slash-offset form are written out. */
        if (g_sec_str[index] != 0) {
            size_t length = strlen(g_spec.sections[index].name);
            memcpy(data + str_at + g_sec_str[index], g_spec.sections[index].name, length + 1u);
        }
    }
    /* Overwrite the machine field with the forced value, or 0xAAAA when none was set. */
    if (g_spec.corrupt == CORRUPT_MACHINE) {
        write_u16(data, g_spec.force_machine != 0 ? g_spec.force_machine : 0xaaaau);
    /* Claim the object has no sections after the headers were written. */
    } else if (g_spec.corrupt == CORRUPT_ZERO_SECTIONS) {
        write_u16(data + 2, 0);
    /* Set a nonzero optional-header size so the file is not a plain object. */
    } else if (g_spec.corrupt == CORRUPT_OPTIONAL) {
        write_u16(data + 16, 32);
    /* Zero the symbol-table pointer and count so the linker reports no symbols. */
    } else if (g_spec.corrupt == CORRUPT_SYMBOLS) {
        write_u32(data + 8, 0);
        write_u32(data + 12, 0);
    /* Publish a string-table size that runs far past the file. */
    } else if (g_spec.corrupt == CORRUPT_STRINGS) {
        write_u32(data + str_at, 0x0fffffffu);
    }
    emitted.data = data;
    emitted.size = g_spec.corrupt == CORRUPT_TRUNCATE ? g_spec.truncate_to : cursor;
    emitted.string_off = str_at;
    return emitted;
}

/* try_link_bytes links one in-memory COFF buffer the same way the CLI links a file.
   It clears the previous diagnostic, copies the bytes, subsystem, pack flag, and imports, then runs parse, layout, copy, imports, entry, relocations, and build_image.
   On success it fills LinkInfo from the linker and returns 1.
   fail() prints the error and exits, so this function does not return on a rejected object.
   The image pointer is arena memory and remains valid until the next arena_reset. */
static int try_link_bytes(uint8_t *data, size_t size, int pack, uint16_t subsystem, const char *entry,
                          const char **imports, int import_count, LinkInfo *info) {
    Linker linker;
    int index;
    g_error_text[0] = 0;
    memset(&linker, 0, sizeof linker);
    memset(info, 0, sizeof *info);
    linker.file_data = data;
    linker.file_size = size;
    linker.subsystem = subsystem;
    linker.pack_headers = pack ? true : false;
    /* Register each import specification before the object is parsed. */
    for (index = 0; index < import_count; index++) add_import(&linker, imports[index]);
    parse_object(&linker);
    prepare_imports(&linker);
    assign_layout(&linker);
    copy_section_data(&linker);
    build_import_data(&linker);
    select_entry(&linker, entry);
    apply_relocations(&linker);
    info->image = build_image(&linker, &info->image_size);
    info->emitted_section_count = linker.emitted_section_count;
    info->machine = linker.machine;
    info->subsystem = linker.subsystem;
    info->entry_name = linker.entry_name;
    return 1;
}

/* link_spec emits g_spec and links that object.
   arena_reset drops the previous image and allocations first, then emit_coff builds the COFF bytes.
   The return value is try_link_bytes: 1 when a PE was produced. A rejected object never returns, because fail() exits.
   pack, subsystem, entry, and the import list are forwarded unchanged.
   Callers that expect failure use expect_fail, which runs a child process. */
static int link_spec(int pack, uint16_t subsystem, const char *entry, const char **imports, int import_count, LinkInfo *info) {
    Emitted emitted;
    arena_reset();
    emitted = emit_coff();
    return try_link_bytes(emitted.data, emitted.size, pack, subsystem, entry, imports, import_count, info);
}

/* ret_object builds the smallest linkable object: a .text of the requested size whose first byte is the RET opcode 0xC3, plus a start symbol at offset 0.
   spec_reset selects the machine and clears leftover sections.
   The rest of the section bytes stay zero.
   It does not link the object; expect_ok or expect_fail does that later.
   A size that breaks spec_add_section ends the test through crash_if. */
static void ret_object(uint16_t machine, uint32_t size) {
    spec_reset(machine);
    spec_add_section(".text", TEXT_CHARS, NULL, size, 0);
    g_spec.sections[0].bytes[0] = 0xc3;
    spec_add_symbol("start", 0, 1);
}

/* rva_to_offset maps an RVA in a linked PE back to a file offset.
   It walks the section table and, when the RVA falls in a section's raw range, returns the corresponding file pointer.
   An RVA that misses every raw range is accepted only when it lies inside the header span, which is where pack-headers places .rdata.
   crash_if ends the test when the image is too small to hold the headers, the mapped offset is past the file, or the RVA is neither in a section nor in the headers.
   The returned offset is the position check_import_binding then reads. */
static uint32_t rva_to_offset(const uint8_t *image, size_t image_size, uint32_t rva, uint32_t headers) {
    uint32_t pe;
    uint32_t optional;
    uint16_t count;
    uint16_t index;
    /* The DOS header is not fully present. */
    crash_if(image_size < 0x40);
    pe = read_u32(image + 0x3c);
    /* The PE signature and file header do not fit in the image. */
    crash_if((size_t)pe + 24u > image_size);
    optional = read_u16(image + pe + 20);
    count = read_u16(image + pe + 6);
    /* The section table does not fit in the image. */
    crash_if((size_t)pe + 24u + optional + (size_t)count * 40u > image_size);
    /* Search the real section table for a raw range that covers this RVA. */
    for (index = 0; index < count; index++) {
        const uint8_t *header = image + pe + 24 + optional + (uint32_t)index * 40u;
        uint32_t virtual_address = read_u32(header + 12);
        uint32_t raw_size = read_u32(header + 16);
        uint32_t raw_pointer = read_u32(header + 20);
        /* This section's raw bytes contain the RVA. Return that file offset. */
        if (raw_size != 0 && rva >= virtual_address && rva - virtual_address < raw_size) {
            uint32_t offset = raw_pointer + (rva - virtual_address);
            /* The mapped file offset is outside the image. */
            crash_if((size_t)offset >= image_size);
            return offset;
        }
    }
    /* The RVA is not in a section and not in the headers either. */
    crash_if(!(rva < headers && (size_t)rva < image_size));
    return rva;
}

/* check_pe checks a linked image against the PE layout this linker promises.
   It requires DOS and NT signatures, the expected machine, subsystem, section count, entry RVA, alignments, image base, and NX compatibility.
   Section virtual addresses must increase and be section-aligned.
   Discarded names (.debug, .llvm, .drectve) and dollar signs must be absent.
   Raw data must be packed with no file gap after the headers, and the entry RVA must land in some section's virtual size.
   An expected file size or section count of zero skips that one comparison.
   Every miss goes through crash_if, which exits with the source line and does not return. */
static void check_pe(const LinkInfo *info, uint16_t expect_machine, uint16_t expect_subsystem,
                     uint32_t expect_file_size, uint16_t expect_sections, uint32_t expect_entry) {
    const uint8_t *image = info->image;
    size_t image_size = info->image_size;
    uint32_t pe;
    uint32_t optional_size;
    uint16_t magic;
    uint16_t sections;
    uint32_t headers;
    uint32_t entry;
    uint32_t raw_cursor;
    uint32_t previous_va = 0;
    int entry_inside = 0;
    uint16_t index;
    /* There is no image, or it is shorter than one file alignment unit. */
    crash_if(image == NULL || image_size < 512);
    /* The file does not start with the DOS signature. */
    crash_if(read_u16(image) != IMAGE_DOS_SIGNATURE);
    pe = read_u32(image + 0x3c);
    /* The e_lfanew header does not fit in the image. */
    crash_if((size_t)pe + 24u > image_size);
    /* The NT signature is missing at e_lfanew. */
    crash_if(read_u32(image + pe) != IMAGE_NT_SIGNATURE);
    /* The PE machine is not the machine this case requested. */
    crash_if(read_u16(image + pe + 4) != expect_machine);
    /* The linker object recorded a different machine than the PE header. */
    crash_if(info->machine != expect_machine);
    sections = read_u16(image + pe + 6);
    /* The PE section count disagrees with the linker. */
    crash_if(sections != info->emitted_section_count);
    /* When the oracle named a section count, the PE must match it. */
    if (expect_sections != 0) crash_if(sections != expect_sections);
    optional_size = read_u16(image + pe + 20);
    magic = read_u16(image + pe + 24);
    /* AMD64 must carry the PE32+ optional header and the 64-bit image base. */
    if (expect_machine == IMAGE_FILE_MACHINE_AMD64) {
        /* The optional-header magic is not PE32+. */
        crash_if(magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC);
        /* The AMD64 optional header has the wrong size. */
        crash_if(optional_size != sizeof(IMAGE_OPTIONAL_HEADER64));
        /* The AMD64 image base is not the fixed linker base. */
        crash_if(read_u64(image + pe + 48) != IMAGE_BASE64);
    /* I386 must carry the PE32 optional header and the 32-bit image base. */
    } else {
        /* The optional-header magic is not PE32. */
        crash_if(magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC);
        /* The I386 optional header has the wrong size. */
        crash_if(optional_size != sizeof(IMAGE_OPTIONAL_HEADER32));
        /* The I386 image base is not the fixed linker base. */
        crash_if(read_u32(image + pe + 52) != (uint32_t)IMAGE_BASE32);
    }
    /* Section alignment is not the linker's 4096-byte value. */
    crash_if(read_u32(image + pe + 56) != SECTION_ALIGNMENT);
    /* File alignment is not the linker's 512-byte value. */
    crash_if(read_u32(image + pe + 60) != FILE_ALIGNMENT);
    headers = read_u32(image + pe + 84);
    /* SizeOfHeaders must be a positive file-aligned span inside the image. */
    crash_if(headers == 0 || (headers % FILE_ALIGNMENT) != 0 || headers > image_size);
    /* The section table must sit inside SizeOfHeaders. */
    crash_if((size_t)pe + 24u + optional_size + (size_t)sections * 40u > headers);
    entry = read_u32(image + pe + 40);
    /* AddressOfEntryPoint is not the RVA this case required. */
    crash_if(entry != expect_entry);
    /* The linker did not record an entry symbol name. */
    crash_if(info->entry_name == NULL || info->entry_name[0] == 0);
    /* The optional-header subsystem is not the expected one. */
    crash_if(read_u16(image + pe + 92) != expect_subsystem);
    /* The linker object recorded a different subsystem than requested. */
    crash_if(info->subsystem != expect_subsystem);
    /* NX compatibility is not set in DllCharacteristics. */
    crash_if((read_u16(image + pe + 94) & IMAGE_DLLCHARACTERISTICS_NX_COMPAT) == 0);
    raw_cursor = headers;
    /* Check every emitted section header and pack its raw data. */
    for (index = 0; index < sections; index++) {
        const uint8_t *header = image + pe + 24 + optional_size + (uint32_t)index * 40u;
        char name[9];
        uint32_t virtual_size = read_u32(header + 8);
        uint32_t virtual_address = read_u32(header + 12);
        uint32_t raw_size = read_u32(header + 16);
        uint32_t raw_pointer = read_u32(header + 20);
        memcpy(name, header, 8);
        name[8] = 0;
        /* A dollar-suffixed COFF name should have been merged or discarded. */
        crash_if(strchr(name, '$') != NULL);
        /* A .debug section should have been discarded. */
        crash_if(strncmp(name, ".debug", 6) == 0);
        /* A .llvm section should have been discarded. */
        crash_if(strncmp(name, ".llvm", 5) == 0);
        /* The .drectve section should have been consumed and omitted. */
        crash_if(strcmp(name, ".drectve") == 0);
        /* Virtual addresses must strictly increase and be section-aligned. */
        crash_if(virtual_address <= previous_va || (virtual_address % SECTION_ALIGNMENT) != 0);
        previous_va = virtual_address;
        /* Remember when the entry RVA falls inside this section's virtual span. */
        if (virtual_size != 0 && entry >= virtual_address && entry - virtual_address < virtual_size) entry_inside = 1;
        /* An empty raw section, such as BSS, must not claim a file pointer. */
        if (raw_size == 0) {
            /* A zero raw size with a nonzero file pointer is a broken layout. */
            crash_if(raw_pointer != 0);
            continue;
        }
        /* Raw sections must be packed immediately after the headers and each other. */
        crash_if(raw_pointer != raw_cursor);
        /* This section's raw bytes run past the end of the image. */
        crash_if((size_t)raw_pointer + raw_size > image_size);
        raw_cursor += raw_size;
    }
    /* The entry RVA never landed in a section. */
    crash_if(!entry_inside);
    /* The file must end where the last raw section ends. */
    crash_if(image_size != raw_cursor);
    /* When the oracle named a file size, the image must have that length. */
    if (expect_file_size != 0) crash_if(image_size != (size_t)expect_file_size);
}

/* section_named reports whether the linked PE has a section header whose eight-byte name equals name.
   It reads the PE offset at 0x3C, then walks every section header after the optional header.
   A match returns 1. No match returns 0.
   It does not call fail. Callers use the result to prove .rdata was emitted or was packed away.
   The image is assumed to be large enough; a short image is a bug in an earlier oracle. */
static int section_named(const LinkInfo *info, const char *name) {
    uint32_t pe = read_u32(info->image + 0x3c);
    uint32_t optional = read_u16(info->image + pe + 20);
    uint16_t count = read_u16(info->image + pe + 6);
    uint16_t index;
    /* Walk each PE section header looking for this name. */
    for (index = 0; index < count; index++) {
        char found[9];
        memcpy(found, info->image + pe + 24 + optional + (uint32_t)index * 40u, 8);
        found[8] = 0;
        /* This header's name matches, so the section is present. */
        if (strcmp(found, name) == 0) return 1;
    }
    return 0;
}

/* find_bytes searches the first limit bytes of image for an exact needle.
   It returns 1 on the first match and 0 when the needle cannot fit or never appears.
   The packed-.rdata oracle uses it to find the 0xA5 fill inside SizeOfHeaders.
   It does not call fail and does not read past limit. */
static int find_bytes(const uint8_t *image, size_t limit, const uint8_t *needle, size_t needle_length) {
    size_t offset;
    size_t index;
    /* The window is shorter than the needle, so it cannot occur. */
    if (limit < needle_length) return 0;
    /* Try every start offset where the whole needle still fits in the window. */
    for (offset = 0; offset + needle_length <= limit; offset++) {
        /* Compare the needle one byte at a time. */
        for (index = 0; index < needle_length; index++) {
            /* This offset mismatches. Try the next start. */
            if (image[offset + index] != needle[index]) break;
        }
        /* Every needle byte matched, so the pattern is present. */
        if (index == needle_length) return 1;
    }
    return 0;
}

/* check_import_binding checks that an ExitProcess import was relocated the way the linker claims.
   It finds the .text raw bytes, requires exactly one FF 25 indirect jump, and reads the import descriptor.
   The descriptor's DLL name must be kernel32.dll and the image must contain the text ExitProcess.
   On AMD64 the absolute slot at raw offset 16 is the image base plus the IAT RVA when want_iat is set, otherwise plus the thunk RVA, and the FF 25 disp32 must point at the IAT.
   On I386 the same slot is a 32-bit VA and the instruction operand is the absolute IAT address.
   A missing .text, a wrong jump count, or a bad address ends the test through crash_if and does not return. */
static void check_import_binding(const LinkInfo *info, int want_iat) {
    const uint8_t *image = info->image;
    uint32_t pe = read_u32(image + 0x3c);
    uint32_t optional = read_u16(image + pe + 20);
    uint16_t count = read_u16(image + pe + 6);
    uint32_t headers = read_u32(image + pe + 84);
    uint16_t index;
    uint32_t text_raw = 0;
    uint32_t text_va = 0;
    uint32_t text_raw_size = 0;
    uint32_t ff_count = 0;
    uint32_t thunk_rva = 0;
    uint32_t import_rva;
    uint32_t descriptor;
    uint32_t iat_rva;
    uint32_t name_rva;
    uint64_t base;
    int is64 = info->machine == IMAGE_FILE_MACHINE_AMD64;
    int found_text = 0;
    /* Find the .text section header and remember its RVA and raw location. */
    for (index = 0; index < count; index++) {
        const uint8_t *header = image + pe + 24 + optional + (uint32_t)index * 40u;
        char name[9];
        memcpy(name, header, 8);
        name[8] = 0;
        /* This is .text. Record the virtual address, raw size, and file pointer. */
        if (strcmp(name, ".text") == 0) {
            text_va = read_u32(header + 12);
            text_raw_size = read_u32(header + 16);
            text_raw = read_u32(header + 20);
            found_text = 1;
        }
    }
    /* The import object must have emitted a .text section. */
    crash_if(!found_text);
    /* The import template needs 24 bytes of code to hold the slot and the jump. */
    crash_if(text_raw_size < 24);
    /* Scan .text for the FF 25 indirect jump. */
    for (index = 0; index + 1u < text_raw_size; index++) {
        /* Count this FF 25 and remember its RVA. The oracle expects exactly one. */
        if (image[text_raw + index] == 0xff && image[text_raw + index + 1u] == 0x25) {
            ff_count++;
            thunk_rva = text_va + index;
        }
    }
    /* There must be one indirect jump, not zero and not several. */
    crash_if(ff_count != 1);
    import_rva = read_u32(image + pe + (is64 ? 144u : 128u));
    /* The import data directory is empty. */
    crash_if(import_rva == 0);
    descriptor = rva_to_offset(image, info->image_size, import_rva, headers);
    /* The first import descriptor does not fit in the image. */
    crash_if((size_t)descriptor + 20u > info->image_size);
    name_rva = read_u32(image + descriptor + 12);
    iat_rva = read_u32(image + descriptor + 16);
    /* The import DLL name is not kernel32.dll. */
    crash_if(strcmp((const char *)image + rva_to_offset(image, info->image_size, name_rva, headers), "kernel32.dll") != 0);
    /* The import name ExitProcess is missing from the image. */
    crash_if(!image_has(image, info->image_size, "ExitProcess"));
    base = is64 ? IMAGE_BASE64 : IMAGE_BASE32;
    /* AMD64: the absolute slot and the RIP-relative disp32 must both name the right address. */
    if (is64) {
        uint64_t value = read_u64(image + text_raw + 16);
        uint32_t disp = read_u32(image + text_raw + (thunk_rva - text_va) + 2u);
        int32_t expected_disp = (int32_t)iat_rva - (int32_t)(thunk_rva + 6u);
        /* The 64-bit slot is not the IAT or the thunk, as this case required. */
        crash_if(value != base + (want_iat ? iat_rva : thunk_rva));
        /* An IAT binding must not still point at the thunk slot. */
        if (want_iat) crash_if(value == base + thunk_rva);
        /* The FF 25 displacement does not reach the IAT. */
        crash_if(disp != (uint32_t)expected_disp);
    /* I386: the absolute slot and the instruction operand are 32-bit virtual addresses. */
    } else {
        uint32_t value = read_u32(image + text_raw + 16);
        uint32_t slot = read_u32(image + text_raw + (thunk_rva - text_va) + 2u);
        /* The 32-bit slot is not the IAT or the thunk, as this case required. */
        crash_if(value != (uint32_t)(base + (want_iat ? iat_rva : thunk_rva)));
        /* An IAT binding must not still point at the thunk slot. */
        if (want_iat) crash_if(value == (uint32_t)(base + thunk_rva));
        /* The FF 25 operand is not the absolute IAT address. */
        crash_if(slot != (uint32_t)(base + iat_rva));
    }
}

/* expect_ok links the current spec and checks the resulting PE.
   It stores name in g_case so a later crash names the oracle, then calls link_spec with the active import list.
   A failed link ends the test through crash_if.
   A successful link is passed to check_pe with the expected machine, subsystem, file size, section count, and entry RVA.
   file_size or sections of zero lets check_pe skip that one comparison. This helper does not return after a miss. */
static void expect_ok(const char *name, int pack, uint16_t subsystem, const char *entry,
                      uint32_t file_size, uint16_t sections, uint32_t entry_rva, LinkInfo *info) {
    int ok;
    g_case = name;
    ok = link_spec(pack, subsystem, entry, g_imp_count > 0 ? g_imp_ptr : NULL, g_imp_count, info);
    crash_if(!ok);
    check_pe(info, g_spec.machine, subsystem, file_size, sections, entry_rva);
}

/* write_file_w is defined with the CLI file helpers. The self-test uses it to
   store a generated object next to the executable before a child linker reads it. */
static void write_file_w(const wchar_t *path, const uint8_t *data, size_t size);

#define FILE_SHARE_WRITE 0x00000002u
#define STARTF_USESTDHANDLES 0x00000100u
#define CREATE_NO_WINDOW 0x08000000u

/* Security descriptor and inheritance flag passed to CreateFileW for the child error file.
   The self-test fills one of these so the child can inherit the write handle. */
typedef struct {
    /* Byte length of this record. The test sets it to the struct size so the kernel reads bInheritHandle. */
    DWORD nLength;
    /* Optional security descriptor pointer. The test stores NULL so the new handle keeps the default DACL. */
    void *lpSecurityDescriptor;
    /* Nonzero when a child may inherit the handle. The test sets it so the child receives the error file. */
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;

/* Startup record CreateProcessW reads for the child window and standard handles.
   The self-test zeroes it, then sets the size, the std-handle flag, and the three handles. */
typedef struct {
    /* Byte length of STARTUPINFOW. The test sets it to the struct size before CreateProcessW. */
    DWORD cb;
    /* Reserved pointer that must stay NULL. The test never writes it after the zero fill. */
    wchar_t *lpReserved;
    /* Desktop name for the child. Left NULL so the child stays on the parent desktop. */
    wchar_t *lpDesktop;
    /* Console title. Left NULL because the child is created with CREATE_NO_WINDOW. */
    wchar_t *lpTitle;
    /* Suggested window origin x. Left zero; STARTF_USEPOSITION is never set. */
    DWORD dwX;
    /* Suggested window origin y. Left zero and ignored for the same reason as dwX. */
    DWORD dwY;
    /* Suggested window width. Left zero; the size flag is not set. */
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
    /* Child standard-input handle. The test passes the error file so the child does not read a console. */
    HANDLE hStdInput;
    /* Child standard-output handle. The test passes the same error file. */
    HANDLE hStdOutput;
    /* Child standard-error handle. fail() writes the diagnostic here. */
    HANDLE hStdError;
} STARTUPINFOW;

/* Handles and ids CreateProcessW writes after it starts a child.
   The self-test waits on the process, reads the exit code, and closes both handles. */
typedef struct {
    /* Process handle of the child. The test waits on it and reads the exit code. */
    HANDLE hProcess;
    /* Primary-thread handle. The test closes it after CreateProcessW and does not wait on the thread. */
    HANDLE hThread;
    /* Process id assigned by the kernel. CreateProcessW writes it; the test does not read it. */
    DWORD dwProcessId;
    /* Primary-thread id. CreateProcessW writes it; the test does not read it. */
    DWORD dwThreadId;
} PROCESS_INFORMATION;

typedef char sa_size_ok[(sizeof(SECURITY_ATTRIBUTES) == 24) ? 1 : -1];
typedef char startup_size_ok[(sizeof(STARTUPINFOW) == 104) ? 1 : -1];
typedef char pi_size_ok[(sizeof(PROCESS_INFORMATION) == 24) ? 1 : -1];

extern BOOL CreateProcessW(const wchar_t *application, wchar_t *command, SECURITY_ATTRIBUTES *process_attr, SECURITY_ATTRIBUTES *thread_attr, BOOL inherit, DWORD flags, void *environment, const wchar_t *directory, STARTUPINFOW *startup, PROCESS_INFORMATION *info);
extern DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
extern BOOL GetExitCodeProcess(HANDLE process, DWORD *code);
extern BOOL TerminateProcess(HANDLE process, DWORD code);
extern BOOL DeleteFileW(const wchar_t *path);

/* exe_path writes the running executable's path into path.
   GetModuleFileNameW fills the buffer, including the file name.
   A missing path or a path that does not fit ends the test through test_fail_at_line.
   capacity counts wide characters and includes the room for the NUL. */
static void exe_path(wchar_t *path, DWORD capacity) {
    DWORD length = GetModuleFileNameW(NULL, path, capacity);
    /* The module path is missing or does not fit in the caller's buffer. */
    if (length == 0 || length >= capacity) test_fail_at_line(__LINE__);
}

/* append_wchar adds one wide character and keeps the buffer NUL-terminated.
   used is the current length, not counting the NUL. capacity is the buffer size in wide characters.
   Running out of room ends the test. The character is stored before the new NUL. */
static void append_wchar(wchar_t *text, int *used, int capacity, wchar_t ch) {
    /* The next character and its NUL do not fit. */
    if (*used + 1 >= capacity) test_fail_at_line(__LINE__);
    text[*used] = ch;
    *used = *used + 1;
    text[*used] = 0;
}

/* append_ascii appends a narrow string as wide characters with the same code points.
   A NULL string adds nothing. Each byte is widened without a code-page conversion.
   Overflow ends the test inside append_wchar. The text must be ASCII. */
static void append_ascii(wchar_t *text, int *used, int capacity, const char *ascii) {
    int index = 0;
    /* There is no narrow text to append. */
    if (ascii == NULL) return;
    /* Widen every byte until the NUL. */
    while (ascii[index] != 0) {
        append_wchar(text, used, capacity, (wchar_t)(unsigned char)ascii[index]);
        index++;
    }
}

/* append_quoted appends a wide string wrapped in double quotes.
   CreateProcessW uses those quotes so a path that contains spaces stays one argument.
   A NULL value ends the test. Overflow ends the test inside append_wchar. */
static void append_quoted(wchar_t *text, int *used, int capacity, const wchar_t *value) {
    int index = 0;
    /* A missing path cannot be quoted. */
    if (value == NULL) test_fail_at_line(__LINE__);
    append_wchar(text, used, capacity, L'"');
    /* Copy the path characters between the quotes. */
    while (value[index] != 0) {
        append_wchar(text, used, capacity, value[index]);
        index++;
    }
    append_wchar(text, used, capacity, L'"');
}

/* sibling_path builds a path in the executable's directory with the given leaf name.
   It copies the directory portion of the module path and then the leaf.
   A path that does not fit ends the test. dest receives a NUL-terminated wide path. */
static void sibling_path(wchar_t *dest, DWORD capacity, const wchar_t *leaf) {
    wchar_t exe[400];
    DWORD length = 0;
    DWORD slash = 0;
    DWORD index;
    DWORD leaf_index = 0;
    int found = 0;
    exe_path(exe, 400);
    /* Find the last directory separator so the leaf replaces the file name. */
    while (exe[length] != 0) {
        /* Remember the offset just past this separator. */
        if (exe[length] == L'\\' || exe[length] == L'/') {
            slash = length + 1u;
            found = 1;
        }
        length++;
    }
    /* No separator was present. Write the leaf at the start of the buffer. */
    if (!found) slash = 0;
    index = 0;
    /* Copy the directory, including the separator. */
    while (index < slash) {
        /* The directory alone does not fit. */
        if (index + 1u >= capacity) test_fail_at_line(__LINE__);
        dest[index] = exe[index];
        index++;
    }
    dest[index] = 0;
    /* Copy the leaf after the directory. */
    while (leaf[leaf_index] != 0) {
        /* The combined path does not fit. */
        if (index + 1u >= capacity) test_fail_at_line(__LINE__);
        dest[index] = leaf[leaf_index];
        index++;
        leaf_index++;
    }
    dest[index] = 0;
}

/* read_captured_error copies the child diagnostic file into g_error_text.
   It opens the path, reads at most one byte less than the buffer, and NUL-terminates the result.
   A missing file leaves g_error_text empty. The handle is closed before return.
   The caller then looks for the expected snippet in that text. */
static void read_captured_error(const wchar_t *path) {
    HANDLE file;
    DWORD got = 0;
    g_error_text[0] = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    /* The child wrote nothing, or the file could not be opened. */
    if (file == INVALID_HANDLE_VALUE) return;
    /* Read the diagnostic. A failed read leaves the buffer empty. */
    if (!ReadFile(file, g_error_text, (DWORD)(sizeof g_error_text - 1u), &got, NULL)) got = 0;
    /* A full buffer must still end with a NUL. */
    if (got >= sizeof g_error_text) got = (DWORD)(sizeof g_error_text - 1u);
    g_error_text[got] = 0;
    CloseHandle(file);
}

/* run_command_expect_fail starts command as a hidden child and requires it to fail.
   Standard error is redirected to self_fail.err beside the executable. fail() in the child writes that file and exits 1.
   The wait is 60 seconds. A timeout kills the child and ends the test.
   crash_if ends the test when the process cannot be started, the exit code is not 1, or the captured text misses snippet.
   g_error_text holds the captured diagnostic so test_fail_at_line can log it. */
static void run_command_expect_fail(wchar_t *command, const char *snippet) {
    wchar_t err_path[400];
    SECURITY_ATTRIBUTES security;
    STARTUPINFOW startup;
    PROCESS_INFORMATION info;
    HANDLE err_file;
    DWORD waited;
    DWORD exit_code = 0;
    sibling_path(err_path, 400, L"self_fail.err");
    DeleteFileW(err_path);
    memset(&security, 0, sizeof security);
    security.nLength = sizeof security;
    security.lpSecurityDescriptor = NULL;
    security.bInheritHandle = 1;
    err_file = CreateFileW(err_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    /* The diagnostic file could not be created, so the child output would be lost. */
    if (err_file == INVALID_HANDLE_VALUE) {
        write_text(g_error_text, sizeof g_error_text, 0, "cannot create self-test error file");
        crash_if(1);
    }
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = err_file;
    startup.hStdOutput = err_file;
    startup.hStdError = err_file;
    memset(&info, 0, sizeof info);
    /* CreateProcessW can edit the command buffer. A failed start ends the test. */
    if (!CreateProcessW(NULL, command, NULL, NULL, 1, CREATE_NO_WINDOW, NULL, NULL, &startup, &info)) {
        CloseHandle(err_file);
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot start small_linker (error ");
        used = write_u(g_error_text, sizeof g_error_text, used, (unsigned)GetLastError());
        write_text(g_error_text, sizeof g_error_text, used, ")");
        crash_if(1);
    }
    CloseHandle(err_file);
    CloseHandle(info.hThread);
    waited = WaitForSingleObject(info.hProcess, 60000);
    /* The child did not exit in time. Stop it and fail the test. */
    if (waited != 0) {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hProcess);
        write_text(g_error_text, sizeof g_error_text, 0, "self-test child timed out");
        crash_if(1);
    }
    /* The exit code is 1 when fail() ran. Any other code fails the oracle. */
    if (!GetExitCodeProcess(info.hProcess, &exit_code)) exit_code = 0;
    CloseHandle(info.hProcess);
    read_captured_error(err_path);
    DeleteFileW(err_path);
    /* The child did not exit through fail(). */
    crash_if(exit_code != 1);
    /* fail() ran, but the diagnostic missed the expected snippet. */
    crash_if(snippet == NULL || !contains(g_error_text, snippet));
}

/* expect_cli_fail runs this executable with the given arguments and requires fail().
   count is the number of arguments after the program name. Each argument is quoted.
   The child prints the diagnostic and exits. This process checks the exit code and the snippet.
   A command that does not fit ends the test inside the append helpers. */
static void expect_cli_fail(const char *snippet, int count, char **args) {
    wchar_t exe[400];
    wchar_t command[4096];
    int used = 0;
    int index;
    exe_path(exe, 400);
    command[0] = 0;
    append_quoted(command, &used, 4096, exe);
    /* Quote every argument so spaces in a path stay inside one token. */
    for (index = 0; index < count; index++) {
        append_wchar(command, &used, 4096, L' ');
        append_wchar(command, &used, 4096, L'"');
        append_ascii(command, &used, 4096, args[index]);
        append_wchar(command, &used, 4096, L'"');
    }
    run_command_expect_fail(command, snippet);
}

/* expect_fail writes the current spec to an object file and requires a child linker to reject it.
   The child uses the Windows GUI subsystem, the pack flag, the entry name, and the active import list.
   crash_if ends the test if that child exits with any code other than 1 or if its diagnostic misses snippet.
   The case name is stored first so the failure log identifies the oracle.
   Temporary object and executable files beside this executable are removed after the child exits. */
static void expect_fail(const char *name, int pack, const char *entry, const char *snippet) {
    Emitted emitted;
    wchar_t exe[400];
    wchar_t obj_path[400];
    wchar_t out_path[400];
    wchar_t command[4096];
    int used = 0;
    int index;
    g_case = name;
    arena_reset();
    emitted = emit_coff();
    exe_path(exe, 400);
    sibling_path(obj_path, 400, L"self_fail.obj");
    sibling_path(out_path, 400, L"self_fail.exe");
    DeleteFileW(out_path);
    write_file_w(obj_path, emitted.data, emitted.size);
    command[0] = 0;
    append_quoted(command, &used, 4096, exe);
    append_wchar(command, &used, 4096, L' ');
    append_quoted(command, &used, 4096, obj_path);
    append_ascii(command, &used, 4096, " /out:");
    append_quoted(command, &used, 4096, out_path);
    /* Ask the child to pack a read-only .rdata into the header when this case does. */
    if (pack) append_ascii(command, &used, 4096, " /pack-headers");
    /* Pass an explicit entry name when the oracle named one. */
    if (entry != NULL) {
        append_ascii(command, &used, 4096, " /entry:");
        append_ascii(command, &used, 4096, entry);
    }
    /* Repeat /import for each specification the oracle stored. */
    for (index = 0; index < g_imp_count; index++) {
        append_wchar(command, &used, 4096, L' ');
        append_wchar(command, &used, 4096, L'"');
        append_ascii(command, &used, 4096, "/import:");
        append_ascii(command, &used, 4096, g_imp_ptr[index]);
        append_wchar(command, &used, 4096, L'"');
    }
    run_command_expect_fail(command, snippet);
    DeleteFileW(obj_path);
    DeleteFileW(out_path);
}

/* use_imports replaces the import list that the next link_spec call will pass.
   A NULL argument is skipped, so one or both strings may be omitted.
   The pointers are stored, not copied, and g_imp_count is the number of live entries.
   It cannot fail. The strings must outlive the following expect_ok or expect_fail call. */
static void use_imports(const char *first, const char *second) {
    g_imp_count = 0;
    /* Keep the first import specification. */
    if (first != NULL) g_imp_ptr[g_imp_count++] = first;
    /* Keep the second import specification when the caller passed one. */
    if (second != NULL) g_imp_ptr[g_imp_count++] = second;
}

/* load_reloc_object builds a 48-byte .text that exercises the relocation types the linker accepts.
   Two E8 bytes sit at offsets 0 and 8. start is at offset 0 and target is at offset 40, both in section 1.
   A REL32 at offset 1 aims at target. AMD64 also gets REL32_1 at offset 9.
   Offset 16 is ADDR64 or DIR32, offset 24 is ADDR32NB or DIR32NB, offset 28 is a section relocation, and offset 32 is SECREL.
   The later oracle checks the patched immediates in the PE. A capacity miss ends the test inside the spec helpers. */
static void load_reloc_object(uint16_t machine) {
    uint8_t bytes[48];
    memset(bytes, 0, sizeof bytes);
    bytes[0] = 0xe8;
    bytes[8] = 0xe8;
    spec_reset(machine);
    spec_add_section(".text", TEXT_CHARS, bytes, 48, 0);
    spec_add_symbol("start", 0, 1);
    spec_add_symbol("target", 40, 1);
    spec_add_reloc(0, 1, 1, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_REL32 : IMAGE_REL_I386_REL32);
    /* Only AMD64 has the REL32_1 form. Plant it at offset 9. */
    if (machine == IMAGE_FILE_MACHINE_AMD64) spec_add_reloc(0, 9, 1, IMAGE_REL_AMD64_REL32_1);
    spec_add_reloc(0, 16, 1, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_ADDR64 : IMAGE_REL_I386_DIR32);
    spec_add_reloc(0, 24, 1, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_ADDR32NB : IMAGE_REL_I386_DIR32NB);
    spec_add_reloc(0, 28, 1, IMAGE_REL_AMD64_SECTION);
    spec_add_reloc(0, 32, 1, IMAGE_REL_AMD64_SECREL);
}

/* load_import_object builds a 32-byte return stub that imports ExitProcess from kernel32.dll.
   want_iat selects the __imp_ spelling, which binds to the IAT; otherwise the symbol is the direct ExitProcess name.
   I386 uses the stdcall decorations _ExitProcess@4 and __imp__ExitProcess@4.
   A DIR32 or ADDR64 relocation at offset 16 targets that import symbol, which is an undefined external.
   use_imports then arms the matching /import specification for the next link. */
static void load_import_object(uint16_t machine, int want_iat) {
    const char *symbol = "ExitProcess";
    ret_object(machine, 32);
    /* I386 import names are stdcall-decorated, with __imp_ only when the IAT is requested. */
    if (machine == IMAGE_FILE_MACHINE_I386) symbol = want_iat ? "__imp__ExitProcess@4" : "_ExitProcess@4";
    /* AMD64 IAT binding uses the undecorated __imp_ExitProcess symbol. */
    else if (want_iat) symbol = "__imp_ExitProcess";
    spec_add_symbol(symbol, 0, 0);
    spec_add_reloc(0, 16, 1, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_ADDR64 : IMAGE_REL_I386_DIR32);
    use_imports("kernel32.dll,ExitProcess", NULL);
}

/* fuzz_test_layout_oracles runs the fixed layout cases that the random fuzzer does not spell out one by one.
   It links minimal AMD64 and I386 images, unpacked and packed .rdata, BSS, both relocation sets, overflow failures, absolute and merged sections, directives, entry selection, and import bindings.
   Each success calls expect_ok and check_pe, then checks bytes the generic checker cannot see, and records a COV_ id.
   Expected failures go through expect_fail with a snippet of the linker diagnostic.
   A miss ends the process at the failing source line. The function logs a single OK line when every oracle has passed. */
static void fuzz_test_layout_oracles(void) {
    LinkInfo info;
    uint8_t pattern[32];
    uint32_t unpacked_size;
    uint32_t packed_size;
    uint32_t bss_size;
    uint32_t headers;
    int index;
    g_imp_count = 0;
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    expect_ok("amd64 ret", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
    crash_if(info.image[512] != 0xc3);
    cov_set(COV_MIN_AMD64);
    ret_object(IMAGE_FILE_MACHINE_I386, 1);
    expect_ok("i386 ret", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
    crash_if(info.image[512] != 0xc3);
    cov_set(COV_MIN_I386);

    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    spec_add_section(".rdata", RDATA_CHARS, NULL, 32, 0);
    memset(g_spec.sections[1].bytes, 0xa5, 32);
    expect_ok("unpacked rdata", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1536, 2, 0x1000, &info);
    crash_if(!section_named(&info, ".rdata"));
    unpacked_size = (uint32_t)info.image_size;
    cov_set(COV_UNPACKED);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    spec_add_section(".rdata", RDATA_CHARS, NULL, 32, 0);
    memset(g_spec.sections[1].bytes, 0xa5, 32);
    expect_ok("packed rdata", 1, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
    crash_if(section_named(&info, ".rdata"));
    headers = read_u32(info.image + read_u32(info.image + 0x3c) + 84);
    memset(pattern, 0xa5, sizeof pattern);
    crash_if(!find_bytes(info.image, headers, pattern, sizeof pattern));
    packed_size = (uint32_t)info.image_size;
    crash_if(unpacked_size - packed_size != 512);
    cov_set(COV_PACK);
    cov_set(COV_PACK_DIFF);

    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    spec_add_section(".bss", BSS_CHARS, NULL, 32, 1);
    expect_ok("bss", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 2, 0x1000, &info);
    bss_size = (uint32_t)info.image_size;
    crash_if(bss_size != 1024);
    cov_set(COV_BSS);

    load_reloc_object(IMAGE_FILE_MACHINE_AMD64);
    expect_ok("amd64 relocs", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
    crash_if(info.image[512] != 0xe8);
    crash_if(read_u32(info.image + 513) != 35u);
    crash_if(info.image[520] != 0xe8);
    crash_if(read_u32(info.image + 521) != 26u);
    crash_if(read_u64(info.image + 528) != 0x140001028ull);
    crash_if(read_u32(info.image + 536) != 0x1028u);
    crash_if(read_u16(info.image + 540) != 1u);
    crash_if(read_u32(info.image + 544) != 40u);
    cov_set(COV_RELOC_AMD64);
    load_reloc_object(IMAGE_FILE_MACHINE_I386);
    expect_ok("i386 relocs", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
    crash_if(read_u32(info.image + 513) != 35u);
    crash_if(read_u32(info.image + 528) != 0x00401028u);
    crash_if(read_u32(info.image + 536) != 0x1028u);
    crash_if(read_u16(info.image + 540) != 1u);
    crash_if(read_u32(info.image + 544) != 40u);
    cov_set(COV_RELOC_I386);

    ret_object(IMAGE_FILE_MACHINE_AMD64, 48);
    g_spec.sections[0].bytes[0] = 0xff;
    g_spec.sections[0].bytes[1] = 0xff;
    g_spec.sections[0].bytes[2] = 0xff;
    g_spec.sections[0].bytes[3] = 0x7f;
    spec_add_symbol("target", 40, 1);
    spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_REL32);
    expect_fail("rel32 overflow", 0, NULL, "REL32");
    cov_set(COV_REL32_OVF);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 16);
    spec_add_symbol("target", 0, 1);
    spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_ADDR32);
    expect_fail("addr32 overflow", 0, NULL, "ADDR32");
    cov_set(COV_ADDR32);

    {
        uint8_t bytes[4];
        bytes[0] = 0x11;
        bytes[1] = 0x22;
        bytes[2] = 0x33;
        bytes[3] = 0x44;
        spec_reset(IMAGE_FILE_MACHINE_AMD64);
        spec_add_section(".text", TEXT_CHARS, bytes, 4, 0);
        spec_add_symbol("start", 0, 1);
        spec_add_reloc(0, 0, 0, IMAGE_REL_AMD64_ABSOLUTE);
        expect_ok("absolute", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
        crash_if(info.image[512] != 0x11 || info.image[513] != 0x22 || info.image[514] != 0x33 || info.image[515] != 0x44);
        cov_set(COV_ABSOLUTE);
    }
    {
        uint8_t first[4];
        uint8_t second[4];
        memset(first, 'A', 4);
        memset(second, 'B', 4);
        spec_reset(IMAGE_FILE_MACHINE_AMD64);
        spec_add_section(".text$a", TEXT_CHARS, first, 4, 0);
        spec_add_section(".text$b", TEXT_CHARS, second, 4, 0);
        spec_add_symbol("start", 0, 1);
        expect_ok("merge", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
        /* The merged .text must begin with the four A bytes from .text$a. */
        for (index = 0; index < 4; index++) crash_if(info.image[512 + index] != 'A');
        /* The next four bytes must be the B payload from .text$b. */
        for (index = 0; index < 4; index++) crash_if(info.image[516 + index] != 'B');
        cov_set(COV_MERGE);
    }
    {
        const char *directive = " /ENTRY:start ";
        ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
        spec_add_section(".drectve", IMAGE_SCN_LNK_REMOVE | IMAGE_SCN_CNT_INITIALIZED_DATA, directive, (uint32_t)strlen(directive), 0);
        spec_add_section(".debug$S", IMAGE_SCN_CNT_INITIALIZED_DATA, "dbg", 3, 0);
        spec_add_section(".llvm", IMAGE_SCN_CNT_INITIALIZED_DATA, "ll", 2, 0);
        expect_ok("directive", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1000, &info);
        crash_if(strcmp(info.entry_name, "start") != 0);
        cov_set(COV_DIRECTIVE);
    }
    ret_object(IMAGE_FILE_MACHINE_AMD64, 16);
    g_spec.sections[0].bytes[8] = 0xc3;
    spec_add_symbol("mainCRTStartup", 8, 1);
    expect_ok("entry order", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, NULL, 1024, 1, 0x1008, &info);
    crash_if(strcmp(info.entry_name, "mainCRTStartup") != 0);
    cov_set(COV_ENTRY_ORDER);
    ret_object(IMAGE_FILE_MACHINE_I386, 1);
    strcpy(g_spec.symbols[0].name, "_start");
    expect_ok("x86 decorated start", 0, IMAGE_SUBSYSTEM_WINDOWS_GUI, "start", 1024, 1, 0x1000, &info);
    crash_if(strcmp(info.entry_name, "start") != 0);
    cov_set(COV_X86_DECOR);

    load_import_object(IMAGE_FILE_MACHINE_AMD64, 0);
    expect_ok("amd64 direct import", 0, IMAGE_SUBSYSTEM_WINDOWS_CUI, "start", 1536, 2, 0x1000, &info);
    check_import_binding(&info, 0);
    cov_set(COV_IMP_DIR64);
    load_import_object(IMAGE_FILE_MACHINE_AMD64, 1);
    expect_ok("amd64 iat import", 0, IMAGE_SUBSYSTEM_WINDOWS_CUI, "start", 1536, 2, 0x1000, &info);
    check_import_binding(&info, 1);
    cov_set(COV_IMP_IAT64);
    load_import_object(IMAGE_FILE_MACHINE_I386, 0);
    expect_ok("i386 direct import", 0, IMAGE_SUBSYSTEM_WINDOWS_CUI, "start", 1536, 2, 0x1000, &info);
    check_import_binding(&info, 0);
    cov_set(COV_IMP_DIR32);
    load_import_object(IMAGE_FILE_MACHINE_I386, 1);
    expect_ok("i386 iat import", 0, IMAGE_SUBSYSTEM_WINDOWS_CUI, "start", 1536, 2, 0x1000, &info);
    check_import_binding(&info, 1);
    cov_set(COV_IMP_IAT32);
    g_imp_count = 0;

    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    expect_fail("pack without rdata", 1, NULL, "pack-headers");
    cov_set(COV_PACK_NONE);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    spec_add_section(".rdata", RDATA_CHARS, NULL, 400, 0);
    memset(g_spec.sections[1].bytes, 0xa5, 400);
    expect_fail("pack huge rdata", 1, NULL, ".rdata needs");
    cov_set(COV_PACK_HUGE);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 8);
    spec_add_section(".rdata", RDATA_CHARS, NULL, 16, 0);
    spec_add_symbol("label", 0, 2);
    spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_SECTION);
    expect_fail("packed section reloc", 1, NULL, "without an output section");
    cov_set(COV_PACK_SECTION);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 8);
    spec_add_section(".rdata", RDATA_CHARS, NULL, 16, 0);
    spec_add_symbol("label", 0, 2);
    spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_SECREL);
    expect_fail("packed secrel", 1, NULL, "without an output section");
    cov_set(COV_PACK_SECREL);

    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.corrupt = CORRUPT_MACHINE;
    g_spec.force_machine = 0xaaaau;
    expect_fail("bad machine", 0, NULL, "unsupported COFF machine");
    cov_set(COV_BAD_MACHINE);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.corrupt = CORRUPT_TRUNCATE;
    g_spec.truncate_to = 8;
    expect_fail("truncate", 0, NULL, "too small");
    cov_set(COV_TRUNC);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.corrupt = CORRUPT_ZERO_SECTIONS;
    expect_fail("zero sections", 0, NULL, "no sections");
    cov_set(COV_ZERO);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.corrupt = CORRUPT_OPTIONAL;
    expect_fail("optional header", 0, NULL, "optional header");
    cov_set(COV_OPTIONAL);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.corrupt = CORRUPT_STRINGS;
    expect_fail("string table", 0, NULL, "string table");
    cov_set(COV_STRINGS);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.corrupt = CORRUPT_SYMBOLS;
    expect_fail("symbol table", 0, NULL, "no COFF symbol table");
    cov_set(COV_SYMBOLS);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 16);
    spec_add_symbol("missing", 0, 0);
    spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_ADDR64);
    expect_fail("unresolved", 0, NULL, "unresolved external");
    cov_set(COV_UNRESOLVED);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.symbols[0].value = 2;
    expect_fail("symbol past section", 0, NULL, "beyond its section");
    cov_set(COV_PAST);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    g_spec.sections[0].line_numbers = 1;
    expect_fail("line numbers", 0, NULL, "line-number");
    cov_set(COV_LINES);
    ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
    strcpy(g_spec.symbols[0].name, "helper");
    expect_fail("no entry", 0, NULL, "no entry point");
    cov_set(COV_NO_ENTRY);
    test_log("layout oracles ............ OK\r\n");
}

/* build_valid fills g_spec and the expected PE outputs for one valid fuzzer kind.
   kind selects the shape. iter varies the machine, the subsystem, and a few payload bytes.
   On return, subsystem, pack, entry, file_size, sections, entry_rva, and binding describe what expect_ok and the import checker should accept.
   binding is 0 for no import check, 1 for a direct import, 2 for an IAT import, and 3 for name-only checks.
   An unknown kind ends the test through crash_if. This function does not link; the caller does. */
static void build_valid(int kind, int iter, uint16_t *subsystem, int *pack, const char **entry,
                        uint32_t *file_size, uint16_t *sections, uint32_t *entry_rva, int *binding) {
    /* Odd iterations are I386 and even iterations are AMD64, unless a case overrides that. */
    uint16_t machine = (iter & 1) ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    *pack = 0;
    *entry = NULL;
    *subsystem = (iter & 1) ? IMAGE_SUBSYSTEM_WINDOWS_CUI : IMAGE_SUBSYSTEM_WINDOWS_GUI;
    *file_size = 1024;
    *sections = 1;
    *entry_rva = 0x1000;
    *binding = 0;
    g_imp_count = 0;
    /* Pick the object shape for this valid kind. */
    switch (kind) {
    /* AMD64 two-byte return, a random second byte, GUI subsystem, one 1024-byte section. */
    case 0:
        ret_object(IMAGE_FILE_MACHINE_AMD64, 2);
        g_spec.sections[0].bytes[1] = (uint8_t)rng();
        *subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        break;
    /* I386 two-byte return with the same GUI layout as the AMD64 minimum. */
    case 1:
        ret_object(IMAGE_FILE_MACHINE_I386, 2);
        g_spec.sections[0].bytes[1] = (uint8_t)rng();
        *subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        break;
    /* Unpacked read-only .rdata. The file grows to 1536 bytes and keeps two sections. */
    case 2:
        ret_object(machine, 1);
        spec_add_section(".rdata", RDATA_CHARS, NULL, (uint32_t)(8 + (iter % 16)), 0);
        memset(g_spec.sections[1].bytes, (uint8_t)(0x20 + iter), g_spec.sections[1].size);
        *file_size = 1536;
        *sections = 2;
        break;
    /* Packed read-only .rdata. The section header disappears and the file stays 1024 bytes. */
    case 3:
        ret_object(machine, 1);
        spec_add_section(".rdata", RDATA_CHARS, NULL, (uint32_t)(4 + (iter % 20)), 0);
        memset(g_spec.sections[1].bytes, 0x5a, g_spec.sections[1].size);
        *pack = 1;
        break;
    /* Uninitialized .bss adds a section but no raw bytes, so the file stays 1024 bytes. */
    case 4:
        ret_object(machine, 1);
        spec_add_section(".bss", BSS_CHARS, NULL, (uint32_t)(16 + iter), 1);
        *sections = 2;
        break;
    /* Two COFF fragments, .text$a and .text$b, which the linker must merge into one .text. */
    case 5: {
        uint8_t first[4];
        uint8_t second[4];
        memset(first, 'A', 4);
        memset(second, 'B', 4);
        spec_reset(machine);
        spec_add_section(".text$a", TEXT_CHARS, first, 4, 0);
        spec_add_section(".text$b", TEXT_CHARS, second, 4, 0);
        spec_add_symbol("start", 0, 1);
        break;
    }
    /* An ABSOLUTE relocation must leave the four payload bytes unchanged. */
    case 6: {
        uint8_t bytes[4];
        bytes[0] = 0x11;
        bytes[1] = 0x22;
        bytes[2] = 0x33;
        bytes[3] = (uint8_t)iter;
        spec_reset(machine);
        spec_add_section(".text", TEXT_CHARS, bytes, 4, 0);
        spec_add_symbol("start", 0, 1);
        spec_add_reloc(0, 0, 0, IMAGE_REL_AMD64_ABSOLUTE);
        break;
    }
    /* A REL32 relocation at offset 1 targets the section's own start symbol. */
    case 7:
        ret_object(machine, 8);
        g_spec.sections[0].bytes[0] = 0xe8;
        spec_add_reloc(0, 1, 0, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_REL32 : IMAGE_REL_I386_REL32);
        break;
    /* An ADDR64 or DIR32 relocation at offset 4 asks for an absolute virtual address. */
    case 8:
        ret_object(machine, 16);
        spec_add_reloc(0, 4, 0, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_ADDR64 : IMAGE_REL_I386_DIR32);
        break;
    /* An ADDR32NB or DIR32NB relocation at offset 4 asks for an RVA. */
    case 9:
        ret_object(machine, 16);
        spec_add_reloc(0, 4, 0, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_ADDR32NB : IMAGE_REL_I386_DIR32NB);
        break;
    /* SECTION and SECREL relocations both target the code section's own symbol. */
    case 10:
        ret_object(machine, 16);
        spec_add_reloc(0, 0, 0, IMAGE_REL_AMD64_SECTION);
        spec_add_reloc(0, 4, 0, IMAGE_REL_AMD64_SECREL);
        break;
    /* A .drectve entry directive plus .debug and .llvm sections, which must be discarded. */
    case 11: {
        const char *directive = " /ENTRY:start ";
        ret_object(machine, 1);
        spec_add_section(".drectve", IMAGE_SCN_LNK_REMOVE | IMAGE_SCN_CNT_INITIALIZED_DATA, directive, (uint32_t)strlen(directive), 0);
        spec_add_section(".debug$S", IMAGE_SCN_CNT_INITIALIZED_DATA, "dbg", 3, 0);
        spec_add_section(".llvm", IMAGE_SCN_CNT_INITIALIZED_DATA, "ll", 2, 0);
        break;
    }
    /* A one-byte return with the defaults already stored above. */
    case 12:
        ret_object(machine, 1);
        break;
    /* A second symbol, main, is the explicit entry and sits at RVA 0x1004. */
    case 13:
        ret_object(machine, 8);
        spec_add_symbol("main", 4, 1);
        *entry = "main";
        *entry_rva = 0x1004;
        break;
    /* A direct ExitProcess import. The image has two sections and binding mode 1. */
    case 14:
        load_import_object(machine, 0);
        *file_size = 1536;
        *sections = 2;
        *entry = "start";
        *binding = 1;
        break;
    /* An IAT ExitProcess import. Binding mode 2 expects the __imp_ address. */
    case 15:
        load_import_object(machine, 1);
        *file_size = 1536;
        *sections = 2;
        *entry = "start";
        *binding = 2;
        break;
    /* Two kernel32 imports and no code relocation. Binding mode 3 checks names only. */
    case 16:
        ret_object(machine, 1);
        use_imports("kernel32.dll,ExitProcess", "kernel32.dll,GetTickCount");
        *file_size = 1536;
        *sections = 2;
        *binding = 3;
        break;
    /* The entry symbol is renamed to entrypoint and that name is passed to the linker. */
    case 17:
        ret_object(machine, 1);
        strcpy(g_spec.symbols[0].name, "entrypoint");
        *entry = "entrypoint";
        break;
    /* One dollar-suffixed .text$payload section holding the five bytes HELLO. */
    case 18:
        spec_reset(machine);
        spec_add_section(".text$payload", TEXT_CHARS, "HELLO", 5, 0);
        spec_add_symbol("start", 0, 1);
        break;
    /* Force the COFF 16-byte alignment nibble on .text and still expect a normal image. */
    case 19:
        ret_object(machine, 1);
        g_spec.sections[0].characteristics |= 0x00500000u;
        break;
    /* Another one-byte return so kind 20 is a distinct bit in the valid mask. */
    case 20:
        ret_object(machine, 1);
        break;
    /* AMD64 REL32_1 at offset 1 on an E8 byte, with the GUI subsystem forced. */
    case 21:
        ret_object(IMAGE_FILE_MACHINE_AMD64, 8);
        g_spec.sections[0].bytes[0] = 0xe8;
        spec_add_reloc(0, 1, 0, IMAGE_REL_AMD64_REL32_1);
        *subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        break;
    /* A discarded .llvm.meta section beside a one-byte .text. */
    case 22:
        ret_object(machine, 1);
        spec_add_section(".llvm.meta", IMAGE_SCN_CNT_INITIALIZED_DATA, "meta", 4, 0);
        break;
    /* Odd iterations decorate an I386 _start. Even iterations stay a plain AMD64 return. */
    case 23:
        /* This odd iteration is the decorated I386 entry. The linker is asked for start. */
        if (iter & 1) {
            ret_object(IMAGE_FILE_MACHINE_I386, 1);
            strcpy(g_spec.symbols[0].name, "_start");
            *entry = "start";
        /* This even iteration is a plain AMD64 return with the default entry. */
        } else {
            ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
        }
        /* Both arms of this kind use the Windows GUI subsystem. */
        *subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        break;
    /* Any other kind number is a broken fuzzer. End the test. */
    default:
        /* This kind is not a valid object shape. */
        crash_if(1);
        break;
    }
}

/* fuzz_test_random_valid links every valid kind ten times and checks each PE.
   rng_reset makes the payload bytes repeatable. Each case is named valid kind.iter in g_case_buf.
   expect_ok enforces the file size, section count, and entry RVA that build_valid predicted.
   Binding modes 1 and 2 also run check_import_binding. Mode 3 requires ExitProcess, GetTickCount, and kernel32.dll in the image.
   Each kind sets a bit in g_valid_mask. A missing bit logs the mask and exits through test_fail_at_line.
   The function then marks COV_VALID_FUZZ and logs that all 240 cases passed. */
static void fuzz_test_random_valid(void) {
    int kind;
    int iter;
    rng_reset();
    /* Visit every valid object shape. */
    for (kind = 0; kind < VALID_KINDS; kind++) {
        /* Repeat the shape so the generator and the machine bit both move. */
        for (iter = 0; iter < VALID_ITERS; iter++) {
            LinkInfo info;
            uint16_t subsystem;
            uint16_t sections;
            uint32_t file_size;
            uint32_t entry_rva;
            const char *entry;
            int pack;
            int binding;
            size_t used = 0;
            used = write_text(g_case_buf, sizeof g_case_buf, used, "valid ");
            used = write_d(g_case_buf, sizeof g_case_buf, used, kind);
            used = write_text(g_case_buf, sizeof g_case_buf, used, ".");
            write_d(g_case_buf, sizeof g_case_buf, used, iter);
            build_valid(kind, iter, &subsystem, &pack, &entry, &file_size, &sections, &entry_rva, &binding);
            expect_ok(g_case_buf, pack, subsystem, entry, file_size, sections, entry_rva, &info);
            /* Direct and IAT imports must match the ExitProcess binding rules. */
            if (binding == 1 || binding == 2) check_import_binding(&info, binding == 2);
            /* Two named imports must both appear, along with the DLL name. */
            if (binding == 3) {
                /* ExitProcess was not written into the image. */
                crash_if(!image_has(info.image, info.image_size, "ExitProcess"));
                /* GetTickCount was not written into the image. */
                crash_if(!image_has(info.image, info.image_size, "GetTickCount"));
                /* kernel32.dll was not written into the image. */
                crash_if(!image_has(info.image, info.image_size, "kernel32.dll"));
            }
            g_valid_mask |= 1u << kind;
        }
    }
    /* A kind never ran. Log the mask and exit with this source line. */
    if (g_valid_mask != ((1u << VALID_KINDS) - 1u)) {
        test_log("valid mask ");
        test_log_uint(g_valid_mask);
        test_fail_at_line(__LINE__);
    }
    cov_set(COV_VALID_FUZZ);
    test_log("random valid objects ...... 240 cases OK\r\n");
}

/* build_invalid fills g_spec for one object the linker must reject, and stores the diagnostic snippet to expect.
   kind selects the damage. iter varies the machine or the truncated length.
   pack is set for the pack-headers failures and cleared otherwise.
   The snippet is a substring of the message fail() stores in g_error_text.
   Kinds 16 through 19 are not built here; the option fuzzer covers them. An unknown kind ends the test. */
static void build_invalid(int kind, int iter, int *pack, const char **snippet) {
    uint16_t machine = (iter & 1) ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    *pack = 0;
    g_imp_count = 0;
    /* Pick the damaged object for this invalid kind. */
    switch (kind) {
    /* Stamp an unsupported machine, 0x1000 plus the iteration, and expect that diagnostic. */
    case 0:
        ret_object(IMAGE_FILE_MACHINE_AMD64, 1);
        g_spec.corrupt = CORRUPT_MACHINE;
        g_spec.force_machine = (uint16_t)(0x1000u + (uint32_t)iter);
        *snippet = "unsupported COFF machine";
        break;
    /* Truncate the file to a short length so the parser says the input is too small. */
    case 1:
        ret_object(machine, 1);
        g_spec.corrupt = CORRUPT_TRUNCATE;
        g_spec.truncate_to = (uint32_t)(1 + (iter % 12));
        *snippet = "too small";
        break;
    /* Zero the section count and expect the no-sections diagnostic. */
    case 2:
        ret_object(machine, 1);
        g_spec.corrupt = CORRUPT_ZERO_SECTIONS;
        *snippet = "no sections";
        break;
    /* Claim an optional header so the file is rejected as a non-object. */
    case 3:
        ret_object(machine, 1);
        g_spec.corrupt = CORRUPT_OPTIONAL;
        *snippet = "optional header";
        break;
    /* Publish an enormous string-table size. */
    case 4:
        ret_object(machine, 1);
        g_spec.corrupt = CORRUPT_STRINGS;
        *snippet = "string table";
        break;
    /* Drop the symbol table and expect the missing-symbol-table diagnostic. */
    case 5:
        ret_object(machine, 1);
        g_spec.corrupt = CORRUPT_SYMBOLS;
        *snippet = "no COFF symbol table";
        break;
    /* Relocate against an undefined external named missing. */
    case 6:
        ret_object(machine, 16);
        spec_add_symbol("missing", 0, 0);
        spec_add_reloc(0, 0, 1, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_ADDR64 : IMAGE_REL_I386_DIR32);
        *snippet = "unresolved external";
        break;
    /* Put the start symbol past the one-byte section. */
    case 7:
        ret_object(machine, 1);
        g_spec.symbols[0].value = 4;
        *snippet = "beyond its section";
        break;
    /* Set a line-number count so the linker rejects the section. */
    case 8:
        ret_object(machine, 1);
        g_spec.sections[0].line_numbers = 1;
        *snippet = "line-number";
        break;
    /* Use relocation type 0x2222, which neither machine supports. */
    case 9:
        ret_object(machine, 8);
        spec_add_reloc(0, 0, 0, 0x2222);
        *snippet = "unsupported";
        break;
    /* Force a REL32 displacement that overflows a signed 32-bit field. */
    case 10:
        ret_object(IMAGE_FILE_MACHINE_AMD64, 48);
        g_spec.sections[0].bytes[0] = 0xff;
        g_spec.sections[0].bytes[1] = 0xff;
        g_spec.sections[0].bytes[2] = 0xff;
        g_spec.sections[0].bytes[3] = 0x7f;
        spec_add_symbol("target", 40, 1);
        spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_REL32);
        *snippet = "REL32";
        break;
    /* Ask ADDR32 to hold a 64-bit image-base address. */
    case 11:
        ret_object(IMAGE_FILE_MACHINE_AMD64, 16);
        spec_add_reloc(0, 0, 0, IMAGE_REL_AMD64_ADDR32);
        *snippet = "ADDR32";
        break;
    /* Turn on pack-headers with no .rdata section. */
    case 12:
        ret_object(machine, 1);
        *pack = 1;
        *snippet = "pack-headers";
        break;
    /* Pack a 400-byte .rdata that cannot fit in the header padding. */
    case 13:
        ret_object(machine, 1);
        spec_add_section(".rdata", RDATA_CHARS, NULL, 400, 0);
        *pack = 1;
        *snippet = ".rdata needs";
        break;
    /* A SECTION relocation targets .rdata that pack-headers will not emit. */
    case 14:
        ret_object(machine, 8);
        spec_add_section(".rdata", RDATA_CHARS, NULL, 16, 0);
        spec_add_symbol("label", 0, 2);
        spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_SECTION);
        *pack = 1;
        *snippet = "without an output section";
        break;
    /* A SECREL relocation targets that same packed .rdata. */
    case 15:
        ret_object(machine, 8);
        spec_add_section(".rdata", RDATA_CHARS, NULL, 16, 0);
        spec_add_symbol("label", 0, 2);
        spec_add_reloc(0, 0, 1, IMAGE_REL_AMD64_SECREL);
        *pack = 1;
        *snippet = "without an output section";
        break;
    /* A REL32 at the section end patches bytes that are not in the section. */
    case 20:
        ret_object(machine, 16);
        spec_add_reloc(0, 16, 0, machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_REL_AMD64_REL32 : IMAGE_REL_I386_REL32);
        *snippet = "extends beyond";
        break;
    /* Any other kind is not an invalid object this builder knows. */
    default:
        /* This kind has no damaged object. End the test. */
        crash_if(1);
        break;
    }
}

/* Forward declaration of parse_wide so the import fuzzer can call it before its body.
   The definition further down parses the wide command line into ParsedArgs.
   This line is only the declaration; failure reporting belongs to the definition. */
static int parse_wide(int argc, wchar_t **argv, ParsedArgs *parsed);

/* fuzz_test_random_invalid links every damaged object and requires the expected diagnostic.
   rng_reset repeats the same damage the layout oracles used. Each case is named invalid kind.iter.
   Kinds 16 through 19 do not build a COFF object; the option and import checks fill those bits later.
   Every other kind must fail the link, and g_error_text must contain the snippet from build_invalid.
   The bits for kinds 16 through 19 are cleared before return so a premature set cannot satisfy the final mask.
   A passing link, or a wrong diagnostic, ends the test through crash_if. */
static void fuzz_test_random_invalid(void) {
    int kind;
    int iter;
    rng_reset();
    /* Visit every invalid kind, including the four parser kinds. */
    for (kind = 0; kind < INVALID_KINDS; kind++) {
        /* Repeat the kind so truncation and machine damage vary. */
        for (iter = 0; iter < INVALID_ITERS; iter++) {
            size_t used = 0;
            used = write_text(g_case_buf, sizeof g_case_buf, used, "invalid ");
            used = write_d(g_case_buf, sizeof g_case_buf, used, kind);
            used = write_text(g_case_buf, sizeof g_case_buf, used, ".");
            write_d(g_case_buf, sizeof g_case_buf, used, iter);
            g_case = g_case_buf;
            /* These four kinds are parser failures. Do not link a COFF object for them. */
            if (kind >= 16 && kind <= 19) {
                /* Parser rejects these without a COFF object. */
            /* Every other kind is a real object that must fail with the predicted snippet. */
            } else {
                const char *snippet = "";
                int pack = 0;
                build_invalid(kind, iter, &pack, &snippet);
                /* A child linker must reject the object and print the snippet. */
                expect_fail(g_case_buf, pack, NULL, snippet);
            }
            g_invalid_mask |= 1u << kind;
        }
    }
    /* Kinds 16-19 are counted after the parser loop below fills their messages.
       Mark them here only once their dedicated checks have run. The mask bits
       for 16-19 are set in fuzz_test_imports_and_options via note_invalid. */
    g_invalid_mask &= ~((1u << 16) | (1u << 17) | (1u << 18) | (1u << 19));
}

/* note_invalid sets one bit in g_invalid_mask for a parser failure that never built a COFF object.
   fuzz_test_imports_and_options calls it for kinds 16 through 19 after those checks have actually run.
   The bit stays set for the final mask comparison. There is no failure path in this helper. */
static void note_invalid(int kind) {
    g_invalid_mask |= 1u << kind;
}

/* import_must requires add_import to accept spec and to record expect_symbols names on one DLL.
   It resets the arena, clears the linker, and calls add_import.
   fail() prints the error and exits, so a rejected specification ends the test immediately.
   A symbol-count mismatch also ends the test through crash_if. Success returns normally. */
static void import_must(const char *spec, int expect_symbols) {
    Linker linker;
    arena_reset();
    memset(&linker, 0, sizeof linker);
    g_error_text[0] = 0;
    /* Call add_import. A rejected specification exits the process. */
    add_import(&linker, spec);
    /* The specification must create exactly one DLL record. */
    crash_if(linker.import_dll_count != 1);
    /* The DLL must hold the expected number of symbols. */
    crash_if(linker.import_dlls[0].symbol_count != expect_symbols);
}

/* import_must_fail requires the CLI to reject first, and second when second is not NULL.
   It builds /import arguments and runs a child linker, which prints the diagnostic and exits.
   crash_if ends the test if the child does not exit 1 or if the diagnostic misses snippet.
   The unused object name is never opened, because parsing the bad import exits first.
   Success here means the bad specification was diagnosed. */
static void import_must_fail(const char *first, const char *second, const char *snippet) {
    char first_arg[160];
    char second_arg[160];
    char *args[3];
    int count = 0;
    size_t used = 0;
    used = write_text(first_arg, sizeof first_arg, used, "/import:");
    write_text(first_arg, sizeof first_arg, used, first);
    args[count++] = first_arg;
    /* A second specification is optional. Pass it only when the caller provided one. */
    if (second != NULL) {
        used = 0;
        used = write_text(second_arg, sizeof second_arg, used, "/import:");
        write_text(second_arg, sizeof second_arg, used, second);
        args[count++] = second_arg;
    }
    args[count++] = "unused.obj";
    expect_cli_fail(snippet, count, args);
}

/* parse_ascii feeds narrow test arguments through the wide command-line parser.
   It converts each argument with ascii_to_wide, points ParsedArgs at the caller-supplied Linker, and calls parse_wide.
   fail() prints the error and exits, so this helper is only used for arguments that parse.
   A clean parse returns 0, 1, or 2 from parse_wide.
   crash_if ends the test if argc is above the eight-slot wide_argv array. The arena is reset first. */
static int parse_ascii(int argc, char **argv, ParsedArgs *parsed, Linker *linker) {
    wchar_t *wide_argv[8];
    int index;
    int result;
    /* The fixed wide argv array cannot hold this many test arguments. */
    crash_if(argc > 8);
    arena_reset();
    memset(linker, 0, sizeof *linker);
    memset(parsed, 0, sizeof *parsed);
    parsed->linker = linker;
    /* Widen every narrow argument before parse_wide sees it. */
    for (index = 0; index < argc; index++) wide_argv[index] = ascii_to_wide(argv[index]);
    g_error_text[0] = 0;
    result = parse_wide(argc, wide_argv, parsed);
    return result;
}

/* fuzz_test_imports_and_options checks import parsing and the command-line switches the linker accepts.
   It case-folds two kernel32 imports into one DLL, rejects duplicates, bad forms, and empty names, and records those invalid kinds.
   Help, usage, and accepted option mixes go through parse_ascii. Rejected options run a child linker, which prints the diagnostic and exits.
   Eighty valid and eighty invalid import strings follow, then forty junk options and forty accepted option combinations.
   The invalid-kind mask must be full at the end or the test logs it and exits with the source line.
   Coverage ids are set as each group passes, and the function logs both summary lines. */
static void fuzz_test_imports_and_options(void) {
    Linker linker;
    ParsedArgs parsed;
    int result;
    int index;
    uint32_t bad_mask = 0;
    char spec[80];
    char option[32];
    rng_reset();
    g_case = "import casefold";
    arena_reset();
    memset(&linker, 0, sizeof linker);
    g_error_text[0] = 0;
    /* Add ExitProcess and Sleep. A rejected specification exits the process. */
    add_import(&linker, "kernel32.dll,ExitProcess");
    add_import(&linker, "Kernel32.DLL,Sleep");
    /* Kernel32.DLL must fold onto the kernel32.dll already recorded. */
    crash_if(linker.import_dll_count != 1);
    crash_if(strcmp(linker.import_dlls[0].name, "kernel32.dll") != 0);
    /* Both ExitProcess and Sleep must be recorded. */
    crash_if(linker.import_dlls[0].symbol_count != 2);
    /* The first symbol must keep the ExitProcess spelling. */
    crash_if(strcmp(linker.import_dlls[0].symbols[0].name, "ExitProcess") != 0);
    /* The second symbol must be Sleep. */
    crash_if(strcmp(linker.import_dlls[0].symbols[1].name, "Sleep") != 0);
    cov_set(COV_CASEFOLD);

    g_case = "duplicate import";
    import_must_fail("kernel32.dll,ExitProcess", "other.dll,ExitProcess", "more than once");
    cov_set(COV_DUP_IMPORT);
    note_invalid(17);
    g_case = "import form";
    import_must_fail("kernel32.dll", NULL, "must have the form");
    cov_set(COV_IMPORT_FORM);
    note_invalid(18);
    g_case = "empty import";
    import_must_fail("kernel32.dll,ExitProcess,", NULL, "empty symbol");
    import_must_fail("kernel32.dll,,Sleep", NULL, "empty symbol");
    cov_set(COV_EMPTY_IMPORT);
    note_invalid(16);

    g_case = "help";
    {
        char *argv[] = {"small_linker", "file.obj", "--help"};
        result = parse_ascii(3, argv, &parsed, &linker);
        crash_if(result != 2);
    }
    {
        char *argv[] = {"small_linker", "/?"};
        result = parse_ascii(2, argv, &parsed, &linker);
        crash_if(result != 2);
    }
    {
        char *argv[] = {"small_linker", "-?"};
        result = parse_ascii(2, argv, &parsed, &linker);
        crash_if(result != 2);
    }
    cov_set(COV_HELP);
    g_case = "usage";
    {
        char *argv[] = {"small_linker", "/subsystem:console"};
        result = parse_ascii(2, argv, &parsed, &linker);
        crash_if(result != 1);
    }
    cov_set(COV_USAGE);
    g_case = "one input";
    {
        char *args[] = {"a.obj", "b.obj"};
        /* Two input paths must be rejected before a file is opened. */
        expect_cli_fail("only one input", 2, args);
    }
    cov_set(COV_ONE_INPUT);
    note_invalid(19);
    g_case = "two token";
    {
        char *argv[] = {"small_linker", "/entry", "start", "/subsystem", "console", "/out:prog.exe", "in.obj"};
        result = parse_ascii(7, argv, &parsed, &linker);
        crash_if(result != 0);
        crash_if(!wide_eq_ascii(parsed.input_path, "in.obj"));
        crash_if(!wide_eq_ascii(parsed.output_path, "prog.exe"));
        crash_if(parsed.entry == NULL || strcmp(parsed.entry, "start") != 0);
        crash_if(linker.subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI);
        crash_if(linker.pack_headers);
    }
    {
        char *args[] = {"/entry"};
        /* /entry with no symbol name must be rejected. */
        expect_cli_fail("/entry requires", 1, args);
    }
    cov_set(COV_TWO_TOKEN);
    g_case = "out prefix";
    {
        char *argv[] = {"small_linker", "-out:other.exe", "/pack-headers", "/subsystem:windows", "a.obj"};
        result = parse_ascii(5, argv, &parsed, &linker);
        crash_if(result != 0);
        crash_if(!wide_eq_ascii(parsed.output_path, "other.exe"));
        crash_if(!linker.pack_headers);
        crash_if(linker.subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI);
    }
    {
        char *args[] = {"/out:", "a.obj"};
        /* An empty /out path must be rejected. */
        expect_cli_fail("/out requires", 2, args);
    }
    cov_set(COV_OUT_PREFIX);
    g_case = "unknown option";
    {
        char *args[] = {"/nope", "a.obj"};
        /* An unrecognized switch must be rejected. */
        expect_cli_fail("unknown option", 2, args);
    }
    cov_set(COV_UNKNOWN);

    /* Accept eighty import specifications, alternating one symbol and two. */
    for (index = 0; index < 80; index++) {
        g_case = "valid import fuzz";
        /* Even indexes are one DLL and one symbol. */
        if ((index & 1) == 0) {
            size_t used = 0;
            used = write_text(spec, sizeof spec, used, "dll");
            used = write_u(spec, sizeof spec, used, (unsigned)index);
            used = write_text(spec, sizeof spec, used, ".dll,Sym");
            write_u(spec, sizeof spec, used, (unsigned)index);
            import_must(spec, 1);
        /* Odd indexes are one DLL and two symbols. */
        } else {
            size_t used = 0;
            used = write_text(spec, sizeof spec, used, "dll");
            used = write_u(spec, sizeof spec, used, (unsigned)index);
            used = write_text(spec, sizeof spec, used, ".dll,A");
            used = write_u(spec, sizeof spec, used, (unsigned)index);
            used = write_text(spec, sizeof spec, used, ",B");
            write_u(spec, sizeof spec, used, (unsigned)index + 1u);
            import_must(spec, 2);
        }
    }
    cov_set(COV_IMPORT_OK);
    /* Reject eighty bad import strings, five shapes repeating. */
    for (index = 0; index < 80; index++) {
        int mode = index % 5;
        const char *snippet = "must have the form";
        g_case = "invalid import fuzz";
        size_t used = 0;
        used = write_text(spec, sizeof spec, used, "bad");
        write_u(spec, sizeof spec, used, (unsigned)index);
        /* A missing DLL name must be rejected as a bad form. */
        if (mode == 1) {
            used = 0;
            used = write_text(spec, sizeof spec, used, ",Sym");
            write_u(spec, sizeof spec, used, (unsigned)index);
            snippet = "must have the form";
        /* A trailing comma must be rejected as an empty symbol. */
        } else if (mode == 2) {
            used = 0;
            used = write_text(spec, sizeof spec, used, "d");
            used = write_u(spec, sizeof spec, used, (unsigned)index);
            used = write_text(spec, sizeof spec, used, ".dll,Sym");
            used = write_u(spec, sizeof spec, used, (unsigned)index);
            write_text(spec, sizeof spec, used, ",");
            snippet = "empty symbol";
        /* An empty symbol between commas must be rejected. */
        } else if (mode == 3) {
            used = 0;
            used = write_text(spec, sizeof spec, used, "d");
            used = write_u(spec, sizeof spec, used, (unsigned)index);
            used = write_text(spec, sizeof spec, used, ".dll,,Sym");
            write_u(spec, sizeof spec, used, (unsigned)index);
            snippet = "empty symbol";
        /* The same symbol twice in one specification must be rejected. */
        } else if (mode == 4) {
            used = 0;
            used = write_text(spec, sizeof spec, used, "d");
            used = write_u(spec, sizeof spec, used, (unsigned)index);
            write_text(spec, sizeof spec, used, ".dll,Same,Same");
            snippet = "more than once";
        }
        import_must_fail(spec, NULL, snippet);
        bad_mask |= 1u << mode;
    }
    /* One of the five bad-import shapes never ran. */
    crash_if(bad_mask != 0x1fu);
    cov_set(COV_IMPORT_BAD);

    /* Forty unknown /fuzz and -fuzz options must be rejected. */
    for (index = 0; index < 40; index++) {
        char *args[2];
        g_case = "junk option";
        option[0] = (char)((index & 1) ? '/' : '-');
        size_t used = 0;
        used = write_text(option + 1, sizeof option - 1, used, "fuzz");
        write_u(option + 1, sizeof option - 1, used, (unsigned)rng());
        args[0] = option;
        args[1] = "in.obj";
        /* Each generated switch must be rejected as an unknown option. */
        expect_cli_fail("unknown option", 2, args);
    }
    cov_set(COV_OPTION_BAD);
    /* Forty random mixes of real options must parse and keep the input path. */
    for (index = 0; index < 40; index++) {
        char *argv[6];
        int count = 0;
        g_case = "valid option";
        argv[count++] = "small_linker";
        argv[count++] = (rng() & 1u) ? "/subsystem:console" : "/subsystem:windows";
        /* Sometimes request header packing. */
        if (rng() & 1u) argv[count++] = "/pack-headers";
        /* Sometimes name the entry symbol. */
        if (rng() & 1u) argv[count++] = "/entry:start";
        /* Sometimes declare one import. */
        if (rng() & 1u) argv[count++] = "/import:kernel32.dll,ExitProcess";
        argv[count++] = "in.obj";
        result = parse_ascii(count, argv, &parsed, &linker);
        crash_if(result != 0);
        /* The input path must survive the option mix. */
        crash_if(!wide_eq_ascii(parsed.input_path, "in.obj"));
    }
    cov_set(COV_OPTION_OK);
    /* An invalid kind never ran. Log the mask and exit. */
    if (g_invalid_mask != ((1u << INVALID_KINDS) - 1u)) {
        test_log("invalid mask ");
        test_log_uint(g_invalid_mask);
        test_fail_at_line(__LINE__);
    }
    cov_set(COV_INVALID_FUZZ);
    test_log("random invalid objects .... 168 cases OK\r\n");
    test_log("imports and options ....... OK\r\n");
}

/* cov_require demands that every coverage id in g_cov was set.
   It walks the table from 0 to COV_COUNT minus one.
   A zero slot logs the id and exits through test_fail_at_line, using that source line as the process code.
   A full table returns normally. This is the last check before the self-test reports success. */
static void cov_require(void) {
    int index;
    /* Inspect every coverage slot the oracles were supposed to hit. */
    for (index = 0; index < COV_COUNT; index++) {
        /* This id was never set. Log it and exit with this source line. */
        if (!g_cov[index]) {
            test_log("missing coverage ");
            test_log_uint((uint64_t)index);
            test_fail_at_line(__LINE__);
        }
    }
}

/* open_test_log creates self_test.txt beside the running executable and stores the handle in g_testLog.
   It reads the module path, keeps the directory, and appends the leaf name.
   A missing path, an overlong path, or a failed CreateFileW calls crash_if and does not return.
   The file is truncated if it already exists. Later test_log writes append to this handle. */
static void open_test_log(void) {
    wchar_t path[400];
    DWORD length = GetModuleFileNameW(NULL, path, 360);
    DWORD index;
    DWORD slash = 0;
    int found = 0;
    const wchar_t *leaf = L"self_test.txt";
    DWORD leaf_index = 0;
    /* The module path is missing or does not fit. */
    crash_if(length == 0 || length >= 360, "cannot read the executable path");
    /* Find the directory separator so the leaf can replace the file name. */
    for (index = 0; index < length; index++) {
        /* Remember the offset just past this separator. */
        if (path[index] == L'\\' || path[index] == L'/') {
            slash = index + 1u;
            found = 1;
        }
    }
    /* No separator was present. Write the leaf at the start of the buffer. */
    if (!found) slash = 0;
    /* Copy self_test.txt into the directory slot. */
    while (leaf[leaf_index] != 0) {
        /* The combined path does not fit. */
        crash_if(slash + leaf_index >= 399, "self-test log path is too long");
        path[slash + leaf_index] = leaf[leaf_index];
        leaf_index++;
    }
    path[slash + leaf_index] = 0;
    g_testLog = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    /* The log file could not be created. */
    crash_if(g_testLog == INVALID_HANDLE_VALUE, "cannot create the self-test log");
}

/* command_line_has_self_test reports whether the raw command line contains the wide text --self-test.
   It does not tokenize. A NULL command line returns 0.
   A match returns 1 as soon as the needle is consumed. Otherwise the scan returns 0.
   mainCRTStartup uses this to choose the self-test instead of a normal link. It does not call fail. */
static int command_line_has_self_test(void) {
    const wchar_t *command = GetCommandLineW();
    const wchar_t *needle = L"--self-test";
    const wchar_t *cursor;
    /* There is no command line, so the self-test switch is absent. */
    if (command == NULL) return 0;
    /* Try the needle at every wide character of the command line. */
    for (cursor = command; *cursor != 0; cursor++) {
        const wchar_t *left = cursor;
        const wchar_t *right = needle;
        /* Advance while this position still matches --self-test. */
        while (*right != 0 && *left == *right) {
            left++;
            right++;
        }
        /* The whole needle matched. The process should run the self-test. */
        if (*right == 0) return 1;
    }
    return 0;
}

/* run_self_test is the fuzzer entry. It opens the log, resets the generator, and runs every suite.
   The suites are the layout oracles, the random valid objects, the random invalid objects, and the import and option checks.
   Successful links run in this process. A case that must be rejected is a child process, because fail() prints the diagnostic and exits.
   cov_require then rejects a coverage id that nobody set.
   On success it logs ALL TESTS PASSED, closes the log, and exits with status 0.
   It does not return. A failure inside a suite has already exited with the source line. */
static void run_self_test(void) {
    open_test_log();
    test_log("small_linker --self-test\r\n");
    test_log("=========================\r\n");
    rng_reset();
    fuzz_test_layout_oracles();
    fuzz_test_random_valid();
    fuzz_test_random_invalid();
    fuzz_test_imports_and_options();
    cov_require();
    test_log("\r\nALL TESTS PASSED\r\n");
    /* Close the log after the success line so the last bytes are written. */
    if (g_testLog != INVALID_HANDLE_VALUE) CloseHandle(g_testLog);
    ExitProcess(0);
}

/* usage writes the linker help text to standard output.
   The text names the input object, /out, /entry, /subsystem, /import, and /pack-headers.
   console_text ignores a missing console, so this function has no failure return.
   Callers exit 0 after an explicit help request and exit 1 when the input path was missing. */
static void usage(void) {
    console_text(STD_OUTPUT_HANDLE,
                 "Usage: small_linker input.obj [/out:program.exe] [/entry:symbol]\n"
                 "                    [/subsystem:windows|console]\n"
                 "                    [/import:dll,symbol[,symbol...]] [...]\n"
                 "                    [/pack-headers]\n"
                 "Links one freestanding I386 or AMD64 COFF object into a PE executable.\n"
                 "The Windows GUI subsystem is used unless /subsystem:console is specified.\n"
                 "/import declares named DLL functions and may be repeated.\n"
                 "/pack-headers stores a small read-only .rdata section in header padding.\n");
}

/* parse_subsystem maps the words windows and console onto the PE subsystem constants.
   The compare is case-insensitive. windows returns IMAGE_SUBSYSTEM_WINDOWS_GUI and console returns IMAGE_SUBSYSTEM_WINDOWS_CUI.
   Any other text calls fail with the subsystem diagnostic and does not return.
   The return 0 after fail is unreachable and only satisfies the compiler. */
static uint16_t parse_subsystem(const char *value) {
    /* windows selects the Windows GUI subsystem. */
    if (_stricmp(value, "windows") == 0) return IMAGE_SUBSYSTEM_WINDOWS_GUI;
    /* console selects the console subsystem. */
    if (_stricmp(value, "console") == 0) return IMAGE_SUBSYSTEM_WINDOWS_CUI;
    fail("/subsystem must be 'windows' or 'console'");
    return 0;
}

/* split_command_line splits a wide Windows command line into argv tokens.
   It honors quotes and the usual backslash escapes: an even run of slashes before a quote toggles quoting, and an odd run emits a literal quote.
   A doubled quote inside quotes emits one quote. Spaces and tabs separate tokens only outside quotes.
   A NULL command line or more than max_args tokens calls fail and does not return.
   The tokens are arena-allocated. The function returns the count and writes the pointers into argv. */
static int split_command_line(const wchar_t *command, wchar_t **argv, int max_args) {
    const wchar_t *cursor = command;
    int argc = 0;
    /* There is no command line to split. fail() prints the error and exits. */
    crash_if(command == NULL, "missing command line");
    /* Take another token until the command line is consumed. */
    while (*cursor != 0) {
        wchar_t *out;
        int used = 0;
        int in_quote = 0;
        int capacity = 0;
        const wchar_t *scan;
        /* Skip spaces and tabs before the next token. */
        while (*cursor == L' ' || *cursor == L'\t') cursor++;
        /* The line ended on whitespace. Stop without an empty token. */
        if (*cursor == 0) break;
        /* The caller has no slot for another token. fail() prints the error and exits. */
        crash_if(argc >= max_args, "too many arguments");
        /* Count the remaining characters so the token buffer is large enough. */
        for (scan = cursor; *scan != 0; scan++) capacity++;
        out = (wchar_t *)allocate(((size_t)capacity + 1u) * sizeof(wchar_t));
        /* Copy this token, applying quote and backslash rules. */
        while (*cursor != 0) {
            int slashes = 0;
            int index;
            /* Count a run of backslashes. They may escape the next quote. */
            while (*cursor == L'\\') {
                slashes++;
                cursor++;
            }
            /* A quote ends the slash run. Emit half of them and decide whether the quote is literal. */
            if (*cursor == L'"') {
                /* An even number of slashes before a quote emits that many literal slashes. */
                for (index = 0; index < slashes / 2; index++) out[used++] = L'\\';
                /* An odd slash escapes the quote, so store a quote and consume it. */
                if ((slashes % 2) == 1) {
                    out[used++] = L'"';
                    cursor++;
                /* A doubled quote inside quotes stores one quote and skips both. */
                } else if (in_quote && cursor[1] == L'"') {
                    out[used++] = L'"';
                    cursor += 2;
                /* An unescaped quote toggles the in-quote state and is not stored. */
                } else {
                    in_quote = !in_quote;
                    cursor++;
                }
            /* This character is not a quote. The slashes are ordinary text. */
            } else {
                /* Store every backslash in the run. */
                for (index = 0; index < slashes; index++) out[used++] = L'\\';
                /* The slash run ended the command line. Finish the token. */
                if (*cursor == 0) break;
                /* Outside quotes, a space or tab ends the token. */
                if (!in_quote && (*cursor == L' ' || *cursor == L'\t')) break;
                out[used++] = *cursor++;
            }
        }
        out[used] = 0;
        argv[argc++] = out;
        /* Skip the separator before the next token. */
        while (*cursor == L' ' || *cursor == L'\t') cursor++;
    }
    return argc;
}

/* parse_wide reads the wide argv and fills ParsedArgs and the linker flags.
   The default subsystem is Windows GUI, pack-headers is off, the output name is a.exe, and no entry or input is set yet.
   Help switches return 2. A missing input path returns 1. A usable command returns 0.
   Empty /out or /entry values, an unknown switch, and a second input call fail and do not return.
   /import is forwarded to add_import, which also fails without returning when the specification is bad.
   The input pointer aliases argv. The entry string is a fresh narrow copy. */
static int parse_wide(int argc, wchar_t **argv, ParsedArgs *parsed) {
    int index;
    parsed->linker->subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    parsed->linker->pack_headers = false;
    parsed->output_path = L"a.exe";
    parsed->entry = NULL;
    parsed->input_path = NULL;
    /* Skip argv[0], the program name, and classify every later argument. */
    for (index = 1; index < argc; index++) {
        const wchar_t *argument = argv[index];
        const wchar_t *value;
        /* Help was requested. Tell the caller to print usage and exit 0. */
        if (wide_eq_ascii(argument, "/?") || wide_eq_ascii(argument, "-?") || wide_eq_ascii_i(argument, "--help")) {
            return 2;
        }
        /* This argument is an output path, either /out: or -out:. */
        if (wide_prefix(argument, "/out:", &value) || wide_prefix(argument, "-out:", &value)) {
            /* The output prefix is empty. fail() prints the error and exits. */
            crash_if(*value == 0, "/out requires a path");
            parsed->output_path = value;
        /* This argument is a joined entry name, /entry: or -entry:. */
        } else if (wide_prefix(argument, "/entry:", &value) || wide_prefix(argument, "-entry:", &value)) {
            /* The entry prefix is empty. fail() prints the error and exits. */
            crash_if(*value == 0, "/entry requires a symbol name");
            parsed->entry = wide_to_ascii(value);
        /* The entry name is a separate following token. */
        } else if (wide_eq_ascii_i(argument, "/entry") || wide_eq_ascii_i(argument, "-entry")) {
            /* No token follows /entry. fail() prints the error and exits. */
            crash_if(++index == argc, "/entry requires a symbol name");
            parsed->entry = wide_to_ascii(argv[index]);
        /* This argument declares DLL imports. add_import fails itself when the text is bad. */
        } else if (wide_prefix(argument, "/import:", &value) || wide_prefix(argument, "-import:", &value)) {
            add_import(parsed->linker, wide_to_ascii(value));
        /* This argument is a joined subsystem name. */
        } else if (wide_prefix(argument, "/subsystem:", &value) || wide_prefix(argument, "-subsystem:", &value)) {
            parsed->linker->subsystem = parse_subsystem(wide_to_ascii(value));
        /* The subsystem name is a separate following token. */
        } else if (wide_eq_ascii_i(argument, "/subsystem") || wide_eq_ascii_i(argument, "-subsystem")) {
            /* No token follows /subsystem. fail() prints the error and exits. */
            crash_if(++index == argc, "/subsystem requires 'windows' or 'console'");
            parsed->linker->subsystem = parse_subsystem(wide_to_ascii(argv[index]));
        /* Turn on header packing for this link. */
        } else if (wide_eq_ascii_i(argument, "/pack-headers") || wide_eq_ascii_i(argument, "-pack-headers")) {
            parsed->linker->pack_headers = true;
        /* Any other switch is not recognized. fail() prints the error and exits. */
        } else if (argument[0] == L'/' || argument[0] == L'-') {
            size_t used = 0;
            used = write_text(g_error_text, sizeof g_error_text, used, "unknown option '");
            used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(argument));
            write_text(g_error_text, sizeof g_error_text, used, "'");
            fail(g_error_text);
        /* The first bare argument is the input object path. */
        } else if (parsed->input_path == NULL) {
            parsed->input_path = argument;
        /* A second bare argument is not allowed. fail() prints the error and exits. */
        } else {
            fail("only one input object is supported");
        }
    }
    /* Nothing named an input. Tell the caller to print usage and exit 1. */
    if (parsed->input_path == NULL) return 1;
    return 0;
}

/* read_file_w reads an entire file into an arena buffer and stores the length in size.
   It opens the wide path for reading, rejects a size it cannot measure, and rejects anything larger than 64 MiB.
   A failed open, a failed read, or a failed close calls fail and does not return. The handle is closed before those later failures.
   An empty file returns a zero-length allocation and sets size to 0.
   The bytes stay valid until the next arena_reset. */
static uint8_t *read_file_w(const wchar_t *path, size_t *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER length;
    uint8_t *data;
    DWORD got = 0;
    DWORD error;
    /* The path could not be opened. fail() prints the error and exits. */
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot open '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        used = write_text(g_error_text, sizeof g_error_text, used, "' (error ");
        used = write_u(g_error_text, sizeof g_error_text, used, error);
        write_text(g_error_text, sizeof g_error_text, used, ")");
        fail(g_error_text);
    }
    /* The size is unavailable. Close the handle, then fail() without returning. */
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) {
        CloseHandle(file);
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot determine the size of '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        write_text(g_error_text, sizeof g_error_text, used, "'");
        fail(g_error_text);
    }
    /* The input is above 64 MiB. Close the handle, then fail() without returning. */
    if ((unsigned long long)length.QuadPart > 64ull * 1024ull * 1024ull) {
        CloseHandle(file);
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "input '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        write_text(g_error_text, sizeof g_error_text, used, "' is too large");
        fail(g_error_text);
    }
    data = (uint8_t *)allocate((size_t)length.QuadPart);
    /* A nonempty file must be read in full. A short read closes the handle and calls fail(). */
    if (length.QuadPart != 0 &&
        (!ReadFile(file, data, (DWORD)length.QuadPart, &got, NULL) || got != (DWORD)length.QuadPart)) {
        CloseHandle(file);
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot read '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        write_text(g_error_text, sizeof g_error_text, used, "'");
        fail(g_error_text);
    }
    /* The bytes were read but the handle did not close. fail() prints the error and exits. */
    if (!CloseHandle(file)) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot close '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        write_text(g_error_text, sizeof g_error_text, used, "' after reading");
        fail(g_error_text);
    }
    *size = (size_t)length.QuadPart;
    return data;
}

/* write_file_w creates or truncates a wide path and writes size bytes from data.
   A failed create, a short write, or a failed close calls fail and does not return.
   The handle is closed before a write failure is reported.
   A zero size creates an empty file and skips WriteFile.
   The CLI uses this for the finished PE. The self-test does not. */
static void write_file_w(const wchar_t *path, const uint8_t *data, size_t size) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    DWORD error;
    /* The output path could not be created. fail() prints the error and exits. */
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot create '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        used = write_text(g_error_text, sizeof g_error_text, used, "' (error ");
        used = write_u(g_error_text, sizeof g_error_text, used, error);
        write_text(g_error_text, sizeof g_error_text, used, ")");
        fail(g_error_text);
    }
    /* A nonempty image must be written in full. A short write closes the handle and calls fail(). */
    if (size != 0 && (!WriteFile(file, data, (DWORD)size, &written, NULL) || written != (DWORD)size)) {
        CloseHandle(file);
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot write '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        write_text(g_error_text, sizeof g_error_text, used, "'");
        fail(g_error_text);
    }
    /* The bytes were written but the handle did not close. fail() prints the error and exits. */
    if (!CloseHandle(file)) {
        size_t used = 0;
        used = write_text(g_error_text, sizeof g_error_text, used, "cannot close '");
        used = write_text(g_error_text, sizeof g_error_text, used, wide_to_ascii(path));
        write_text(g_error_text, sizeof g_error_text, used, "' after writing");
        fail(g_error_text);
    }
}

/* cli_main is the normal process path: parse the command line, link one object, and write the PE.
   Help exits 0 after usage. A missing input exits 1 after usage. Both of those returns come from parse_wide.
   A successful link writes the image, prints the path, size, section count, bitness, subsystem, and entry, then exits 0.
   fail() prints the diagnostic and exits 1, so a rejected link does not reach the success message.
   The function does not return to mainCRTStartup on any path. */
static void cli_main(void) {
    wchar_t *argv[64];
    ParsedArgs parsed;
    Linker linker;
    LinkInfo info;
    int argc;
    int disposition;
    char message[1200];
    memset(&linker, 0, sizeof linker);
    memset(&parsed, 0, sizeof parsed);
    parsed.linker = &linker;
    argc = split_command_line(GetCommandLineW(), argv, 64);
    disposition = parse_wide(argc, argv, &parsed);
    /* Help was requested. Print usage and exit 0. */
    if (disposition == 2) {
        usage();
        ExitProcess(0);
    }
    /* No input path was given. Print usage and exit 1. */
    if (disposition == 1) {
        usage();
        ExitProcess(1);
    }
    linker.file_data = read_file_w(parsed.input_path, &linker.file_size);
    parse_object(&linker);
    prepare_imports(&linker);
    assign_layout(&linker);
    copy_section_data(&linker);
    build_import_data(&linker);
    select_entry(&linker, parsed.entry);
    apply_relocations(&linker);
    info.image = build_image(&linker, &info.image_size);
    info.emitted_section_count = linker.emitted_section_count;
    info.machine = linker.machine;
    info.subsystem = linker.subsystem;
    info.entry_name = linker.entry_name;
    write_file_w(parsed.output_path, info.image, info.image_size);
    size_t used = 0;
    used = write_text(message, sizeof message, used, "small_linker: wrote ");
    used = write_text(message, sizeof message, used, wide_to_ascii(parsed.output_path));
    used = write_text(message, sizeof message, used, " (");
    used = write_u(message, sizeof message, used, info.image_size);
    used = write_text(message, sizeof message, used, " bytes, ");
    used = write_u(message, sizeof message, used, (unsigned)info.emitted_section_count);
    used = write_text(message, sizeof message, used, " sections, ");
    used = write_text(message, sizeof message, used, linker_is_64_bit(&linker) ? "64-bit" : "32-bit");
    used = write_text(message, sizeof message, used, ", ");
    used = write_text(message, sizeof message, used, linker.subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI ? "windows" : "console");
    used = write_text(message, sizeof message, used, ", entry ");
    used = write_text(message, sizeof message, used, info.entry_name);
    write_text(message, sizeof message, used, "\n");
    console_text(STD_OUTPUT_HANDLE, message);
    ExitProcess(0);
}

/* How far commit_stack_pages walks. 180 chunks of 2500 bytes is about half of
   the default 1 MiB reserve: enough for the locals in this file, and inside
   the reserve so the probe never steps off the stack. */
#define STACK_PROBE_STEPS 180
/* Each step is smaller than one 4096-byte page, so the touch lands on the
   guard page instead of past it. */
#define STACK_PROBE_CHUNK 2500

/* commit_stack_pages touches the stack one chunk at a time so each touch lands on the guard page.
   It reads stepsLeft and writes two volatile bytes in a local array of STACK_PROBE_CHUNK bytes, then calls itself until one step remains.
   This build links /NODEFAULTLIB and compiles with /Gs262144, so there is no __chkstk and the compiler does not probe a frame smaller than 256 KiB.
   Windows starts the thread with one committed page and a guard page below it. expect_cli_fail's wchar_t command[4096] is 8192 bytes and is allocated in one step, which can skip that guard.
   The pages stay committed after this returns. The function does not call fail() and does not return early. */
static void commit_stack_pages(int stepsLeft) {
    volatile unsigned char page[STACK_PROBE_CHUNK];
    /* Touch the low end of this frame so the guard page commits before the next step. */
    page[0] = (unsigned char)stepsLeft;
    /* Walk one chunk farther. The call keeps this frame on the stack. */
    if (stepsLeft > 1) commit_stack_pages(stepsLeft - 1);
    /* Touch the high end after the call so the compiler cannot drop the array or turn the recursion into a loop that never walks downward. */
    page[STACK_PROBE_CHUNK - 1] = page[0];
}

/* mainCRTStartup is the process entry. It commits stack pages, prepares the arena, and then either runs the self-test or links one object.
   commit_stack_pages runs first, while this frame is still small, because a later local can be larger than the guard gap and this build has no __chkstk.
   command_line_has_self_test selects run_self_test, which exits and does not return.
   Otherwise cli_main parses argv, writes the PE, and exits.
   Neither path returns. fail() prints the diagnostic and exits before either function returns. */
void mainCRTStartup(void) {
    /* Commit the stack while this frame is only the call itself. */
    commit_stack_pages(STACK_PROBE_STEPS);
    arena_init();
    /* The command line contains --self-test. Run the fuzzer, which exits. */
    if (command_line_has_self_test()) run_self_test();
    cli_main();
}
