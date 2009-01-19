/*
 * Scriptor body.
 * Copyright (C) 2007 Petr Kubanek <petr@kubanek.net>
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

#include "../utils/rts2device.h"
#include "rts2execcli.h"
#include "rts2devcliphot.h"
#include "rts2targetscr.h"

#include <map>
#include <stdexcept>

#define OPT_EXPAND_PATH       OPT_LOCAL + 101
#define OPT_GEN               OPT_LOCAL + 102

/** Prefix for variables holding script for device **/
#define DEV_SCRIPT_PREFIX     "SC_"

/**
 * This is main scriptor body. Scriptor is used to run on RTS2 device list,
 * reading script generated by script-gen command. Script generator is passed
 * as argument system state. Scriptor generator can then base its
 * decision on system state.
 *
 * @author Petr Kubanek <petr@kubanek.net>
 */
class Rts2Scriptor:public Rts2Device, public Rts2ScriptInterface
{
	private:
		Rts2ValueInteger *scriptCount;
		Rts2ValueString *expandPath;
		Rts2ValueSelection *scriptGen;

		std::map <std::string, Rts2ValueString *> scriptVar;

		Rts2TargetScr *currentTarget;
	protected:
		virtual int init ();
		virtual int processOption (int in_opt);
		virtual int willConnect (Rts2Address * in_addr);
		virtual Rts2DevClient *createOtherType (Rts2Conn *conn, int other_device_type);
		virtual void deviceReady (Rts2Conn * conn);
		virtual int setValue (Rts2Value * old_value, Rts2Value * new_value);
	public:
		Rts2Scriptor (int argc, char **argv);
		virtual ~Rts2Scriptor (void);

		virtual void postEvent (Rts2Event * event);

		virtual int findScript (std::string in_deviceName, std::string & buf);
		virtual void getPosition (struct ln_equ_posn *posn, double JD);
};

Rts2Scriptor::Rts2Scriptor (int in_argc, char **in_argv)
:Rts2Device (in_argc, in_argv, DEVICE_TYPE_SCRIPTOR, "SCRIPTOR"), Rts2ScriptInterface ()
{
	createValue (scriptCount, "script_count", "number of scripts execuced", false);
	createValue (expandPath, "expand_path", "expand path for new images", false);
	expandPath->setValueCharArr ("%f");

	createValue (scriptGen, "script_generator", "command which gets state and generates next script", false);
	scriptGen->addSelVal ("/etc/rts2/scriptor");

	addOption (OPT_EXPAND_PATH, "expand-path", 1, "path used for filename expansion");
	addOption (OPT_GEN, "script-gen", 1, "script generator");

	currentTarget = NULL;
}


Rts2Scriptor::~Rts2Scriptor (void)
{
}


int
Rts2Scriptor::init ()
{
	int ret;
	ret = Rts2Device::init ();
	if (ret)
		return ret;

	currentTarget = new Rts2TargetScr (this);
	currentTarget->moveEnded ();

	return 0;
}


int
Rts2Scriptor::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_EXPAND_PATH:
			expandPath->setValueCharArr (optarg);
			break;
		case OPT_GEN:
			scriptGen->addSelVal (optarg);
			break;
		default:
			return Rts2Device::processOption (in_opt);
	}
	return 0;
}


int
Rts2Scriptor::willConnect (Rts2Address * in_addr)
{
	if (in_addr->getType () < getDeviceType ())
		return 1;
	return 0;
}


Rts2DevClient *
Rts2Scriptor::createOtherType (Rts2Conn *conn, int other_device_type)
{
	switch (other_device_type)
	{
		case DEVICE_TYPE_CCD:
			return new Rts2DevClientCameraExec (conn, expandPath);
		default:
			return Rts2Device::createOtherType (conn, other_device_type);
	}
}


void
Rts2Scriptor::deviceReady (Rts2Conn * conn)
{
	// add variable for this device..
	Rts2ValueString *stringVal;
	createValue (stringVal, (std::string (DEV_SCRIPT_PREFIX) + std::string (conn->getName())).c_str (), std::string ("Script value for ") + std::string (conn->getName ()), true);
	updateMetaInformations (stringVal);
	scriptVar[std::string (conn->getName ())] = stringVal;

	conn->postEvent (new Rts2Event (EVENT_SET_TARGET, (void *) currentTarget));
//	conn->postEvent (new Rts2Event (EVENT_OBSERVE));
}


int
Rts2Scriptor::setValue (Rts2Value * old_value, Rts2Value * new_value)
{
	if (old_value == expandPath)
		return 0;
	return Rts2Device::setValue (old_value, new_value);
}


void
Rts2Scriptor::postEvent (Rts2Event * event)
{
	switch (event->getType ())
	{
		case EVENT_SCRIPT_ENDED:
			postEvent (new Rts2Event (EVENT_SET_TARGET, (void *) currentTarget));
			break;
		case EVENT_SCRIPT_STARTED:
//			postEvent (new Rts2Event (EVENT_OBSERVE));
			break;
	}
	Rts2Device::postEvent (event);
}


int
Rts2Scriptor::findScript (std::string in_deviceName, std::string & buf)
{
	std::ostringstream cmd;
	cmd << scriptGen->getSelName () << " " << getMasterState () << " " << in_deviceName;
	logStream (MESSAGE_DEBUG) << "Calling '" << cmd.str () << "'." << sendLog;
	FILE *gen = popen (cmd.str().c_str (), "r");

	char *filebuf = NULL;
	size_t len;
	ssize_t ret = getline (&filebuf, &len, gen);
	if (ret == -1)
		return -1;
	// replace \n
	filebuf[ret - 1] = '\0';
	buf = std::string (filebuf);
	logStream (MESSAGE_DEBUG) << "Script '" << buf << "'." << sendLog;
	pclose (gen);

	try
	{
	  	Rts2ValueString *val = scriptVar.at (in_deviceName);
		val->setValueString (filebuf);
		sendValueAll (val);
	}
	catch (std::out_of_range ex)
	{

	}

	free (filebuf);
	return 0;
}


void
Rts2Scriptor::getPosition (struct ln_equ_posn *posn, double JD)
{
	posn->ra = 20;
	posn->dec = 20;
}


int
main (int argc, char **argv)
{
	Rts2Scriptor scriptor = Rts2Scriptor (argc, argv);
	return scriptor.run ();
}
