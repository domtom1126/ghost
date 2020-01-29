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

#include "ghost/signal.h"

#include "kernel/calls/syscall_memory.hpp"
#include "kernel/tasking/tasking_memory.hpp"

#include "shared/logger/logger.hpp"

void syscallSbrk(g_task* task, g_syscall_sbrk* data)
{
	data->successful = taskingMemoryExtendHeap(task, data->amount, &data->address);
}

void syscallLowerMemoryAllocate(g_task* task, g_syscall_lower_malloc* data)
{
	logInfo("syscall not implemented: syscallLowerMemoryAllocate");
	for(;;)
		;
}

void syscallLowerMemoryFree(g_task* task, g_syscall_lower_malloc* data)
{
	logInfo("syscall not implemented: syscallLowerMemoryFree");
	for(;;)
		;
}

void syscallAllocateMemory(g_task* task, g_syscall_alloc_mem* data)
{
	logInfo("syscall not implemented: syscallAllocateMemory");
	for(;;)
		;
}

void syscallUnmap(g_task* task, g_syscall_unmap* data)
{
	logInfo("syscall not implemented: syscallUnmap");
	for(;;)
		;
}

void syscallShareMemory(g_task* task, g_syscall_share_mem* data)
{
	logInfo("syscall not implemented: syscallShareMemory");
	for(;;)
		;
}

void syscallMapMmioArea(g_task* task, g_syscall_map_mmio* data)
{
	logInfo("syscall not implemented: syscallMapMmioArea");
	for(;;)
		;
}
