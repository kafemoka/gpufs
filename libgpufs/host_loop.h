/* 
 * This expermental software is provided AS IS.
 * Feel free to use/modify/distribute,
 * If used, please retain this disclaimer and cite
 * "GPUfs: Integrating a file system with GPUs",
 * M Silberstein,B Ford,I Keidar,E Witchel
 * ASPLOS13, March 2013, Houston,USA
 */

/* 
 * This expermental software is provided AS IS.
 * Feel free to use/modify/distribute,
 * If used, please retain this disclaimer and cite
 * "GPUfs: Integrating a file system with GPUs",
 * M Silberstein,B Ford,I Keidar,E Witchel
 * ASPLOS13, March 2013, Houston,USA
 */

#ifndef HOST_LOOP_CPP
#define HOST_LOOP_CPP

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <iostream>
#include <iomanip>

#include <nvToolsExt.h>

#include "fs_initializer.cu.h"
//#include "gpufs_con_lib.h"

#define TRACE std::cout << __FILE__ << ":" << __LINE__ << std::endl;

void fd2name(const int fd, char* name, int namelen)
{

	char slink[100];
	pid_t me = getpid();
	name[0] = 0;
	sprintf(slink, "/proc/%d/fd/0", me);

	int s = readlink(slink, name, namelen - 1);
	if (s >= 0)
		name[s] = '\0';
}

double transfer_time = 0;
double asyncMemCpyTime[RW_HOST_WORKERS] = {0};
int asyncMemCpyCount[RW_HOST_WORKERS] = {0};
size_t asyncMemCpySize[RW_HOST_WORKERS] = {0};

bool debug_expecting_close = 0;

double total_stat = 0;
double total_stat1 = 0;

volatile int done = 0;

volatile int writeRequests[RW_HOST_WORKERS] = {0};
volatile int activeEntries[RW_HOST_WORKERS][RW_SCRATCH_PER_WORKER][RW_SLOTS_PER_WORKER] = {0};

pthread_mutex_t rwLoopTasksLocks[RW_HOST_WORKERS];
pthread_cond_t  rwLoopTasksConds[RW_HOST_WORKERS];

void* memoryMenager( void* param )
{
	TaskData* taskData = (TaskData*)param;

	int gpuid = taskData->gpuid;
	int id = taskData->id;
	volatile GPUGlobals* globals = taskData->gpuGlobals;
	int currentScratchIDs[RW_HOST_WORKERS] = {0};
	int lastScratchIDs[RW_HOST_WORKERS] = {0};

	cudaEvent_t events[RW_HOST_WORKERS][RW_SCRATCH_PER_WORKER];

	for( int i = 0; i < RW_HOST_WORKERS; ++i )
	{
		for( int j = 0; j < RW_SCRATCH_PER_WORKER; ++j )
		{
			CUDA_SAFE_CALL( cudaEventCreate( &events[i][j] ) );
		}
	}

	while( !done )
	{
		volatile cudaError_t cuda_status = cudaStreamQuery( globals->streamMgr->kernelStream );
		if ( cudaErrorNotReady != cuda_status )
		{
			done = 1;
			break;
		}

		for( int i = 0; i < RW_HOST_WORKERS; ++i )
		{
			int firstSlot = i * RW_SLOTS_PER_WORKER;

			// check for pending writes
			if( lastScratchIDs[i] != currentScratchIDs[i] )
			{
				cudaError_t cuda_status = cudaEventQuery( events[i][lastScratchIDs[i]] );

				if( cudaSuccess == cuda_status )
				{
					asyncMemCpyTime[i] += _timestamp();

//					std::cout << "Finished event[" << i << "][" << lastScratchIDs[i] << "]" << std::endl;

					// complete the request
					for( int k = 0; k < RW_SLOTS_PER_WORKER; ++k )
					{
						if( activeEntries[i][lastScratchIDs[i]][k] != 0 )
						{
//							std::cout << "Notifying request complete[" << firstSlot + k << "]" << std::endl;
							globals->cpu_ipcRWQueue->entries[ firstSlot + k ].status = CPU_IPC_READY;
							activeEntries[i][lastScratchIDs[i]][k] = 0;
						}
					}

					__sync_synchronize();

					lastScratchIDs[i]++;
					if( RW_SCRATCH_PER_WORKER == lastScratchIDs[i] )
					{
						lastScratchIDs[i] = 0;
					}
				}
			}

			// Check if we ran out of space in the ring buffer
			if( ( currentScratchIDs[i] + 1 == lastScratchIDs[i] ) ||
				( ( currentScratchIDs[i] + 1 == RW_SCRATCH_PER_WORKER ) && ( lastScratchIDs[i] == 0 ) )	)
			{
				// We can't handle any more requests right now
				continue;
			}

			int writeReq = writeRequests[i];

			if( writeReq == 0 )
			{
				continue;
			}

//			std::cout << "Got request. Worker: " << i << std::endl;

			// Handle new requests
			if( writeReq > 0 )
			{
				asyncMemCpyTime[i] -= _timestamp();

				writeRequests[i] = 0;

				// Notify the rw loop that we've read the request
				pthread_mutex_lock( &rwLoopTasksLocks[i] );
				pthread_cond_signal( &rwLoopTasksConds[i] );
				pthread_mutex_unlock( &rwLoopTasksLocks[i] );

				CUDA_SAFE_CALL(
						cudaMemcpyAsync( ( (char* ) globals->stagingArea[i][currentScratchIDs[i]] ), globals->streamMgr->scratch[i][currentScratchIDs[i]], writeReq,
								cudaMemcpyHostToDevice, globals->streamMgr->memStream[i] ) );

				CUDA_SAFE_CALL( cudaEventRecord( events[i][currentScratchIDs[i]], globals->streamMgr->memStream[i]));

//				std::cout << "Recording event[" << i << "][" << currentScratchIDs[i] << "]" << std::endl;

				currentScratchIDs[i]++;
				if( RW_SCRATCH_PER_WORKER == currentScratchIDs[i] )
				{
					currentScratchIDs[i] = 0;
				}
			}
		}
	}
}

void* rw_task( void* param )
{
	TaskData* taskData = (TaskData*)param;

	int id = taskData->id;
	volatile GPUGlobals* globals = taskData->gpuGlobals;

	int firstSlot = id * RW_SLOTS_PER_WORKER;

	volatile CPU_IPC_RW_Entry* entries[RW_SLOTS_PER_WORKER];

	for( int i = 0; i < RW_SLOTS_PER_WORKER; i++ )
	{
		entries[i] = &globals->cpu_ipcRWQueue->entries[firstSlot + i];
	}

	int scratchIdx = 0;

	while( !done )
	{
		size_t scratchSize = 0;
		int numRequests = 0;

		for( int i = 0; i < RW_SLOTS_PER_WORKER; i++ )
		{
			volatile CPU_IPC_RW_Entry* e = entries[i];
			if( e->status == CPU_IPC_PENDING )
			{
//				std::cout << "Handle request. Worker: " << id << " scratch id: " << scratchIdx << " scratch offset: " << scratchSize
//						<< " request id: " << firstSlot + i << std::endl;

 				e->status = CPU_IPC_IN_PROCESS;

				while( globals->cpu_ipcRWFlags->entries[id][scratchIdx] != 0 )
				{
//					std::cout << "Waiting. Worker: " << id << " scratch id: " << scratchIdx << " status: " << globals->cpu_ipcRWFlags->entries[id][scratchIdx] << std::endl;
//					TRACE
					usleep(0);
				}

				int req_cpu_fd = e->cpu_fd;
				size_t req_buffer_offset = e->buffer_offset;
				size_t req_file_offset = e->file_offset;
				size_t req_size = e->size;
				int req_type = e->type;

				assert(
						req_type == RW_IPC_READ || req_type == RW_IPC_WRITE || req_type == RW_IPC_DIFF || req_type == RW_IPC_TRUNC );

				if( req_type != RW_IPC_TRUNC )
				{
					assert( req_cpu_fd >= 0 && req_size > 0 );
				}

				switch( req_type )
				{
				case RW_IPC_READ:
				{
					// read
					int cpu_read_size = 0;

					nvtxRangePushA( "pread" );
					cpu_read_size = pread( req_cpu_fd, globals->streamMgr->scratch[id][scratchIdx] + scratchSize, req_size,
							req_file_offset );
					nvtxRangePop();

					assert( cpu_read_size > 0 );

					e->return_size = cpu_read_size;
					e->return_offset = scratchSize;
					e->scratch_index = scratchIdx;
					__sync_synchronize();

					activeEntries[id][scratchIdx][i] = 1;
					scratchSize += cpu_read_size;
					numRequests++;
				}
					break;

				default:
					assert( NULL );
				}
				nvtxRangePop();
				asyncMemCpyTime[id] += _timestamp();
			}
		}

		if( 0 == numRequests )
		{
			// Didn't find any request, move on
			continue;
		}

		asyncMemCpyCount[id]++;
		asyncMemCpySize[id] += scratchSize;

		globals->cpu_ipcRWFlags->entries[id][scratchIdx] = numRequests;
		__sync_synchronize();

		pthread_mutex_lock( &rwLoopTasksLocks[id] );

		writeRequests[id] = scratchSize;
//		std::cout << "Pass request. Worker: " << id << std::endl;

		pthread_cond_wait( &rwLoopTasksConds[id], &rwLoopTasksLocks[id]);
		pthread_mutex_unlock( &rwLoopTasksLocks[id] );

		scratchIdx++;
		if( RW_SCRATCH_PER_WORKER == scratchIdx )
		{
			scratchIdx = 0;
		}
	}

	return NULL;
}


void* open_task( void* param )
{
	TaskData* taskData = (TaskData*)param;

	int gpuid = taskData->gpuid;
	int id = taskData->id;
	volatile GPUGlobals* globals = taskData->gpuGlobals;

	char* use_gpufs_lib = getenv("USE_GPUFS_DEVICE");

	while( !done )
	{
		for (int i = 0; i < FSTABLE_SIZE; i++)
		{
			char filename[FILENAME_SIZE];
			volatile CPU_IPC_OPEN_Entry* e = &globals->cpu_ipcOpenQueue->entries[i];
			// we are doing open
			if (e->status == CPU_IPC_PENDING && e->cpu_fd < 0)
			{
				double vvvv = _timestamp();
				memcpy(filename, (char*) e->filename, FILENAME_SIZE);
				// OPEN
				if (e->flags & O_GWRONCE)
				{
					e->flags = O_RDWR | O_CREAT;
				}
				char pageflush = 0;
				int cpu_fd = -1;
				struct stat s;
				if (e->do_not_open)
				{

					if (stat(filename, &s) < 0)
					{
						fprintf(stderr, " problem with STAT file %s on CPU: %s\n",
								filename, strerror(errno));
					}
					//	fprintf(stderr,"Do not open for inode %d, time %d\n",s.st_ino, s.st_mtime);
				}
				else
				{
					if (use_gpufs_lib)
						cpu_fd = gpufs_file_open(globals->gpufs_fd, gpuid, filename,
								e->flags, S_IRUSR | S_IWUSR, &pageflush);
					else
					{
						cpu_fd = open(filename, e->flags, S_IRUSR | S_IWUSR);
					}

					if (cpu_fd < 0)
					{
						fprintf(stderr,
								"Problem with opening file %s on CPU: %s \n ",
								filename, strerror(errno));
					}

					if (fstat(cpu_fd, &s))
					{
						fprintf(stderr,
								"Problem with fstat the file %s on CPU: %s \n ",
								filename, strerror(errno));
					}
				}

				//fprintf(stderr, "FD %d,  inode %ld, size %ld, Found file %s\n",i, s.st_ino, s.st_size, filename);

				e->cpu_fd = cpu_fd;
				e->flush_cache = pageflush;
				e->cpu_inode = s.st_ino;
				e->size = s.st_size;
				e->cpu_timestamp = s.st_ctime;
				__sync_synchronize();
				e->status = CPU_IPC_READY;
				__sync_synchronize();
				total_stat += (_timestamp() - vvvv);
			}
			if (e->status == CPU_IPC_PENDING && e->cpu_fd >= 0)
			{
				double vvvv1 = _timestamp();
				// do close
				// fprintf(stderr, "FD %d, closing file %s\n",i, e->filename);

				if (use_gpufs_lib)
				{
					if (e->is_dirty)
					{ // if dirty, update gpufs device, but keep the file open
						e->cpu_fd = gpufs_file_close_stay_open(globals->gpufs_fd,
								gpuid, e->cpu_fd);
					}
					else
					{
						e->cpu_fd = gpufs_file_close(globals->gpufs_fd, gpuid,
								e->cpu_fd);
					}
					gpufs_drop_residence(globals->gpufs_fd, gpuid,
							e->drop_residence_inode);
				}
				else
				{
					if (!e->is_dirty)
					{
						e->cpu_fd = close(e->cpu_fd);
					}
				}
				__sync_synchronize();
				e->status = CPU_IPC_READY;
				__sync_synchronize();
				total_stat1 += _timestamp() - vvvv1;
			}

		}


	}

	return NULL;
}

Page diff_page;

int max_req = 0;
int report = 0;




void run_gpufs_handler(volatile GPUGlobals* gpuGlobals, int gpuid)
{
	done = 0;

	double totalTime = 0;

	totalTime -= _timestamp();

	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE );

	pthread_t openLoopTasksIDs[FSTABLE_SIZE];
	TaskData openLoopTasksData[FSTABLE_SIZE];

	pthread_t rwLoopTasksIDs[RW_HOST_WORKERS];
	TaskData rwLoopTasksData[RW_HOST_WORKERS];

	pthread_t memoryMenagerID;
	TaskData memoryMenagerData;

	memoryMenagerData.gpuGlobals = gpuGlobals;

//	pthread_create( &memoryMenagerID, &attr, memoryMenager, &memoryMenagerData );

	for( int i = 0; i < RW_HOST_WORKERS; ++i )
	{
		rwLoopTasksData[i].id = i;
		rwLoopTasksData[i].gpuGlobals =  gpuGlobals;
		rwLoopTasksData[i].gpuid = 0;

//		rwLoopTasksConds[i] = {0};
//		rwLoopTasksLocks[i] = {0};

		pthread_create( (pthread_t*)&(rwLoopTasksIDs[i]), &attr, rw_task, (TaskData*)&(rwLoopTasksData[i]) );
	}

	for( int i = 0; i < 1; ++i )
	{
		openLoopTasksData[i].id = i;
		openLoopTasksData[i].gpuGlobals =  gpuGlobals;
		openLoopTasksData[i].gpuid = gpuid;

		pthread_create( &openLoopTasksIDs[i], &attr, open_task, &openLoopTasksData[i] );
	}

	pthread_attr_destroy( &attr );

	memoryMenager( &memoryMenagerData );

//	CUDA_SAFE_CALL(
//		cudaStreamSynchronize( gpuGlobals->streamMgr->kernelStream ));
//	done = 1;
//
//	pthread_join( memoryMenagerID, NULL );

	for( int i = 0; i < 1; ++i )
	{
		pthread_join( openLoopTasksIDs[i], NULL );
	}

	for( int i = 0; i < RW_HOST_WORKERS; ++i )
	{
		pthread_join( rwLoopTasksIDs[i], NULL );
	}

	totalTime += _timestamp();

	fprintf(stderr, "kernel is complete\n");
//	fprintf(stderr, "Max pending requests: %d\n", max_req);
	fprintf(stderr, "Transfer time: %fms\n", totalTime / 1e3);

	int totalCount = 0;
	size_t totalSize = 0;
	double cpyTime = 0;

	for (int i = 0; i < RW_HOST_WORKERS; i++)
	{
		fprintf(stderr, "Async memcpy [%d]:\n", i);
		fprintf(stderr, "\tTime: %fms\n", asyncMemCpyTime[i] / 1e3);
		fprintf(stderr, "\tCount: %d\n", asyncMemCpyCount[i]);
		fprintf(stderr, "\tSize: %lluMB\n", asyncMemCpySize[i] >> 20);
//		fprintf(stderr, "\tAverage buffer size: %lluKB\n", (asyncMemCpySize[i] >> 10) / asyncMemCpyCount[i]);
		fprintf(stderr, "\tBandwidth: %fGB/s\n\n", ((float)asyncMemCpySize[i] / (1 << 30)) / (totalTime / 1e6));

		totalCount += asyncMemCpyCount[i];
		totalSize += asyncMemCpySize[i];
	}

	fprintf(stderr, "Async memcpy total:\n");
	fprintf(stderr, "\tCount: %d\n", totalCount);
	fprintf(stderr, "\tSize: %lluMB\n", totalSize >> 20);
//	fprintf(stdout, "\tAverage buffer size: %lluKB\n", (totalSize >> 10) / totalCount);
	fprintf(stdout, "\tBandwidth: %fGB/s\n\n", ((float)totalSize / (1 << 30)) / (totalTime / 1e6));
}

#endif
