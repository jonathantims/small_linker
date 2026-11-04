/* Entry point linked as the PE entry by the integration tests.
   Windows uses the value returned from the entry point as the process exit code.
   42 is distinct from the generated object that returns 37 and from the import cases. */
int start(void)
{
    /* Hand the fixed code back to the loader. No DLL call is made. */
    return 42;
}
