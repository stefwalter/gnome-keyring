/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* unit-test-other.c: Test miscellaneous functionality

   Copyright (C) 2007 Stefan Walter

   The Gnome Keyring Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Keyring Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Stef Walter <stef@memberwebs.com>
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "unit-test-auto.h"
#include "library/gnome-keyring.h"

/* 
 * Each test function must begin with (on the same line):
 *   void unit_test_
 * 
 * Tests will be run in the order specified here.
 */
 
void unit_test_set_display (CuTest* cu)
{
	GnomeKeyringResult res;
	
	/* Shouldn't work */
	res = gnome_keyring_daemon_set_display_sync ("WOOF");
	CuAssertIntEquals(cu, GNOME_KEYRING_RESULT_DENIED, res);	
}