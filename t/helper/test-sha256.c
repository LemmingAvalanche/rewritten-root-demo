#include "test-tool.h"
#include "cache.h"

int cmd__sha256(int ac, const char **av)
{
	return cmd_hash_impl(ac, av, GIT_HASH_SHA256);
}
