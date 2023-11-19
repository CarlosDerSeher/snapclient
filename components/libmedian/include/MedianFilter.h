/*
 * MedianFilter.h
 *
 *  Created on: May 19, 2018
 *      Author: alexandru.bogdan
 *      Editor: Carlos Derseher
 *
 *      original source code:
 *      https://github.com/accabog/MedianFilter
 */

#ifndef MEDIANFILTER_H_
#define MEDIANFILTER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct sMedianNode {
  int64_t value;                  // sample value
  struct sMedianNode *nextAge;    // pointer to next oldest value
  struct sMedianNode *nextValue;  // pointer to next smallest value
  struct sMedianNode *prevValue;  // pointer to previous smallest value
} sMedianNode_t;

typedef struct {
  unsigned int numNodes;        // median node buffer length
  sMedianNode_t *medianBuffer;  // median node buffer
  sMedianNode_t *ageHead;       // pointer to oldest value
  sMedianNode_t *valueHead;     // pointer to smallest value
  sMedianNode_t *medianHead;    // pointer to median value
  unsigned int bufferCnt;
} sMedianFilter_t;

int MEDIANFILTER_Init(sMedianFilter_t *medianFilter);
int64_t MEDIANFILTER_Insert(sMedianFilter_t *medianFilter, int64_t sample);
int64_t MEDIANFILTER_get_median(sMedianFilter_t *medianFilter, uint32_t n);
uint32_t MEDIANFILTER_isFull(sMedianFilter_t *medianFilter, uint32_t n);


#ifdef __cplusplus
}
#endif

#endif  // MEDIANFILTER_H_
