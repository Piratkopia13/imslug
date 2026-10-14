// imslug - scripts/fetchcontent_smoke/main.cpp
//
// Minimal clean-project consumer: includes only the public header, links
// the core library and prints the version. Built and run by
// scripts/verify_fetchcontent.* from the throwaway FetchContent project.

#include "imslug.h"

#include <stdio.h>

int main()
{
    const char* version = ImSlug::GetVersion();
    printf("fc_smoke: imslug version: %s (num %d)\n", version != NULL ? version : "(null)", IMSLUG_VERSION_NUM);
    return version != NULL && version[0] != '\0' ? 0 : 1;
}
