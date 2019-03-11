/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                                                                           *
 *  Ghost, a micro-kernel based operating system for the x86 architecture    *
 *  Copyright (C) 2015, Max Schlüssel <lokoxe@gmail.com>                     *
 *                                                                           *
 *  This program is free software: you can redistribute it and/or modify     *
 *  it under the terms of the GNU General Public License as published by     *
 *  the Free Software Foundation, either version 3 of the License, or        *
 *  (at your option) any later version.                                      *
 *                                                                           *
 *  This program is distributed in the hope that it will be useful,          *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU General Public License for more details.                             *
 *                                                                           *
 *  You should have received a copy of the GNU General Public License        *
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.    *
 *                                                                           *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "ghost/calls/calls.h"

#include "kernel/tasking/tasking.hpp"
#include "kernel/tasking/tasking_memory.hpp"
#include "kernel/tasking/scheduler.hpp"
#include "kernel/tasking/wait.hpp"
#include "kernel/filesystem/filesystem_process.hpp"

#include "kernel/system/processor/processor.hpp"
#include "kernel/memory/memory.hpp"
#include "kernel/memory/gdt.hpp"
#include "kernel/memory/page_reference_tracker.hpp"
#include "kernel/kernel.hpp"
#include "shared/logger/logger.hpp"
#include "kernel/utils/hashmap.hpp"

static g_tasking_local* taskingLocal;
static g_mutex taskingIdLock;
static g_tid taskingIdNext = 0;

static g_hashmap<g_tid, g_task*>* taskGlobalMap;

g_tasking_local* taskingGetLocal()
{
	return &taskingLocal[processorGetCurrentId()];
}

g_tid taskingGetNextId()
{
	mutexAcquire(&taskingIdLock);
	g_tid next = taskingIdNext++;
	mutexRelease(&taskingIdLock);
	return next;
}

g_task* taskingGetById(g_tid id)
{
	return hashmapGet(taskGlobalMap, id, (g_task*) 0);
}

void taskingInitializeBsp()
{
	mutexInitialize(&taskingIdLock);
	taskingLocal = (g_tasking_local*) heapAllocate(sizeof(g_tasking_local) * processorGetNumberOfProcessors());
	taskGlobalMap = hashmapCreateNumeric<g_tid, g_task*>(128);

	taskingInitializeLocal();
}

void taskingInitializeAp()
{
	taskingInitializeLocal();
}

void taskingInitializeLocal()
{
	g_tasking_local* local = taskingGetLocal();
	local->locksHeld = 0;
	local->time = 0;

	local->scheduling.current = 0;
	local->scheduling.list = 0;
	local->scheduling.taskCount = 0;
	local->scheduling.round = 0;
	local->scheduling.idleTask = 0;
	local->scheduling.preferredNextTask = 0;

	mutexInitialize(&local->lock);

	g_process* idle = taskingCreateProcess();
	local->scheduling.idleTask = taskingCreateThread((g_virtual_address) taskingIdleThread, idle, G_SECURITY_LEVEL_KERNEL);
	logInfo("%! core: %i idle task: %i", "tasking", processorGetCurrentId(), idle->main->id);

	g_process* cleanup = taskingCreateProcess();
	taskingAssign(taskingGetLocal(), taskingCreateThread((g_virtual_address) taskingCleanupThread, cleanup, G_SECURITY_LEVEL_KERNEL));
	logInfo("%! core: %i cleanup task: %i", "tasking", processorGetCurrentId(), cleanup->main->id);

	schedulerInitializeLocal();
}

void taskingApplySecurityLevel(volatile g_processor_state* state, g_security_level securityLevel)
{
	if(securityLevel == G_SECURITY_LEVEL_KERNEL)
	{
		state->cs = G_GDT_DESCRIPTOR_KERNEL_CODE | G_SEGMENT_SELECTOR_RING0;
		state->ss = G_GDT_DESCRIPTOR_KERNEL_DATA | G_SEGMENT_SELECTOR_RING0;
		state->ds = G_GDT_DESCRIPTOR_KERNEL_DATA | G_SEGMENT_SELECTOR_RING0;
		state->es = G_GDT_DESCRIPTOR_KERNEL_DATA | G_SEGMENT_SELECTOR_RING0;
		state->fs = G_GDT_DESCRIPTOR_KERNEL_DATA | G_SEGMENT_SELECTOR_RING0;
		state->gs = G_GDT_DESCRIPTOR_KERNEL_DATA | G_SEGMENT_SELECTOR_RING0;
	} else
	{
		state->cs = G_GDT_DESCRIPTOR_USER_CODE | G_SEGMENT_SELECTOR_RING3;
		state->ss = G_GDT_DESCRIPTOR_USER_DATA | G_SEGMENT_SELECTOR_RING3;
		state->ds = G_GDT_DESCRIPTOR_USER_DATA | G_SEGMENT_SELECTOR_RING3;
		state->es = G_GDT_DESCRIPTOR_USER_DATA | G_SEGMENT_SELECTOR_RING3;
		state->fs = G_GDT_DESCRIPTOR_USER_DATA | G_SEGMENT_SELECTOR_RING3;
		state->gs = G_GDT_DESCRIPTOR_USER_DATA | G_SEGMENT_SELECTOR_RING3;
	}

	if(securityLevel <= G_SECURITY_LEVEL_DRIVER)
	{
		state->eflags |= 0x3000; // IOPL 3
	}
}

void taskingResetTaskState(g_task* task)
{
	g_virtual_address esp = task->stack.end - sizeof(g_processor_state);
	task->state = (g_processor_state*) esp;

	memorySetBytes((void*) task->state, 0, sizeof(g_processor_state));
	task->state->eflags = 0x200;
	task->state->esp = (g_virtual_address) task->state;
	taskingApplySecurityLevel(task->state, task->securityLevel);
}

g_task* taskingCreateThread(g_virtual_address eip, g_process* process, g_security_level level)
{
	g_task* task = (g_task*) heapAllocate(sizeof(g_task));
	task->id = taskingGetNextId();
	task->process = process;
	task->securityLevel = level;
	task->status = G_THREAD_STATUS_RUNNING;
	task->type = G_THREAD_TYPE_DEFAULT;

	task->overridePageDirectory = 0;

	task->tlsCopy.start = 0;
	task->tlsCopy.end = 0;
	task->tlsCopy.userThreadObject = 0;

	task->syscall.processingTask = 0;
	task->syscall.sourceTask = 0;
	task->syscall.handler = 0;
	task->syscall.data = 0;

	task->waitResolver = 0;
	task->waitData = 0;

	task->interruptionInfo = 0;

	// Switch to task directory
	g_physical_address returnDirectory = taskingTemporarySwitchToSpace(task->process->pageDirectory);

	taskingMemoryCreateStacks(task);
	taskingResetTaskState(task);
	task->state->eip = eip;

	taskingTemporarySwitchBack(returnDirectory);

	// Add to process
	mutexAcquire(&process->lock);

	g_task_entry* entry = (g_task_entry*) heapAllocate(sizeof(g_task_entry));
	entry->task = task;
	entry->next = process->tasks;
	process->tasks = entry;
	if(process->main == 0)
	{
		process->main = task;
		process->id = task->id;
		filesystemProcessCreate((g_pid) task->id);
	}
	if(task->securityLevel != G_SECURITY_LEVEL_KERNEL)
	{
		taskingPrepareThreadLocalStorage(task);
	}

	mutexRelease(&process->lock);

	hashmapPut(taskGlobalMap, task->id, task);
	return task;
}

void taskingAssign(g_tasking_local* local, g_task* task)
{
	mutexAcquire(&local->lock);

	bool alreadyInList = false;
	g_schedule_entry* existing = local->scheduling.list;
	while(existing)
	{
		if(existing->task == task)
		{
			alreadyInList = true;
			break;
		}
		existing = existing->next;
	}

	if(!alreadyInList)
	{
		g_schedule_entry* newEntry = (g_schedule_entry*) heapAllocate(sizeof(g_schedule_entry));
		newEntry->task = task;
		newEntry->next = local->scheduling.list;
		schedulerPrepareEntry(newEntry);
		local->scheduling.list = newEntry;

		local->scheduling.taskCount++;
	}

	task->assignment = local;

	mutexRelease(&local->lock);
}

bool taskingStore(g_virtual_address esp)
{
	g_task* task = taskingGetLocal()->scheduling.current;

	// Very first interrupt that happened on this processor
	if(!task)
	{
		taskingSchedule();
		return false;
	}

	// Store where registers were pushed to
	task->state = (g_processor_state*) esp;

	return true;
}

g_virtual_address taskingRestore(g_virtual_address esp)
{
	g_task* task = taskingGetLocal()->scheduling.current;
	if(!task)
		kernelPanic("%! tried to restore without a current task", "tasking");

	// Switch to process address space
	if(task->overridePageDirectory)
	{
		pagingSwitchToSpace(task->overridePageDirectory);
	} else
	{
		pagingSwitchToSpace(task->process->pageDirectory);
	}

	// For TLS: write user thread address to GDT & set GS of thread to user pointer segment
	gdtSetUserThreadObjectAddress(task->tlsCopy.userThreadObject);
	task->state->gs = 0x30;

	// Set TSS ESP0 for ring 3 tasks to return onto
	gdtSetTssEsp0(task->interruptStack.end);

	return (g_virtual_address) task->state;
}

void taskingSchedule()
{
	g_tasking_local* local = taskingGetLocal();

	if(!local->inInterruptHandler)
		kernelPanic("%! scheduling may only be triggered during interrupt handling", "tasking");

	// If there are kernel locks held by the currently running task, we may not
	// switch tasks - otherwise we will deadlock.
	if(local->locksHeld == 0)
	{
		schedulerSchedule(local);
	}
}

void taskingPleaseSchedule(g_task* task)
{
	schedulerPleaseSchedule(task);
}

g_process* taskingCreateProcess()
{
	g_process* process = (g_process*) heapAllocate(sizeof(g_process));
	process->main = 0;
	process->tasks = 0;

	mutexInitialize(&process->lock);

	process->tlsMaster.location = 0;
	process->tlsMaster.copysize = 0;
	process->tlsMaster.totalsize = 0;
	process->tlsMaster.alignment = 0;

	process->pageDirectory = taskingMemoryCreatePageDirectory();

	process->virtualRangePool = (g_address_range_pool*) heapAllocate(sizeof(g_address_range_pool));
	addressRangePoolInitialize(process->virtualRangePool);
	addressRangePoolAddRange(process->virtualRangePool, G_CONST_USER_VIRTUAL_RANGES_START, G_CONST_USER_VIRTUAL_RANGES_END);

	for(int i = 0; i < SIG_COUNT; i++)
	{
		process->signalHandlers[i].handlerAddress = 0;
		process->signalHandlers[i].returnAddress = 0;
		process->signalHandlers[i].task = 0;
	}

	process->heap.brk = 0;
	process->heap.start = 0;
	process->heap.pages = 0;

	process->environment.arguments = 0;
	process->environment.executablePath = 0;
	process->environment.workingDirectory = 0;

	return process;
}

void taskingPrepareThreadLocalStorage(g_task* thread)
{
	// if tls master copy available, copy it to thread
	g_process* process = thread->process;
	if(process->tlsMaster.location == 0)
	{
		logDebug("%! failed to copy tls master, not available in process", "tls");
		return;
	}

	// calculate size that TLS needs including alignment
	uint32_t alignedTotalSize = G_ALIGN_UP(process->tlsMaster.totalsize, process->tlsMaster.alignment);

	// allocate virtual range with aligned size of TLS + size of {g_user_thread}
	uint32_t requiredSize = alignedTotalSize + sizeof(g_user_thread);
	uint32_t requiredPages = G_PAGE_ALIGN_UP(requiredSize) / G_PAGE_SIZE;
	g_virtual_address tlsCopyStart = addressRangePoolAllocate(process->virtualRangePool, requiredPages, G_PROC_VIRTUAL_RANGE_FLAG_PHYSICAL_OWNER);
	g_virtual_address tlsCopyEnd = tlsCopyStart + requiredPages * G_PAGE_SIZE;

	// temporarily switch to target process directory, copy TLS contents
	g_physical_address returnDirectory = taskingTemporarySwitchToSpace(process->pageDirectory);
	for(g_virtual_address page = tlsCopyStart; page < tlsCopyEnd; page += G_PAGE_SIZE)
	{
		g_physical_address phys = bitmapPageAllocatorAllocate(&memoryPhysicalAllocator);
		pagingMapPage(page, phys, DEFAULT_USER_TABLE_FLAGS, DEFAULT_USER_PAGE_FLAGS);
		pageReferenceTrackerIncrement(phys);
	}

	// zero & copy TLS content
	memorySetBytes((void*) tlsCopyStart, 0, process->tlsMaster.totalsize);
	memoryCopy((void*) tlsCopyStart, (void*) process->tlsMaster.location, process->tlsMaster.copysize);

	// fill user thread
	g_virtual_address userThreadObject = tlsCopyStart + alignedTotalSize;
	g_user_thread* userThread = (g_user_thread*) userThreadObject;
	userThread->self = userThread;

	// switch back
	taskingTemporarySwitchBack(returnDirectory);

	// set threads TLS location
	thread->tlsCopy.userThreadObject = userThreadObject;
	thread->tlsCopy.start = tlsCopyStart;
	thread->tlsCopy.end = tlsCopyEnd;

	logDebug("%! created tls copy in process %i, thread %i at %h", "threadmgr", process->main->id, thread->id, thread->tlsCopy.location);
}

void taskingKernelThreadYield()
{
	g_tasking_local* local = taskingGetLocal();
	if(local->locksHeld > 0)
	{
		logInfo("%! warning: kernel thread %i tried to yield while holding %i kernel locks", "tasking", local->scheduling.current->id, local->locksHeld);
		return;
	}

	asm volatile ("int $0x81"
			:
			: "a"(0), "b"(0)
			: "cc", "memory");
}

void taskingKernelThreadExit()
{
	taskingGetLocal()->scheduling.current->status = G_THREAD_STATUS_DEAD;
	taskingKernelThreadYield();
}

void taskingIdleThread()
{
	for(;;)
	{
		asm("hlt");
	}
}

void taskingCleanupThread()
{
	g_tasking_local* local = taskingGetLocal();
	g_task* task = local->scheduling.current;
	for(;;)
	{
		// Find and remove dead tasks from local scheduling list
		mutexAcquire(&local->lock);

		g_schedule_entry* deadList = 0;
		g_schedule_entry* entry = local->scheduling.list;
		g_schedule_entry* previous = 0;
		while(entry)
		{
			g_schedule_entry* next = entry->next;
			if(entry->task->status == G_THREAD_STATUS_DEAD)
			{
				hashmapRemove(taskGlobalMap, entry->task->id);

				if(previous)
					previous->next = next;
				else
					local->scheduling.list = next;

				entry->next = deadList;
				deadList = entry;
			} else
			{
				previous = entry;
			}
			entry = next;
		}

		mutexRelease(&local->lock);

		// Remove each task
		while(deadList)
		{
			g_schedule_entry* next = deadList->next;
			taskingRemoveThread(deadList->task);
			heapFree(deadList);
			deadList = next;
		}

		// Sleep for some time
		waitSleep(task, 3000);
		taskingKernelThreadYield();
	}
}

void taskingRemoveThread(g_task* task)
{
	if(task->status != G_THREAD_STATUS_DEAD)
		kernelPanic("%! tried to remove a task %i that is not dead", "tasking", task->id);

	// Switch to task space
	g_physical_address returnDirectory = taskingTemporarySwitchToSpace(task->process->pageDirectory);

	// Free stack pages
	if(task->interruptStack.start)
	{
		for(g_virtual_address page = task->interruptStack.start; page < task->interruptStack.end; page += G_PAGE_SIZE)
		{
			g_physical_address pagePhys = pagingVirtualToPhysical(page);
			if(pagePhys > 0)
			{
				if(pageReferenceTrackerDecrement(pagePhys) == 0)
					bitmapPageAllocatorMarkFree(&memoryPhysicalAllocator, pagePhys);
				pagingUnmapPage(page);
			}
		}
		addressRangePoolFree(memoryVirtualRangePool, task->interruptStack.start);
	}
	for(g_virtual_address page = task->stack.start; page < task->stack.end; page += G_PAGE_SIZE)
	{
		g_physical_address pagePhys = pagingVirtualToPhysical(page);
		if(pagePhys > 0)
		{
			if(pageReferenceTrackerDecrement(pagePhys) == 0)
				bitmapPageAllocatorMarkFree(&memoryPhysicalAllocator, pagePhys);
			pagingUnmapPage(page);
		}
	}
	if(task->securityLevel == G_SECURITY_LEVEL_KERNEL)
		addressRangePoolFree(memoryVirtualRangePool, task->stack.start);
	else
		addressRangePoolFree(task->process->virtualRangePool, task->stack.start);

	// Free TLS copy if available
	if(task->tlsCopy.start)
	{
		for(g_virtual_address page = task->tlsCopy.start; page < task->tlsCopy.end; page += G_PAGE_SIZE)
		{
			g_physical_address pagePhys = pagingVirtualToPhysical(page);
			if(pagePhys > 0)
			{
				if(pageReferenceTrackerDecrement(pagePhys) == 0)
					bitmapPageAllocatorMarkFree(&memoryPhysicalAllocator, pagePhys);
				pagingUnmapPage(page);
			}
		}
		addressRangePoolFree(memoryVirtualRangePool, task->tlsCopy.start);
	}

	taskingTemporarySwitchBack(returnDirectory);

	// Remove self from process
	mutexAcquire(&task->process->lock);

	g_task_entry* entry = task->process->tasks;
	g_task_entry* previous = 0;
	while(entry)
	{
		if(entry->task == task)
		{
			if(previous)
			{
				previous->next = entry->next;
			} else
			{
				task->process->tasks = entry->next;
			}
			heapFree(entry);
			break;
		}
		previous = entry;
		entry = entry->next;
	}

	mutexRelease(&task->process->lock);

	if(task->process->tasks == 0)
	{
		taskingRemoveProcess(task->process);

	} else if(task->process->main == task)
	{
		taskingKillProcess(task->process->id);
	}

	heapFree(task);
}

void taskingKillProcess(g_pid pid)
{
	g_task* task = hashmapGet<g_pid, g_task*>(taskGlobalMap, pid, 0);
	if(!task)
	{
		logInfo("%! tried to kill non-existing process %i", "tasking", pid);
		return;
	}

	mutexAcquire(&task->process->lock);

	g_task_entry* entry = task->process->tasks;
	while(entry)
	{
		entry->task->status = G_THREAD_STATUS_DEAD;
		entry = entry->next;
	}

	mutexRelease(&task->process->lock);
}

void taskingRemoveProcess(g_process* process)
{
	mutexAcquire(&process->lock);
	g_physical_address returnDirectory = taskingTemporarySwitchToSpace(process->pageDirectory);

	// Clear mappings and free physical space above 4 MiB
	g_page_directory directoryCurrent = (g_page_directory) G_CONST_RECURSIVE_PAGE_DIRECTORY_ADDRESS;
	for(uint32_t ti = 1; ti < 1024; ti++)
	{
		if((directoryCurrent[ti] & G_PAGE_ALIGN_MASK) & G_PAGE_TABLE_USERSPACE)
		{
			g_page_table table = ((g_page_table) G_CONST_RECURSIVE_PAGE_DIRECTORY_AREA) + (0x400 * ti);
			for(uint32_t pi = 0; pi < 1024; pi++)
			{
				if(table[pi])
				{
					g_physical_address page = table[pi] & ~G_PAGE_ALIGN_MASK;

					int rem = pageReferenceTrackerDecrement(page);
					if(rem == 0)
					{
						bitmapPageAllocatorMarkFree(&memoryPhysicalAllocator, page);
					}
				}
			}
		}
	}

	taskingTemporarySwitchBack(returnDirectory);
	mutexRelease(&process->lock);

	filesystemProcessRemove(process->id);

	heapFree(process->virtualRangePool);
	bitmapPageAllocatorMarkFree(&memoryPhysicalAllocator, process->pageDirectory);
	heapFree(process);
}

g_physical_address taskingTemporarySwitchToSpace(g_physical_address pageDirectory)
{

	g_physical_address back = pagingGetCurrentSpace();
	g_tasking_local* local = taskingGetLocal();
	if(local->scheduling.current)
	{
		if(local->scheduling.current->overridePageDirectory != 0)
			kernelPanic("%! tried temporary directory switching twice", "tasking");

		local->scheduling.current->overridePageDirectory = pageDirectory;
	}
	pagingSwitchToSpace(pageDirectory);
	return back;
}

void taskingTemporarySwitchBack(g_physical_address back)
{
	g_tasking_local* local = taskingGetLocal();
	if(local->scheduling.current)
	{
		local->scheduling.current->overridePageDirectory = 0;
	}
	pagingSwitchToSpace(back);
}

g_raise_signal_status taskingRaiseSignal(g_task* task, int signal)
{
	g_signal_handler* handler = &(task->process->signalHandlers[signal]);
	if(handler->handlerAddress)
	{
		g_task* handlingTask = 0;
		if(handler->task == task->id)
			handlingTask = task;
		else
			handlingTask = taskingGetById(handler->task);

		if(!handlingTask)
		{
			logInfo("%! signal(%i, %i): registered signal handler task %i doesn't exist", "signal", task->id, signal, handler->task);
			return G_RAISE_SIGNAL_STATUS_INVALID_TARGET;
		}

		if(handlingTask->interruptionInfo)
		{
			logInfo("%! can't raise signal in currently interrupted task %i", "signal", task->id);
			return G_RAISE_SIGNAL_STATUS_INVALID_STATE;
		}

		taskingInterruptTask(task, handler->handlerAddress, handler->returnAddress, 1, signal);

	} else if(signal == SIGSEGV)
	{
		logInfo("%! thread %i killed by SIGSEGV", "signal", task->id);
		task->status = G_THREAD_STATUS_DEAD;
		if(taskingGetLocal()->scheduling.current == task)
			taskingSchedule();
	}

	return G_RAISE_SIGNAL_STATUS_SUCCESSFUL;
}

void taskingInterruptTask(g_task* task, g_virtual_address entry, g_virtual_address returnAddress, int argumentCount, ...)
{
	if(task->securityLevel == G_SECURITY_LEVEL_KERNEL)
	{
		logInfo("%! kernel task %i can not be interrupted", "tasking", task->id);
		return;
	}

	mutexAcquire(&task->process->lock);
	g_physical_address returnDirectory = taskingTemporarySwitchToSpace(task->process->pageDirectory);

	// Prepare interruption
	task->interruptionInfo = (g_task_interruption_info*) heapAllocate(sizeof(g_task_interruption_info));
	task->interruptionInfo->previousWaitData = task->waitData;
	task->interruptionInfo->previousWaitResolver = task->waitResolver;
	task->interruptionInfo->previousStatus = task->status;
	task->waitData = 0;
	task->waitResolver = 0;
	task->status = G_THREAD_STATUS_RUNNING;

	// Save processor state
	memoryCopy(&task->interruptionInfo->state, task->state, sizeof(g_processor_state));
	task->interruptionInfo->statePtr = (g_processor_state*) task->state;

	// Set the new entry
	task->state->eip = entry;

	// Pass parameters on stack
	uint32_t* esp = (uint32_t*) (task->state->esp);
	va_list args;
	va_start(args, argumentCount);
	while(argumentCount--)
	{
		--esp;
		*esp = va_arg(args, uint32_t);
	}
	va_end(args);

	// Put return address on stack
	--esp;
	*esp = returnAddress;

	// Set new ESP
	task->state->esp = (uint32_t) esp;

	taskingTemporarySwitchBack(returnDirectory);
	mutexRelease(&task->process->lock);
}