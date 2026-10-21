#include <stdint.h>
uint32_t t_read32(uint64_t a); uint64_t t_read64(uint64_t a); void t_write32(uint64_t a, uint32_t v); void t_write64(uint64_t a, uint64_t v);
#define read32(a) t_read32(a)
#define read64(a) t_read64(a)
#define write32(a, v) t_write32((a), (v))
#define write64(a, v) t_write64((a), (v))
#define SYNC() do {} while (0)
