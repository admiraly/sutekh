/* Independent reference: splitmix64 as in S. Vigna's public-domain splitmix64.c
   (x += 0x9e3779b97f4a7c15; z = x; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb; return z ^ (z >> 31)). */
#include <stdint.h>
#include <stdio.h>
static uint64_t x;
static uint64_t next(void) {
  uint64_t z;
  x += 0x9e3779b97f4a7c15ULL;
  z = x;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
int main(void) {
  uint64_t seeds[3] = {0ULL, 1ULL, 0x0123456789ABCDEFULL};
  for (int s = 0; s < 3; s++) {
    x = seeds[s];
    printf("seed %016llx:", (unsigned long long)seeds[s]);
    for (int i = 0; i < 4; i++) printf(" %016llx", (unsigned long long)next());
    printf("\n");
  }
  x = 0;
  uint64_t u = next();
  uint32_t u32 = (uint32_t)(u >> 32);
  printf("seed 0 first u32 %08x uniform(1024)=%llu uniform(3)=%llu uniform(17)=%llu\n", u32,
         (unsigned long long)(((uint64_t)u32 * 1024ULL) >> 32),
         (unsigned long long)(((uint64_t)u32 * 3ULL) >> 32),
         (unsigned long long)(((uint64_t)u32 * 17ULL) >> 32));
  return 0;
}
