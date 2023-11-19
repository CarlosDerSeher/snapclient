/*
 * MedianFilter.c
 *
 *  Created on: May 19, 2018
 *      Author: alexandru.bogdan
 *      Editor: Carlos Derseher
 *
 *      original source code:
 *      https://github.com/accabog/MedianFilter
 */

/**
 * This Module expects odd numbers of buffer lengths!!!
 */

#include <stdint.h>
#include "MedianFilter.h"
/**
 *
 */
int MEDIANFILTER_Init(sMedianFilter_t *medianFilter) {
  if (medianFilter && medianFilter->medianBuffer &&
      (medianFilter->numNodes % 2) && (medianFilter->numNodes > 1)) {
    // initialize buffer nodes
    for (unsigned int i = 0; i < medianFilter->numNodes; i++) {
      medianFilter->medianBuffer[i].value = INT64_MAX;
      medianFilter->medianBuffer[i].nextAge =
          &medianFilter->medianBuffer[(i + 1) % medianFilter->numNodes];
      medianFilter->medianBuffer[i].nextValue =
          &medianFilter->medianBuffer[(i + 1) % medianFilter->numNodes];
      medianFilter->medianBuffer[(i + 1) % medianFilter->numNodes].prevValue =
          &medianFilter->medianBuffer[i];
    }
    // initialize heads
    medianFilter->ageHead = medianFilter->medianBuffer;
    medianFilter->valueHead = medianFilter->medianBuffer;
    medianFilter->medianHead = medianFilter->medianBuffer;

    medianFilter->bufferCnt = 0;

    return 0;
  }

  return -1;
}

/**
 *
 */
int64_t MEDIANFILTER_Insert(sMedianFilter_t *medianFilter, int64_t sample) {
  unsigned int i;
  sMedianNode_t *newNode, *it;
  if (medianFilter->bufferCnt < medianFilter->numNodes) {
    medianFilter->bufferCnt++;
  }


  // if oldest node is also the smallest node,
  // increment value head
  if (medianFilter->ageHead == medianFilter->valueHead) {
    medianFilter->valueHead = medianFilter->valueHead->nextValue;
  }

  if (((medianFilter->ageHead == medianFilter->medianHead) ||
      (medianFilter->ageHead->value > medianFilter->medianHead->value)) &&
      (medianFilter->bufferCnt >= medianFilter->numNodes)) {
    // prepare for median correction
    medianFilter->medianHead = medianFilter->medianHead->prevValue;
  }

  // replace age head with new sample
  newNode = medianFilter->ageHead;
  newNode->value = sample;

  // remove age head from list
  medianFilter->ageHead->nextValue->prevValue =
      medianFilter->ageHead->prevValue;
  medianFilter->ageHead->prevValue->nextValue =
      medianFilter->ageHead->nextValue;
  // increment age head
  medianFilter->ageHead = medianFilter->ageHead->nextAge;

  // find new node position
  it = medianFilter->valueHead;  // set iterator as value head
  for (i = 0; i < medianFilter->bufferCnt - 1; i++) {
    if (sample < it->value) {
      break;
    }
    it = it->nextValue;
  }
  if (i == 0) {  // replace value head if new node is the smallest
    medianFilter->valueHead = newNode;
  }

  // insert new node in list
  it->prevValue->nextValue = newNode;
  newNode->prevValue = it->prevValue;
  it->prevValue = newNode;
  newNode->nextValue = it;

  // adjust median node
  if ((medianFilter->bufferCnt < medianFilter->numNodes)){
      if (medianFilter->bufferCnt % 2 != 0 && medianFilter->bufferCnt != 1) {
          medianFilter->medianHead = medianFilter->medianHead->prevValue;
      }
      if (((i > (medianFilter->bufferCnt / 2)) && (medianFilter->bufferCnt % 2 != 0)) ||
        ((i >= (medianFilter->bufferCnt / 2)) && (medianFilter->bufferCnt % 2 == 0))) {
        medianFilter->medianHead = medianFilter->medianHead->nextValue;
      }
  }
  else if (i >= (medianFilter->bufferCnt / 2) ) {
    medianFilter->medianHead = medianFilter->medianHead->nextValue;
  }

  return medianFilter->medianHead->value;
}

/**
 *
 */
int64_t MEDIANFILTER_get_median(sMedianFilter_t *medianFilter, uint32_t n) {
  int64_t avgMedian = 0;
  sMedianNode_t *it;
  int32_t i;
  if (n >= medianFilter->bufferCnt) {
      n = (((medianFilter->bufferCnt-1)<<1)>>1);
  }
 
  // n should not include the center value
  if ((n % 2) != 0) {
      n--;
  }

  it = medianFilter->medianHead->prevValue;  // set iterator as value head previous
  // first add previous values
  for (i = 0; i < n / 2; i++) {
    avgMedian += it->value;
    it = it->prevValue;
  }

  it = medianFilter->medianHead->nextValue;  // set iterator as value head next
  // second add next values
  for (i = 0; i < n / 2; i++) {
    avgMedian += it->value;
    it = it->nextValue;
  }

  avgMedian += medianFilter->medianHead->value;
  avgMedian /= (n + 1);

  return avgMedian;
}

/**
 *
 */
uint32_t MEDIANFILTER_isFull(sMedianFilter_t *medianFilter, uint32_t n) {
  if (n < 1 || n > medianFilter->numNodes) {
      n = medianFilter->numNodes;
  }
  if (medianFilter->bufferCnt >= n) {
    return 1;
  } else {
    return 0;
  }
}
