/* 
 * Connection for GPIB bus.
 * Copyright (C) 2007-2009 Petr Kubanek <petr@kubanek.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef __RTS2_CONN_GPIB__
#define __RTS2_CONN_GPIB__

#include "../utils/rts2value.h"

#include <list>

namespace rts2sensord
{

/**
 * Class for connections to GPIB devices.
 *
 * This is an abstract class, which provides interface to GPIB. It provides
 * methods to communicate with IEEE-488 GPIB bus. It is subclassed by
 * ConnGpibLinux and ConnGpibEnet.
 *
 * @author Petr Kubanek <petr@kubanek.net>
 */
class ConnGpib
{
	public:
		/**
		 * Write command to GPIB bus.
		 *
		 * @param cmd Null terminated string, which will be written to bus.
		 *
		 * @throw rts2core::Error and its descendats.
		 */
		virtual void gpibWrite (const char *cmd) = 0;

		/*
		 * Read data from GPIB device to a buffer.
		 *
		 * @param reply  Buffer where null terminated data will be stored.
		 * @param blen   Buffer length in bytes.
		 *
		 * @throw rts2core::Error and its descendants
		 */
		virtual void gpibRead (void *reply, int &blen) = 0;

		/**
		 * Write to GPIB bus and read back reply.
		 *
		 * @cmd    Null terminated buffer (string) which will be written to GPIB
		 * @reply  Buffer to store null terminated reply.
		 * @blen   Reply buffer lenght.
		 *
		 * @throw rts2core::Error and its descendats
		 */
		virtual void gpibWriteRead (const char *cmd, char *reply, int blen) = 0;

		/**
		 * Wait while line assert serial register.
		 */
		virtual void gpibWaitSRQ () = 0;

		// function usefull for SCPI style programming

		void readInt (const char *buf, int &val);

		/**
		 * Read value from GPIB bus.
		 */
		void readValue (const char *buf, Rts2Value *val);

		void readValue (const char *subsystem, std::list < Rts2Value * >&vals, int prefix_num);

		void readValue (const char *buf, Rts2ValueString * val);

		void readValue (const char *buf, Rts2ValueDouble * val);

		void readValue (const char *buf, Rts2ValueFloat * val);

		void readValue (const char *buf, Rts2ValueBool * val);

		void readValue (const char *buf, Rts2ValueSelection * val);

		/**
		 * Initialize GPIB connection.
		 */
		virtual void initGpib () = 0;

		/**
		 * Clear GPIB device.
		 */
		virtual void devClear () = 0;

		ConnGpib ()
		{
		}

		virtual ~ ConnGpib (void)
		{
		}
};

};
#endif		 /* !__RTS2_CONN_GPIB__ */
