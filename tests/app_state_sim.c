/* Compile with one of IMPORT_TEST, DIRECT_IMPORT_TEST, or UNRESOLVED_TEST.
   Each translation unit is a single PE entry named start. The integration
   harness links it and checks the exit code or the linker's rejection. */

#if defined(IMPORT_TEST)

/* Kernel32 export reached through the import address table.
   dllimport makes the compiler emit an __imp_ reference instead of a direct call. */
__declspec(dllimport)
void __stdcall ExitProcess(unsigned long exit_code);

/* Entry that exits 74 through an __imp_ ExitProcess binding.
   The linker must resolve the import and the running image must reach that call. */
int start(void)
{
    /* 74 identifies the dllimport case on both x64 and x86. */
    ExitProcess(74);
    /* ExitProcess ends the process. This return is only here so start has an int result. */
    return 0;
}

#elif defined(DIRECT_IMPORT_TEST)

/* Undecorated stdcall declaration with no dllimport attribute.
   The compiler emits a direct call. On x86 the object symbol is _ExitProcess@4,
   which small_linker maps back to the export name ExitProcess. */
void __stdcall ExitProcess(unsigned long exit_code);

/* Entry that exits 75 through a direct call, not an __imp_ load.
   This is the x86 form of the same import the MASM-style object uses on x64. */
int start(void)
{
    /* 75 distinguishes the direct stdcall object from the dllimport object. */
    ExitProcess(75);
    /* ExitProcess ends the process. This return is only here so start has an int result. */
    return 0;
}

#elif defined(UNRESOLVED_TEST)

/* Symbol that no import declaration and no object defines.
   Linking this translation unit must fail before an executable is written. */
extern int missing_function(void);

/* Entry whose only purpose is to reference the missing symbol.
   The returned value is never observed, because the link is rejected. */
int start(void)
{
    /* Keep the reference live so the compiler cannot delete the extern. */
    return missing_function();
}

#else
/* The harness always passes one of the three test macros. */
#error Define IMPORT_TEST or UNRESOLVED_TEST.
#endif
