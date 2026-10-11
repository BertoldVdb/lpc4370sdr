/*
 * Copyright 2014 Benjamin Vernoux <bvernoux@airspy.com>
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
 *
 * This file is part of lpc4370sdr (derived from AirSpy).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <lpc4370sdr.h>

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	lpc4370sdr_lib_version_t lib_version;

	lpc4370sdr_lib_version(&lib_version);
	printf("lpc4370sdr lib version: %d.%d.%d\n",lib_version.major_version,
                                          lib_version.minor_version,
                                          lib_version.revision);

	return EXIT_SUCCESS;
}
