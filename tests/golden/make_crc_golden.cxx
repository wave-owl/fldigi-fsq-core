// CRC-8 vectors from fldigi 4.2.13's own CRC8 class (src/include/crc8.h), for
// tests/test_fsq.c. Build: c++ -I/path/to/fldigi/src/include make_crc_golden.cxx
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include "crc8.h"

int main()
{
    const char *calls[] = {"dg6fl", "DG6FL", "allcall", "cqcqcq", "dg6xx", "zl1bpu", "w1hkj",
                           "a", "ab", "kh6/dl1abc", "dl1abc/p", "9a1a", "vk2xyz/mm",
                           "0123456789abcdefghij", "~!@#$%^&*()_+{}|:<>?", " ", 0};
    CRC8 crc;
    for (int i = 0; calls[i]; i++)
        printf("%s\t%s\n", crc.sval(calls[i]).c_str(), calls[i]);
    return 0;
}
