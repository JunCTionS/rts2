/* 
 * Classes for camera image.
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

#include "cameraimage.h"
#include "rts2devcliimg.h"
#include "../utils/timestamp.h"

CameraImage::~CameraImage (void)
{
	for (std::vector < ImageDeviceWait * >::iterator iter =
		deviceWaits.begin (); iter != deviceWaits.end (); iter++)
	{
		delete *iter;
	}
	deviceWaits.clear ();
	delete image;
}


void
CameraImage::waitForDevice (Rts2DevClient * devClient, double after)
{
	deviceWaits.push_back (new ImageDeviceWait (devClient, after));
}


bool
CameraImage::waitingFor (Rts2DevClient * devClient)
{
	bool ret = false;
	for (std::vector < ImageDeviceWait * >::iterator iter =
		deviceWaits.begin (); iter != deviceWaits.end ();)
	{
		ImageDeviceWait *idw = *iter;
		if (idw->getClient () == devClient
			&& (isnan (devClient->getConnection ()->getInfoTime ())
			|| idw->getAfter () <
			devClient->getConnection ()->getInfoTime ()))
		{
			delete idw;
			iter = deviceWaits.erase (iter);
			ret = true;
		}
		else
		{
			logStream (MESSAGE_DEBUG) << "waitingFor " << (idw->getClient () ==
				devClient) <<
				Timestamp (devClient->getConnection ()->
				getInfoTime ()) << " " << Timestamp (idw->
				getAfter ()) <<
				sendLog;
			iter++;
		}
	}
	return ret;
}


bool
CameraImage::canDelete ()
{
	if (isnan (exEnd) || !dataWriten)
		return false;
	return deviceWaits.empty ();
}


CameraImages::~CameraImages (void)
{
	for (CameraImages::iterator iter = begin (); iter != end (); iter++)
	{
		delete (*iter).second;
	}
	clear ();
}


void
CameraImages::deleteOld ()
{
	for (CameraImages::iterator iter = begin (); iter != end ();)
	{
		CameraImage *ci = (*iter).second;
		if (ci->canDelete ())
		{
			delete ci;
			erase (iter++);
		}
		else
		{
			iter++;
		}
	}
}


void
CameraImages::infoOK (Rts2DevClientCameraImage * master, Rts2DevClient * client)
{
	for (CameraImages::iterator iter = begin (); iter != end ();)
	{
		CameraImage *ci = (*iter).second;
		if (ci->waitingFor (client))
		{
			ci->image->writeConn (client->getConnection (), INFO_CALLED);
			if (ci->canDelete ())
			{
				master->processCameraImage (iter++);
			}
		}
		else
		{
			iter++;
		}
	}
}


void
CameraImages::infoFailed (Rts2DevClientCameraImage * master,
Rts2DevClient * client)
{
	for (CameraImages::iterator iter = begin (); iter != end ();)
	{
		CameraImage *ci = (*iter).second;
		if (ci->waitingFor (client))
		{
			ci->image->writeConn (client->getConnection (), INFO_CALLED);
			master->processCameraImage (iter++);
		}
		else
		{
			iter++;
		}
	}
}
