/*
 * Copyright (c) 2012, Anton Staaf
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the project nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "swd_interface.h"

#include "libs/error/error_stack.h"
#include "libs/log/log_default.h"
#include "libs/command_line/command_line.h"

#include <unistd.h>
#include <stdio.h>
#include <ftdi.h>

using namespace Err;
using namespace Log;

enum PinData
{
    //                            7    6    5    4    3    2    1    0
    //                            led1 led0           rst  in   out  clk
    //
    state_idle         = 0xc9, // 1    1    0    0    1    0    0    1
    state_leds         = 0x09, // 0    0    0    0    1    0    0    1
    state_reset_target = 0xc1, // 1    1    0    0    0    0    0    1
    state_reset_swd    = 0xcb, // 1    1    0    0    1    0    1    1

    direction_write    = 0xfb, // 1    1    1    1    1    0    1    1
    direction_read     = 0xf9, // 1    1    1    1    1    0    0    1
};

/******************************************************************************/
namespace CommandLine
{
    static Scalar<int>          debug ("debug",  true,  0,
				       "What level of debug logging to use.");

    static Argument     *arguments[] = { &debug, null };
}
/******************************************************************************/
Error mpsse_setup_buffers(ftdi_context & ftdi)
{
    uint	read;
    uint	write;

    CheckP(ftdi_usb_purge_buffers(&ftdi));

    CheckP(ftdi_read_data_set_chunksize(&ftdi, 65536));
    CheckP(ftdi_write_data_set_chunksize(&ftdi, 65536));

    CheckP(ftdi_read_data_get_chunksize(&ftdi, &read));
    CheckP(ftdi_write_data_get_chunksize(&ftdi, &write));

    debug(1, "Chunksize (r/w): %d/%d", read, write);

    return success;
}
/******************************************************************************/
Error mpsse_write(ftdi_context & ftdi, uint8 * buffer, int count)
{
    CheckEQ(ftdi_write_data(&ftdi, buffer, count), count);

    return success;
}
/******************************************************************************/
Error mpsse_read(ftdi_context & ftdi, uint8 * buffer, int count, int timeout)
{
    int	received = 0;

    /*
     * This is a crude timeout mechanism.  The time that we wait will never
     * be less than the requested number of milliseconds.  But it can certainly
     * be more.
     */
    for (int i = 0; i < timeout; ++i)
    {
	received += CheckP(ftdi_read_data(&ftdi,
					  buffer + received,
					  count - received));

	if (received >= count)
	{
	    debug(2, "Response took about %d milliseconds.", i);
	    return success;
	}

	/*
	 * The latency timer is set to 1ms, so we wait that long before trying
	 * again.
	 */
	usleep(1000);
    }

    return Err::timeout;
}
/******************************************************************************/
Error mpsse_synchronize(ftdi_context & ftdi)
{
    uint8	commands[] = {0xaa};
    uint8	response[2];

    Check(mpsse_write(ftdi, commands, sizeof(commands)));
    Check(mpsse_read (ftdi, response, sizeof(response), 1000));

    CheckEQ(response[0], 0xfa);
    CheckEQ(response[1], 0xaa);

    return success;
}
/******************************************************************************/
Error mpsse_setup(ftdi_context & ftdi)
{
    /*
     * 1MHz = 60Mhz / ((29 + 1) * 2)
     */
    uint8	commands[] =
    {
	DIS_DIV_5,
	DIS_ADAPTIVE,
	DIS_3_PHASE,
	EN_3_PHASE,
	TCK_DIVISOR,   29, 0,
	SET_BITS_LOW,  state_idle, direction_write,
	SET_BITS_HIGH, 0x00, 0x00
    };

    Check(mpsse_setup_buffers(ftdi));

    CheckP(ftdi_set_latency_timer(&ftdi, 1));

    CheckP(ftdi_set_bitmode(&ftdi, 0x00, BITMODE_RESET));
    CheckP(ftdi_set_bitmode(&ftdi, 0x00, BITMODE_MPSSE));

    Check(mpsse_synchronize(ftdi));

    CheckEQ(ftdi_write_data(&ftdi, commands, sizeof(commands)),
	    sizeof(commands));

    return success;
}
/******************************************************************************/
Error flash_leds(ftdi_context & ftdi)
{
    for (int i = 0; i < 2; ++i)
    {
	uint8	commands[] = {SET_BITS_LOW, 0x00, direction_write};

	commands[1] = (i & 1) ? state_leds : state_idle;

	CheckEQ(ftdi_write_data(&ftdi, commands, sizeof(commands)),
		sizeof(commands));

	usleep(200000);
    }

    return success;
}
/******************************************************************************/
#define SWD_HEADER_START	0x01
#define SWD_HEADER_AP		0x02
#define SWD_HEADER_DP		0x00
#define SWD_HEADER_READ		0x04
#define SWD_HEADER_WRITE	0x00
#define SWD_HEADER_A0		0x00
#define SWD_HEADER_A4		0x08
#define SWD_HEADER_A8		0x10
#define SWD_HEADER_AC		0x18
#define SWD_HEADER_PARITY	0x20
#define SWD_HEADER_PARK		0x80

uint8 swd_request(int address, bool debug_port, bool write)
{
    bool	parity  = debug_port ^ write;
    uint8	request = (SWD_HEADER_START |
			   (debug_port ? SWD_HEADER_DP : SWD_HEADER_AP) |
			   (write ? SWD_HEADER_WRITE : SWD_HEADER_READ) |
			   ((address & 0x03) << 3) |
			   SWD_HEADER_PARK);

    switch (address & 0x03)
    {
	case 0:
	case 3:
	    break;

	case 1:
	case 2:
	    parity ^= 1;
	    break;
    }

    if (parity)
	request |= SWD_HEADER_PARITY;

    return request;
}
/******************************************************************************/
bool swd_parity(uint32 data)
{
    uint32	step = data ^ (data >> 16);

    step = step ^ (step >> 8);
    step = step ^ (step >> 4);
    step = step ^ (step >> 2);
    step = step ^ (step >> 1);

    return (step & 1);
}
/******************************************************************************/
Error swd_reset(ftdi_context & ftdi)
{
    uint8	commands[] =
    {
	SET_BITS_LOW, state_reset_swd, direction_write,
	CLK_BYTES, 5, 0, CLK_BITS, 1,
	SET_BITS_LOW, state_idle, direction_write,
	CLK_BITS, 0
    };

    Check(mpsse_write(ftdi, commands, sizeof(commands)));

    return success;
}
/******************************************************************************/
Error error_main(int argc, char const ** argv)
{
    Error		check_error = success;
    ftdi_context	ftdi;
    uint		chipid;

    CheckCleanupP(ftdi_init(&ftdi), init_failed);

    CheckCleanupStringP(ftdi_usb_open(&ftdi, 0x0403, 0x6014), open_failed,
			"Unable to open FTDI device: %s",
			ftdi_get_error_string(&ftdi));

    CheckCleanupP(ftdi_usb_reset(&ftdi), reset_failed);
    CheckCleanupP(ftdi_set_interface(&ftdi, INTERFACE_A), interface_failed);
    CheckCleanupP(ftdi_read_chipid(&ftdi, &chipid), read_failed);

    debug(1, "FTDI chipid: %X", chipid);

    CheckCleanup(mpsse_setup(ftdi), mpsse_failed);
    CheckCleanup(flash_leds(ftdi), leds_failed);

    {
      SWDInterface swd(&ftdi);
      CheckCleanup(swd.initialize(), initialize_failed);
    }

  initialize_failed:
  leds_failed:
    CheckP(ftdi_set_bitmode(&ftdi, 0xFF, BITMODE_RESET));

  mpsse_failed:
  read_failed:
  interface_failed:
  reset_failed:
    CheckStringP(ftdi_usb_close(&ftdi),
		 "Unable to close FTDI device: %s",
		 ftdi_get_error_string(&ftdi));

  open_failed:
    ftdi_deinit(&ftdi);

  init_failed:
    return check_error;
}
/******************************************************************************/
int main(int argc, char const ** argv)
{
    Error	check_error = success;

    CheckCleanup(CommandLine::parse(argc, argv, CommandLine::arguments),
                 failure);

    log().set_level(CommandLine::debug.get());

    CheckCleanup(error_main(argc, argv), failure);
    return 0;

  failure:
    error_stack_print();
    return 1;
}
/******************************************************************************/