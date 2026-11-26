#ifndef FPGAGPU_TEST_DMA_CONCURRENT_H
#define FPGAGPU_TEST_DMA_CONCURRENT_H

/* Returns nonzero on any failure. Only for an idle, recoverable HIL host. */
int test_dma_concurrent_all(unsigned rounds);

#endif
