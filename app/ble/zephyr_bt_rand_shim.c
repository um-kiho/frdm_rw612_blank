#include <stddef.h>

#include <zephyr/bluetooth/crypto.h>
#include <zephyr/random/random.h>

int bt_rand(void *buf, size_t len)
{
	return sys_csrand_get(buf, len);
}