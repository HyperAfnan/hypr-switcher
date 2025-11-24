#pragma once
#include <stdio.h>
#include <stdlib.h>

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); exit(1); } while (0)
