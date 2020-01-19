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

#include "tester.hpp"
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

int main(int argc, char** argv)
{
	g_sleep(3000);
	klog("Starting test suite...");

	test_result_t result;
	result += runStdioTest();
//	result += runMessageTest();

	klog("Test suite finished: %i successful, %i failed", result.successful, result.failed);

	g_sleep(3000);
	klog("Restarting myself");
	g_spawn("/applications/tester.bin", "-respawned", "/", G_SECURITY_LEVEL_APPLICATION);
}
