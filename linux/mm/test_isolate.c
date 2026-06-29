#include <linux/swap.h>
int test_iso(struct page *p) { return isolate_lru_page(p); }
