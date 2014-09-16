/* Copyright (c) 2013 Quanta Research Cambridge, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <monkit.h>
#include <semaphore.h>

#include <list>
#include <time.h>

#include "StdDmaIndication.h"
#include "DmaConfigProxy.h"
#include "GeneratedTypes.h"
#include "FlashIndicationWrapper.h"
#include "FlashRequestProxy.h"

#define TAG_COUNT 128
//#define MAX_REQS_INFLIGHT 32

#ifndef BSIM
#define DMA_BUFFER_COUNT 16
#define LARGE_NUMBER (1024*1024/8)
bool verbose = false;
#else
#define DMA_BUFFER_COUNT 8
#define LARGE_NUMBER 256
bool verbose = true;
#endif

#define BUFFER_WAYS (128/DMA_BUFFER_COUNT)
#define BUFFER_COUNT (DMA_BUFFER_COUNT*BUFFER_WAYS)

double timespec_diff_sec( timespec start, timespec end ) {
	double t = end.tv_sec - start.tv_sec;
	t += ((double)(end.tv_nsec - start.tv_nsec)/1000000000L);
	return t;
}

pthread_mutex_t flashReqMutex;
pthread_cond_t flashReqCond;

char* log_prefix = "\t\tLOG: ";

sem_t done_sem;
int srcAllocs[DMA_BUFFER_COUNT];
int dstAllocs[DMA_BUFFER_COUNT];
unsigned int ref_srcAllocs[DMA_BUFFER_COUNT];
unsigned int ref_dstAllocs[DMA_BUFFER_COUNT];
unsigned int* srcBuffers[DMA_BUFFER_COUNT];
unsigned int* dstBuffers[DMA_BUFFER_COUNT];
bool srcBufferBusy[BUFFER_COUNT];
bool readTagBusy[TAG_COUNT];

unsigned int* writeBuffers[BUFFER_COUNT];
unsigned int* readBuffers[BUFFER_COUNT];

int numBytes = (1 << (10 +4))*BUFFER_WAYS; //16KB buffer, to be safe
size_t alloc_sz = numBytes*sizeof(unsigned char);

int curWritesInFlight = 0;
int curReadsInFlight = 0;
std::list<int> finishedReadBuffer;
std::list<int> readyReadBuffer;

pthread_mutex_t freeListMutex;
pthread_mutex_t cmdReqMutex;
pthread_cond_t cmdReqCond;
void setFinishedReadBuffer(int idx) {
	pthread_mutex_lock(&freeListMutex);
	finishedReadBuffer.push_front(idx);
	pthread_mutex_unlock(&freeListMutex);
}
void flushFinishedReadBuffers(FlashRequestProxy* device) {
	pthread_mutex_lock(&freeListMutex);
	while(!finishedReadBuffer.empty()) {
		int finishedread = finishedReadBuffer.back();
		if ( verbose ) printf( "%s returning read buffer %d\n", log_prefix, finishedread );
		device->returnReadHostBuffer(finishedread);
		finishedReadBuffer.pop_back();
	}
	pthread_mutex_unlock(&freeListMutex);
}
int popReadyReadBuffer() {
	pthread_mutex_lock(&freeListMutex);
	int ret = -1;
	if ( !readyReadBuffer.empty() ) {
		ret = readyReadBuffer.back();
		readyReadBuffer.pop_back();
	}
	pthread_mutex_unlock(&freeListMutex);
	return ret;
}


timespec readstart[TAG_COUNT];
int readstartidx = 0;
int readdoneidx = 0;
timespec readnow;

int timecheckidx = 0;
float timecheck[2048];

int curCmdCountBudget = 0;
class FlashIndication : public FlashIndicationWrapper
{

public:
  FlashIndication(unsigned int id) : FlashIndicationWrapper(id){}

  virtual void writeDone(unsigned int bufidx) {
	pthread_mutex_lock(&flashReqMutex);
	curWritesInFlight --;
	srcBufferBusy[bufidx] = false;
	pthread_cond_broadcast(&flashReqCond);
	pthread_mutex_unlock(&flashReqMutex);
	
	if ( curWritesInFlight < 0 ) {
		curWritesInFlight = 0;
		fprintf(stderr, "Write requests in flight cannot be negative\n" );
	}

/*
	curReqsInFlight --;
	if ( curReqsInFlight < 0 ) {
		curReqsInFlight = 0;
		fprintf(stderr, "Requests in flight cannot be negative\n" );
	}
*/
	if ( verbose ) printf( "%s received write done buffer: %d curWritesInFlight: %d\n", log_prefix, bufidx, curWritesInFlight );

  }
  virtual void pageReadDone(unsigned int rbuf, unsigned int tag) {
	if ( verbose ) printf( "%s received read page tag: %d buffer: %d %d\n", log_prefix, tag, rbuf, curReadsInFlight );
	pthread_mutex_lock(&flashReqMutex);
	curReadsInFlight --;
	if ( tag < TAG_COUNT ) readTagBusy[tag] = false;
	else fprintf(stderr, "WARNING: done tag larger than tag count\n" );
	pthread_mutex_lock(&freeListMutex);
	readyReadBuffer.push_front(rbuf);
	pthread_mutex_unlock(&freeListMutex);

	pthread_cond_broadcast(&flashReqCond);
	pthread_mutex_unlock(&flashReqMutex);

	if ( curReadsInFlight < 0 ) {
		curReadsInFlight = 0;
		fprintf(stderr, "Read requests in flight cannot be negative\n" );
	}
	if ( verbose ) printf( "%s Finished pagedone: %d buffer: %d %d\n", log_prefix, tag, rbuf, curReadsInFlight );

	if ( timecheckidx < 2048 ) {
		clock_gettime(CLOCK_REALTIME, & readnow);
		timecheck[timecheckidx] = timespec_diff_sec(readstart[readdoneidx], readnow);
		readdoneidx++;
		if ( readdoneidx >= TAG_COUNT ) readdoneidx = 0;
		timecheckidx ++;
	}


/*
	curReqsInFlight --;
	if ( curReqsInFlight < 0 ) {
		curReqsInFlight = 0;
		fprintf(stderr, "Requests in flight cannot be negative\n" );
	}
	*/

  }

  virtual void reqFlashCmd(unsigned int inQ, unsigned int count) {
	pthread_mutex_lock(&cmdReqMutex);
	curCmdCountBudget += count;
	pthread_cond_broadcast(&cmdReqCond);
	pthread_mutex_unlock(&cmdReqMutex);

	if ( verbose ) printf( "\t%s increase flash cmd budget: %d (%d)\n", log_prefix, curCmdCountBudget, inQ );
  }
  virtual void hexDump(unsigned int data) {
  	printf( "%x--\n", data );
	fflush(stdout);
  }
};

int getNumWritesInFlight() { return curWritesInFlight; }
int getNumReadsInFlight() { return curReadsInFlight; }

void writePage(FlashRequestProxy* device, int channel, int chip, int block, int page, int bufidx) {
	if ( bufidx > BUFFER_COUNT ) return;

	if ( verbose ) printf( "%s requesting write page\n", log_prefix );
	
	//while (curReqsInFlight >= MAX_REQS_INFLIGHT ) {
	pthread_mutex_lock(&cmdReqMutex);
	while (curCmdCountBudget <= 0) {
		//if ( verbose ) printf( "%s too many reqs in flight %d\n", log_prefix, curCmdCountBudget );
		pthread_cond_wait(&cmdReqCond, &cmdReqMutex);
	}
	pthread_mutex_unlock(&cmdReqMutex);

	pthread_mutex_lock(&flashReqMutex);
	curWritesInFlight ++;
	curCmdCountBudget--; 
	pthread_mutex_unlock(&flashReqMutex);

	if ( verbose ) printf( "%s sending write req to device %d\n", log_prefix, curWritesInFlight );
	device->writePage(channel,chip,block,page,bufidx);

}

int getIdleWriteBuffer() {
	pthread_mutex_lock(&flashReqMutex);

	int ret = -1;
	for ( int i = 0; i < BUFFER_COUNT; i++ ) {
		if ( !srcBufferBusy[i] ) {
			srcBufferBusy[i] = true;
			ret = i;
			break;
		}
	}
	pthread_mutex_unlock(&flashReqMutex);
	return ret;
}

int waitIdleWriteBuffer() {
	int bufidx = -1;
	while (bufidx == -1) bufidx = getIdleWriteBuffer();

	if ( verbose ) printf( "%s idle write buffer discovered %d\n", log_prefix, bufidx );
	return bufidx;
}

unsigned int noCmdBudgetCount = 0;
unsigned int noTagCount = 0;


void readPage(FlashRequestProxy* device, int channel, int chip, int block, int page) {

	clock_gettime(CLOCK_REALTIME, & readstart[readstartidx]);
	readstartidx++;
	if ( readstartidx >= TAG_COUNT ) readstartidx = 0;

	pthread_mutex_lock(&cmdReqMutex);
	while (curCmdCountBudget <= 0) {
		noCmdBudgetCount++;
		if ( verbose ) printf( "%s no cmd budget \n", log_prefix );
		pthread_cond_wait(&cmdReqCond, &cmdReqMutex);
	}
	curCmdCountBudget --;
	pthread_mutex_unlock(&cmdReqMutex);

	int availTag = -1;
	if ( verbose ) printf( "%s budget: %d finding new tag\n", log_prefix, curCmdCountBudget );
	while (true) {
		int rrb = popReadyReadBuffer();
		while (rrb >= 0 ) {
			setFinishedReadBuffer(rrb);
			rrb = popReadyReadBuffer();
		}
		flushFinishedReadBuffers(device);

		pthread_mutex_lock(&flashReqMutex);
		availTag = -1;
		for ( int i = 0; i < TAG_COUNT; i++ ) {
			if ( readTagBusy[i] == false ) {
				availTag = i;
				curReadsInFlight ++;
				readTagBusy[availTag] = true;
				break;
			}
		}

		if ( availTag < 0 ) {
			if ( verbose ) printf( "%s no tags!\n", log_prefix );
			noTagCount++;
			pthread_cond_wait(&flashReqCond, &flashReqMutex);
			//usleep(100);
		} else {
			pthread_mutex_unlock(&flashReqMutex);
			break;
		}
	}
	
	//if ( verbose ) printf( "%s using tag %d\n", log_prefix, availTag );


	//curReqsInFlight ++;

	if ( verbose ) printf( "%s sending read page request\n", log_prefix );
	device->readPage(channel,chip,block,page,availTag);
}

int main(int argc, const char **argv)
{
	FlashRequestProxy *device = 0;
	DmaConfigProxy *dmap = 0;

	FlashIndication *deviceIndication = 0;
	DmaIndication *dmaIndication = 0;

	if(sem_init(&done_sem, 1, 0)){
		fprintf(stderr, "failed to init done_sem\n");
		exit(1);
	}

	fprintf(stderr, "%s %s\n", __DATE__, __TIME__);

	device = new FlashRequestProxy(IfcNames_FlashRequest);
	dmap = new DmaConfigProxy(IfcNames_DmaConfig);
	DmaManager *dma = new DmaManager(dmap);

	deviceIndication = new FlashIndication(IfcNames_FlashIndication);
	dmaIndication = new DmaIndication(dma, IfcNames_DmaIndication);

	fprintf(stderr, "Main::allocating memory...\n");

	for ( int i = 0; i < DMA_BUFFER_COUNT; i++ ) {
		srcAllocs[i] = portalAlloc(alloc_sz);
		dstAllocs[i] = portalAlloc(alloc_sz);
		srcBuffers[i] = (unsigned int *)portalMmap(srcAllocs[i], alloc_sz);
		dstBuffers[i] = (unsigned int *)portalMmap(dstAllocs[i], alloc_sz);
	}
	portalExec_start();
	for ( int i = 0; i < DMA_BUFFER_COUNT; i++ ) {
		portalDCacheFlushInval(srcAllocs[i], alloc_sz, srcBuffers[i]);
		portalDCacheFlushInval(dstAllocs[i], alloc_sz, dstBuffers[i]);
		ref_srcAllocs[i] = dma->reference(srcAllocs[i]);
		ref_dstAllocs[i] = dma->reference(dstAllocs[i]);
	}

	// Storage system init /////////////////////////////////
	curWritesInFlight = 0;
	curCmdCountBudget = 0;
	pthread_mutex_init(&freeListMutex, NULL);
	pthread_mutex_init(&flashReqMutex, NULL);
	pthread_mutex_init(&cmdReqMutex, NULL);
	pthread_cond_init(&flashReqCond, NULL);
	pthread_cond_init(&cmdReqCond, NULL);

	for ( int i = 0; i < DMA_BUFFER_COUNT; i++ ) {
		for ( int j = 0; j < BUFFER_WAYS; j++ ) {
			int idx = i*BUFFER_WAYS+j;
			srcBufferBusy[idx] = false;

			int offset = j*1024*16;
			device->addReadHostBuffer(ref_dstAllocs[i], offset, idx);
			device->addWriteHostBuffer(ref_srcAllocs[i], offset, idx);
			writeBuffers[idx] = srcBuffers[i] + (offset/sizeof(unsigned int));
			readBuffers[idx] = dstBuffers[i] + (offset/sizeof(unsigned int));
		}
	}
	for ( int i = 0; i > TAG_COUNT; i++ ) {
		readTagBusy[i] = false;
	}
	/////////////////////////////////////////////////////////

	fprintf(stderr, "Main::flush and invalidate complete\n");
	device->start(0);
  
	device->sendTest(LARGE_NUMBER*1024);

	for ( int i = 0; i < (8192+64)/4; i++ ) {
		for ( int j = 0; j < BUFFER_COUNT; j++ ) {
			readBuffers[j][i] = 8192/4-i;
			writeBuffers[j][i] = i;
		}
	}

	timespec start, now;
	printf( "writing pages to flash!\n" );
	clock_gettime(CLOCK_REALTIME, & start);
	for ( int i = 0; i < LARGE_NUMBER/2; i++ ) {
		for ( int j = 0; j < 2; j++ ) {
			writePage(device, j*8,0,0,i,waitIdleWriteBuffer());
			if ( i % 1024 == 0 ) printf( "writing page %d\n", i );
		}
	}
	printf( "waiting for writing pages to flash!\n" );
	while ( getNumWritesInFlight() > 0 ) usleep(1000);
	clock_gettime(CLOCK_REALTIME, & now);
	printf( "finished writing to page! %f\n", timespec_diff_sec(start, now) );

	printf( "wrote pages to flash!\n" );
  
	clock_gettime(CLOCK_REALTIME, & start);
	for ( int i = 0; i < LARGE_NUMBER/2; i++ ) {
		for ( int j = 0; j < 2; j++ ) {

			readPage(device, j*8,0,0,i);
			if ( i % 1024 == 0 ) 
				printf( "reading page %d\n", i );
		}
	}
	
	printf( "trying reading from page!\n" );

	while (true) {
		int rrb = popReadyReadBuffer();
		while (rrb >= 0 ) {
			setFinishedReadBuffer(rrb);
			rrb = popReadyReadBuffer();
		}

		flushFinishedReadBuffers(device);
		if ( getNumReadsInFlight() == 0 ) break;
	}
	clock_gettime(CLOCK_REALTIME, & now);
	printf( "finished reading from page! %f\n", timespec_diff_sec(start, now) );

	for ( int i = 0; i < (8192+64)/4; i++ ) {
		for ( int j = 0; j < BUFFER_COUNT; j++ ) {
			if ( i > (8192+64)/4 - 2 )
			printf( "%d %d %d\n", j, i, readBuffers[j][i] );
		}
	}
	for ( int i = 0; i < 2048; i++ ) {
		printf( "%d: %f\n", i, timecheck[i] );
	}
	printf( "Command buget was gone:%d \nTag was busy:%d\n", noCmdBudgetCount, noTagCount );

	exit(0);
}
