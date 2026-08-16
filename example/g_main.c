#include "bg_lib.h"

void trap_PrintInt(int n);
void trap_Printf(const char* text);

void printf(const char* fmt, ...);
int fib(int n);

/*
================
vmMain

This is the only way control passes into the module.
This must be the very first function compiled into the .qvm file
================
*/
int vmMain(int command, int arg0, int arg1, int arg2, int arg3, int arg4,
           int arg5, int arg6, int arg7, int arg8, int arg9, int arg10,
           int arg11)
{
    char buf[64];
    int  i;

    if (command == 0)
    {
        printf("Hello World! - fib(5) = %i\n", fib(5));

        /* exercise a few more bg_lib routines */
        strcpy(buf, "q3vm");
        strcat(buf, " says hi");
        printf("%s (len=%i)\n", buf, strlen(buf));
        printf("atoi(\"-123\") = %i\n", atoi("-123"));
        printf("abs(-42) = %i\n", abs(-42));

        memset(buf, 'x', 8);
        buf[8] = 0;
        printf("memset: %s\n", buf);
        memcpy(buf, "copied!", 8);
        printf("memcpy: %s\n", buf);

        for (i = 1; i <= 10; i++)
        {
            trap_PrintInt(fib(i));
            trap_Printf(" ");
        }
        trap_Printf("\n");
    }
    else
    {
        printf("Unknown command.\n");
    }

    return 0;
}

void printf(const char* fmt, ...)
{
    va_list argptr;
    char    text[1024];

    va_start(argptr, fmt);
    vsprintf(text, fmt, argptr);
    va_end(argptr);

    trap_Printf(text);
}

int fib(int n)
{
    if (n <= 2)
        return 1;
    else
        return fib(n-1) + fib(n-2);
}
