/* 
 * Astro-Physics GTO mount daemon 
 * Copyright (C) 2009-2010, Markus Wildi, Petr Kubanek and INDI
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

#include "../utils/libnova_cpp.h"
#include "../utils/rts2config.h"

#include "tellx200.h"
#include "clicupola.h"
#include "pier-collision.h"

#define OPT_APGTO_ASSUME_PARKED  OPT_LOCAL + 55
#define OPT_APGTO_FORCE_START    OPT_LOCAL + 56
#define OPT_APGTO_KEEP_HORIZON   OPT_LOCAL + 58

#define SLEW_RATE_1200 '2'
#define SLEW_RATE_0900 '1'
#define SLEW_RATE_0600 '0'

#define MOVE_RATE_120000 '0'
#define MOVE_RATE_060000 '1'
#define MOVE_RATE_006400 '2'
#define MOVE_RATE_001200 '3'
#define MOVE_RATE_000100 '4'
#define MOVE_RATE_000050 '5'
#define MOVE_RATE_000025 '6'

#define setAPPark()         serConn->writePort ("#:KA#", 5)
#define setAPUnPark()       serConn->writePort ("#:PO#", 5)// ok, no response
#define setAPLongFormat()   serConn->writePort ("#:U#", 4) // ok, no response

#define TRACK_MODE_LUNAR         0 
#define TRACK_MODE_SOLAR         1 
#define TRACK_MODE_SIDEREAL      2 
#define TRACK_MODE_ZERO          3 
#define TRACK_MODE_ZERO_NO_RESET 4 
#define DIFFERENCE_MAX_WHILE_NOT_TRACKING 1.  // [deg]

namespace rts2teld
{
/**
 * Driver for Astro-Physics GTO protocol based mount. Some functions are
 * borrowed from INDI to make the transition smoother.
 * 
 * @author Markus Wildi
 * @author Petr Kubanek
 * @author Francisco Foster Buron
 */
class APGTO:public TelLX200 {
	public:
		APGTO (int argc, char **argv);
		virtual ~APGTO (void);
		virtual int processOption (int in_opt);
		virtual int init ();

		/**
		 * Reads mount initial values.
		 */
		virtual int initValues ();
		virtual int info ();
		virtual int idle ();

		virtual int setTo (double set_ra, double set_dec);

		virtual int startResync ();
		virtual int isMoving ();
		virtual int endMove ();
		virtual int stopMove ();
  
		virtual int startPark ();
		virtual int isParking ();
		virtual int endPark ();
  
		virtual int abortAnyMotion () ;
		virtual void valueChanged (Rts2Value * changed_value);
	protected:
		virtual int applyCorrectionsFixed (double ra, double dec);
		virtual void applyCorrections (double &tar_ra, double &tar_dec);

		virtual void startCupolaSync ();
		virtual void notMoveCupola ();

	private:
		int force_start ;
		double on_set_HA ;
		double on_zero_HA ;
		time_t move_timeout;
		time_t slew_start_time;

		int f_scansexa (const char *str0, double *dp);
		void getSexComponents(double value, int *d, int *m, int *s) ;

		// Astro-Physics LX200 protocol specific functions
		int getAPVersionNumber ();
		int getAPUTCOffset ();
		int setAPUTCOffset (int hours);
		int APSyncCMR (char *matchedObject);
		int selectAPMoveToRate (int moveToRate);
		int selectAPTrackingMode (int trackMode);
		int tel_read_declination_axis ();

		int tel_set_move_rate (char moveToRate);
		int setAPSiteLongitude( double Long) ;
		int setAPSiteLatitude( double Lat) ;
		int setAPLocalTime(int x, int y, int z) ;
		int setAPBackLashCompensation( int x, int y, int z) ;
		int setCalenderDate( int dd, int mm, int yy) ;

		// helper
		double siderealTime ();
		int checkSiderealTime (double limit) ;
		int setBasicData ();
		void ParkDisconnect ();

		int tel_slew_to (double ra, double dec);
		int tel_slew_to_altaz (double alt, double az);
		void set_move_timeout (time_t plus_time);
		/**
		 * Perform movement, do not move tube bellow horizon.
		 * @author Francisco Foster Buron
		 */
		int moveAvoidingHorizon (double ra, double dec);
		double HAp(double DECp, double DECpref, double HApref);
    		bool moveandconfirm(double interHAp, double interDECp); // FF: move and confirm that the position is reached
		int isInPosition(double coord1, double coord2, double err1, double err2, char coord); // Alonso: coord e {'a', 'c'} a = coordenadas en altaz y c lo otro

		int tel_check_coords (double ra, double dec);

		// fixed offsets
		Rts2ValueRaDec   *fixedOffsets;
		// further discussion with Petr required
		//int changeMasterState (int new_state);
		// Astro-Physics properties
		Rts2ValueInteger *APslew_rate;
		Rts2ValueInteger *APmove_rate;
		Rts2ValueDouble  *APutc_offset;
		Rts2ValueString  *APfirmware ;
		Rts2ValueString  *DECaxis_HAcoordinate ; // see pier_collision.c 
		Rts2ValueBool    *mount_tracking ;
		Rts2ValueBool    *assume_parked;
		Rts2ValueBool    *collision_detection; 
		Rts2ValueBool    *avoidBellowHorizon;
};

};

using namespace rts2teld;

void APGTO::getSexComponents(double value, int *d, int *m, int *s)
{
	*d = (int) fabs(value);
	*m = (int) ((fabs(value) - *d) * 60.0);
	*s = (int) rint(((fabs(value) - *d) * 60.0 - *m) *60.0);

	if (value < 0)
		*d *= -1;
}

int APGTO::f_scansexa ( const char *str0, double *dp) /* input string, cracked value, if return 0 */
{
	double a = 0, b = 0, c = 0;
	char str[128];
	char *neg;
	int r;

	/* copy str0 so we can play with it */
	strncpy (str, str0, sizeof(str)-1);
	str[sizeof(str)-1] = '\0';

	neg = strchr(str, '-');
	if (neg)
		*neg = ' ';
	r = sscanf (str, "%lf%*[^0-9]%lf%*[^0-9]%lf", &a, &b, &c);
	if (r < 1)
		return (-1);
	*dp = a + b/60 + c/3600;
	if (neg)
		*dp *= -1;
	return (0);
}

/**
 * Reads from APGTO the version character of the firmware
 *
 * @param none
 *
 * @return -1 and set errno on error, otherwise 0
 */
int APGTO::getAPVersionNumber()
{
	int ret= -1 ;

	char version[32];
	APfirmware->setValueString ("none");
	if (( ret = serConn->writeRead ( "#:V#", 4, version, 32, '#')) < 1 )
		return -1 ;
	version[ret] = '\0';
	APfirmware->setValueString ("test"); //version);
	return 0 ;
}

/**
 * Reads from APGTO the UTC offset and corrects the output according to Astro-Physics documentation firmware revision D
 * @param none
 *
 * @return -1 and set errno on error, otherwise 0
 */
int APGTO::getAPUTCOffset()
{
	int nbytes_read = 0;
	double offset ;

	char temp_string[16];

	if ((nbytes_read = serConn->writeRead ("#:GG#", 5, temp_string, 11, '#')) < 1 )
		return -1 ;
	// Negative offsets, see AP keypad manual p. 77
	if ((temp_string[0]== 'A') || ((temp_string[0]== '0') && (temp_string[1]== '0')) || (temp_string[0]== '@'))
	{
		int i;
		for (i = nbytes_read; i > 0; i--)
		{
			temp_string[i]= temp_string[i-1];
		}
		temp_string[0] = '-';
		temp_string[nbytes_read + 1] = '\0';

		if (temp_string[1] == 'A')
		{
			temp_string[1]= '0';
			switch (temp_string[2])
			{
				case '5':
					temp_string[2]= '1';
					break;
				case '4':
					temp_string[2]= '2';
					break;
				case '3':
					temp_string[2]= '3';
					break;
				case '2':
					temp_string[2]= '4';
					break;
				case '1':
					temp_string[2]= '5';
					break;
				default:
					logStream (MESSAGE_ERROR) << "APGTO::getAPUTCOffset string not handled >" << temp_string << "<END" << sendLog;
					return -1;
			}
		}
		else if (temp_string[1]== '0')
		{
			temp_string[1]= '0';
			temp_string[2]= '6';
		}
		else if( temp_string[1]== '@')
		{
			temp_string[1]= '0' ;
			switch (temp_string[2])
			{
				case '9':
					temp_string[2]= '7';
					break;
				case '8':
					temp_string[2]= '8';
					break;
				case '7':
					temp_string[2]= '9';
					break;
				case '6':
					temp_string[2]= '0';
					break;
				case '5':
					temp_string[1]= '1';
					temp_string[2]= '1';
					break;
				case '4':
					temp_string[1]= '1';
					temp_string[2]= '2';
					break;
				default:
					logStream (MESSAGE_DEBUG) <<"APGTO::getAPUTCOffset string not handled >" << temp_string << "<END" << sendLog;
					return -1;
			}    
		}
		else
		{
			logStream (MESSAGE_ERROR) <<"APGTO::getAPUTCOffset string not handled >" << temp_string << "<END" << sendLog;
		}
	}
	else
	{
		temp_string[nbytes_read - 1] = '\0';
	}
	if (f_scansexa(temp_string, &offset))
	{
		logStream (MESSAGE_ERROR) <<"APGTO::getAPUTCOffset string not handled  >" << temp_string << "<END" <<sendLog;
		return -1;
	}
	APutc_offset->setValueDouble( offset * 15. ) ;
	//logStream (MESSAGE_DEBUG) <<"APGTO::getAPUTCOffset received string >" << temp_string << "<END offset is " <<  APutc_offset->getValueDouble() <<sendLog;
	return 0;
}

/*!
 * Writes to APGTO UTC offset
 *
 *
 * @param hours
 *
 * @return -1 on failure, 0 otherwise
 */
int APGTO::setAPUTCOffset (int hours)
{
	int ret= -1;
	int h, m, s ;
	char temp_string[16];
	char retstr[1] ;
	// To avoid the peculiar output format of AP controller, see p. 77 key pad manual
	if (hours < 0.)
	{
		hours += 24.;
	}
	getSexComponents(hours, &h, &m, &s);
    
	snprintf (temp_string, sizeof(temp_string), "#:SG %+03d#", h);

	if ((ret = serConn->writeRead (temp_string, sizeof (temp_string), retstr, 1)) < 0)
		return -1;
	return 0;
}

/*!
 * Writes to APGTO sync #:CMR
 *
 *
 * @param *matchedObject 
 *
 * @return -1 on failure, 0 otherwise
 */
// wildi: not yet in in use
int 
APGTO::APSyncCMR(char *matchedObject)
{
	int error_type;
    
	if ( (error_type = serConn->writeRead ("#:CMR#", 6, matchedObject, 33, '#')) < 0)
		return error_type;
	return 0;
}
/*!
 * Writes to APGTO the tracking mode
 *
 *
 * @param trackMode
 *
 * @return -1 on failure, 0 otherwise
 */
int APGTO::selectAPTrackingMode(int trackMode)
{
  int error_type;
  double RA ;
  double JD ;
  double lng ;
  double local_sidereal_time ;
  switch (trackMode) {
    /* Lunar */
  case TRACK_MODE_LUNAR:
    //logStream (MESSAGE_DEBUG) <<"APGTO::selectAPTrackingMode setting tracking mode to lunar." << sendLog;
    if ( (error_type = serConn->writePort ("#:RT0#", 6)) < 0)
      return error_type;
    mount_tracking->setValueBool(true) ;
    break;
    
    /* Solar */
  case TRACK_MODE_SOLAR:
    //logStream (MESSAGE_DEBUG) <<"APGTO::selectAPTrackingMode setting tracking mode to solar." << sendLog;
    if ( (error_type = serConn->writePort ("#:RT1#", 6)) < 0)
      return error_type;
    mount_tracking->setValueBool(true) ;
    break;

    /* Sidereal */
  case TRACK_MODE_SIDEREAL:
    //logStream (MESSAGE_DEBUG) <<"APGTO::selectAPTrackingMode setting tracking mode to sidereal." << sendLog;
    if ( (error_type = serConn->writePort ("#:RT2#", 6)) < 0)
      return error_type;
    mount_tracking->setValueBool(true) ;
    break;

    /* Zero, used normally */
  case TRACK_MODE_ZERO:
    //logStream (MESSAGE_DEBUG) <<"APGTO::selectAPTrackingMode setting tracking mode to zero." << sendLog;
    if ( (error_type = serConn->writePort ( "#:RT9#", 6)) < 0)
      return error_type;
    mount_tracking->setValueBool(false) ;
    notMoveCupola() ;

    if( tel_read_ra ()) {
      logStream (MESSAGE_ERROR) <<"APGTO::selectAPTrackingMode can not read RA." << sendLog;
      return -1 ;
    }
    RA  = getTelRa() ;
    JD  = ln_get_julian_from_sys ();
    lng = telLongitude->getValueDouble ();
    local_sidereal_time= fmod((ln_get_mean_sidereal_time( JD) * 15. + lng + 360.), 360.);  // longitude positive to the East
    on_zero_HA= fmod( local_sidereal_time- RA+ 360., 360.) ;
    break;
    /* Zero, used if necessary during ::info, ::idle */
  case TRACK_MODE_ZERO_NO_RESET:
    //logStream (MESSAGE_DEBUG) <<"APGTO::selectAPTrackingMode setting tracking mode to zero, no reset of HA." << sendLog;
    if ( (error_type = serConn->writePort ( "#:RT9#", 6)) < 0)
      return error_type;
    mount_tracking->setValueBool(false) ;
    notMoveCupola() ;

    break;
  default:
    return -1;
    break;
  }
  return 0;
}

/*!
 * Writes to APGTO the observatory longitude
 *
 *
 * @param Long
 *
 * @return -1 on failure, 0 otherwise
 */
int APGTO::setAPSiteLongitude (double Long)
{
	int ret = -1;
	int d, m, s;
	char temp_string[32];
	char retstr[1];

	// fix east longitudes..
	if (Long < 0)
		Long += 360;

	getSexComponents (Long, &d, &m, &s);
	snprintf (temp_string, sizeof( temp_string ), "#:Sg %03d*%02d:%02d#", d, m, s);
  	if (( ret= serConn->writeRead ( temp_string, sizeof( temp_string ), retstr, 1)) < 0)
		return -1;
	return 0;
}

/*!
 * Writes to APGTO the observatory latitude
 *
 *
 * @param Lat
 *
 * @return -1 on failure, 0 otherwise
 */
int APGTO::setAPSiteLatitude(double Lat)
{
	int ret = -1;
	int d, m, s;
	char temp_string[32];
	char retstr[1];

	getSexComponents(Lat, &d, &m, &s);
	snprintf (temp_string, sizeof( temp_string ), "#:St %+02d*%02d:%02d#", d, m, s);
	if (( ret = serConn->writeRead ( temp_string, sizeof(temp_string), retstr, 1)) < 0)
		return -1 ;
	return 0;
}

/*!
 * Writes to APGTO the gear back lash compensation
 *
 *
 * @param x see Astro-Physics documentation
 * @param y see Astro-Physics documentation
 * @param z see Astro-Physics documentation
 *
 * @return -1 on failure, 0 otherwise
 */
int APGTO::setAPBackLashCompensation( int x, int y, int z)
{
	int ret = -1;
	char temp_string[16];
	char retstr[1];
  
	snprintf(temp_string, sizeof( temp_string ), "%s %02d:%02d:%02d#", "#:Br", x, y, z);
  
	if ((ret = serConn->writeRead ( temp_string, sizeof(temp_string), retstr, 1)) < 0)
		return -1 ;
	return 0;
}
/*!
 * Writes to APGTO the local timezone time
 *
 *
 * @param x hour
 * @param y minute
 * @param z seconds
 *
 * @return -1 on failure, 0 otherwise
 */
int APGTO::setAPLocalTime(int x, int y, int z)
{
	int ret = -1;
	char temp_string[16];
	char retstr[1];
  
	snprintf (temp_string, sizeof( temp_string ), "%s %02d:%02d:%02d#", "#:SL" , x, y, z);
	if ((ret = serConn->writeRead (temp_string, sizeof(temp_string), retstr, 1)) < 0)
		return -1;
	return 0;
}
/*!
 * Writes to APGTO the local timezone date
 *
 *
 * @param dd day
 * @param mm month
 * @param yy year
 *
 * @return -1 on failure, 0 otherwise
 */
int APGTO::setCalenderDate(int dd, int mm, int yy)
{
	char cmd_string[32];
	char temp_string[256];

	//Command: :SC MM/DD/YY#
	//Response: 32 spaces followed by “#”, followed by 32 spaces, followed by “#”
	//Sets the current date. Note that year fields equal to or larger than 97 are assumed to be 20 century, 
	//Year fields less than 97 are assumed to be 21st century.

	yy = yy % 100;
	snprintf(cmd_string, sizeof(cmd_string), "#:SC %02d/%02d/%02d#", mm, dd, yy);

	if (serConn->writeRead (cmd_string, 14, temp_string, 33, '#') < 1)
		return -1;
	if (serConn->readPort (temp_string, 33, '#') < 1)
		return -1 ;
	return 0;
}

/*!
 * Reads APGTO relative position of the declination axis to the observed hour angle.
 *
 * @return -1 on error, otherwise 0
 *
 */
int APGTO::tel_read_declination_axis ()
{
	int ret ;
	char new_declination_axis[32] ;

	ret = serConn->writeRead ("#:pS#", 5, new_declination_axis, 5, '#');
	if (ret < 0)
		return -1;
	new_declination_axis[ret - 1] = '\0';
	
	DECaxis_HAcoordinate->setValueString(new_declination_axis) ;
	return 0;
}

/*!
 * Set move rate.
 *
 * @param new_rate	new move speed to set.
 *
 * @return -1 on failure & set errno, 5 (>=0) otherwise
 */
int
APGTO::tel_set_move_rate (char moveToRate)
{
  char command[6];
  switch (moveToRate) {
    /* 0.25x*/
  case MOVE_RATE_000025:
    //logStream (MESSAGE_DEBUG) <<"APGTO::tel_set_move_rate setting move to rate to 0.25x." << sendLog;
    strcpy( command, "#:RG0#"); 
    /* 0.5x*/
    break;
  case MOVE_RATE_000050:
    //logStream (MESSAGE_DEBUG) <<"APGTO::tel_set_move_rate setting move to rate to 0.5x." << sendLog;
    strcpy( command, "#:RG1#"); 
    break;
    /* 1x*/
  case MOVE_RATE_000100:
    //logStream (MESSAGE_DEBUG) <<"APGTO::tel_set_move_rate setting move to rate to 1x." << sendLog;
    strcpy( command, "#:RG2#"); 
    break;
    /* 12x*/
  case MOVE_RATE_001200:
    //logStream (MESSAGE_DEBUG) <<"APGTO::tel_set_move_rate setting move to rate to 12x." << sendLog;
    strcpy( command, "#:RC0#"); 
    break;
    /* 64x */
  case MOVE_RATE_006400:
    //logStream (MESSAGE_DEBUG) <<"APGTO::tel_set_move_rate setting move to rate to 64x." << sendLog;
    strcpy( command, "#:RC1#"); 
    break;
    /* 600x */
  case MOVE_RATE_060000: 
    //logStream (MESSAGE_DEBUG) <<"APGTO::tel_set_move_rate setting move to rate to 600x." << sendLog;
    strcpy( command, "#:RC2#"); 
    break;
    /* 1200x */
  case MOVE_RATE_120000:
    //logStream (MESSAGE_DEBUG) <<"APGTO::tel_set_move_rate setting move to rate to 1200x." << sendLog;
    strcpy( command, "#:RC3#"); 
    break;
  default:
    return -1;
    break;
  }
  return serConn->writePort (command, 6);
}

/*!
 * Slew (=set) APGTO to new coordinates.
 *
 * @param ra 		new right ascenation
 * @param dec 		new declination
 *
 * @return -1 on error, otherwise 0
 */
int
APGTO::tel_slew_to (double ra, double dec)
{
  int ret ;
  char retstr;
  char target_DECaxis_HAcoordinate[32] ;
  struct ln_lnlat_posn observer;
  struct ln_equ_posn target_equ;

	if (collision_detection->getValueBool() == false)
	{
		logStream (MESSAGE_INFO) << "APGTO::tel_slew_to collision detection is disabled" << sendLog;
		return -1 ;
	}

  target_equ.ra = fmod( ra + 360., 360.) ;
  target_equ.dec= fmod( dec, 90.);

  double local_sidereal_time= getLocSidTime ();
  double target_HA= fmod( local_sidereal_time- target_equ.ra+ 360., 360.) ;

  if(( target_HA > 180.) && ( target_HA <= 360.)) {
    strcpy(target_DECaxis_HAcoordinate, "West");
    //logStream (MESSAGE_DEBUG) << "APGTO::tel_slew_to assumed target is 180. < HA <= 360." << sendLog;
  } else {
    strcpy(target_DECaxis_HAcoordinate, "East");
    //logStream (MESSAGE_DEBUG) << "APGTO::tel_slew_to assumed target is 0. < HA <= 180." << sendLog;
  }

  if( !( strcmp( "West", target_DECaxis_HAcoordinate))) {
    ret= pier_collision( &target_equ, &observer) ;
  } else if( !( strcmp( "East", target_DECaxis_HAcoordinate))) {
    //really target_equ.dec += 180. ;
    struct ln_equ_posn t_equ;
    t_equ.ra = target_equ.ra ;
    t_equ.dec= target_equ.dec + 180. ;
    ret= pier_collision( &t_equ, &observer) ;
  }
  if(ret != NO_COLLISION) {
    if( ret== COLLIDING) {
      logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to NOT slewing ra " << target_equ.ra << " dec " << target_equ.dec << " COLLIDING, NOT syncing cupola"  << sendLog;
    } else if( ret== WITHIN_DANGER_ZONE){
      logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to NOT slewing ra " << target_equ.ra << " dec " << target_equ.dec << " within DANGER zone, NOT syncing cupola"  << sendLog;
    } else {
      logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to NOT slewing ra " << target_equ.ra << " target_equ.dec " << target_equ.dec << " invalid condition, exiting"  << sendLog;
      exit(1) ;
    }
//    return -1;
  }
  logStream (MESSAGE_INFO) << "APGTO::tel_slew_to cleared geometry  slewing ra " << target_equ.ra << " target_equ.dec " << dec  << sendLog;

  if (( ret=tel_write_ra (target_equ.ra)) < 0) {
    logStream (MESSAGE_DEBUG) << "APGTO::tel_slew_to, not slewing, tel_write_ra return value was " << ret << sendLog ;
    return -1;
  }
  if (( ret=tel_write_dec (target_equ.dec)) < 0) {
    logStream (MESSAGE_DEBUG) << "APGTO::tel_slew_to, not slewing, tel_write_dec return value was " << ret << sendLog ;
    return -1;
  }
  // (re-)enable tracking
  // in case the mount detected a collision, tracking and any motion is stopped
  // but:
  // if target position is ok, mount can track again
  // renable tracking before slew
  if( ! mount_tracking->getValueBool()) {
    if ( selectAPTrackingMode(TRACK_MODE_SIDEREAL) < 0 ) { 
      logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to set track mode sidereal failed." << sendLog;
      notMoveCupola() ;
      return -1;
    } else {
      logStream (MESSAGE_DEBUG) << "APGTO::tel_slew_to set track mode sidereal (re-)enabled." << sendLog;
    }
  }
	// slew now
	if ((ret = serConn->writeRead ("#:MS#", 5, &retstr, 1)) < 0)
		return -1;

  //logStream (MESSAGE_DEBUG) << "APGTO::tel_slew_to #:MS# on ra " << ra << ", dec " << dec << sendLog ;
  if (retstr == '0') {
    if(( ret= tel_read_declination_axis()) != 0)
      return -1 ;
    // check if the assumption was correct
    if( !( strcmp( target_DECaxis_HAcoordinate, target_DECaxis_HAcoordinate))) {
      //logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to assumed target is correct" << sendLog;
    } else {
      if( (abortAnyMotion () !=0)) {
	logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to stoped any tracking and motion" << sendLog;
      }
    }
    on_set_HA= target_HA ; // used for defining state transition while tracking
    time(&slew_start_time);
    return 0;
  }
  logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to NOT slewing ra " << target_equ.ra << " dec " << target_equ.dec << " got '0'!= >" << retstr<<"<END, NOT syncing cupola"  << sendLog;
  return -1;
}
/*!
 * Check, if mount match given coordinates.
 *
 * @param ra		target right ascenation
 * @param dec		target declination
 *
 * @return -1 on error, 0 if not matched, 1 if matched, 2 if timeouted
 */
int
APGTO::tel_check_coords (double ra, double dec)
{
  // ADDED BY JF
  double sep;
  time_t now;

  struct ln_equ_posn object, target;

  time (&now);
  if (now > move_timeout)
    return 2;

  if ((tel_read_ra () < 0) || (tel_read_dec () < 0))
    return -1;

  object.ra = fmod(getTelRa () + 360., 360.);
  object.dec= fmod( getTelDec (), 90.);

  target.ra = fmod( ra+ 360., 360.);
  target.dec= fmod( dec , 90.) ;
  
  sep = ln_get_angular_separation (&object, &target);
  
  if (sep > 0.1)
    return 0;
  
  return 1;
}

/*
 * Solo mueve el telescopio  a las coordenadas indicadas, no guarda ningu  valor en la instancia
 */
int
APGTO::tel_slew_to_altaz(double alt, double az)
{
	char retstr;
	
	if(tel_write_altitude(alt) < 0)
	{
		logStream (MESSAGE_ERROR) <<"APGTO::tel_slew_to_altaz tel_write_altitude("<< alt<< ") failed"<< sendLog;
                return -1;
	}
	if(tel_write_azimuth(az) < 0)
	{
		logStream (MESSAGE_ERROR) <<"APGTO::tel_slew_to_altaz tel_write_azimuth("<< az<< ") failed"<< sendLog;
                return -1;
	}
	if (serConn->writeRead ("#:MS#", 5, &retstr, 1) < 0)
        {
                logStream (MESSAGE_ERROR) <<"APGTO::tel_slew_to_altaz tel_write_read #:MS# failed"<< sendLog;
                return -1;
        }

	logStream (MESSAGE_DEBUG) << "APGTO::tel_slew_to_altaz #:MS# on alt " << alt << ", az " << az << sendLog ;
	if(retstr == '0')
	{
		return 0;
	}
	else
	{
		logStream (MESSAGE_ERROR) << "APGTO::tel_slew_to_altaz error:" << "retstring:"<< retstr << "should be '0'" << sendLog;
		return -1;
	}
}

void APGTO::set_move_timeout (time_t plus_time)
{
	time_t now;
	time (&now);

	move_timeout = now + plus_time;
}

int APGTO::moveAvoidingHorizon (double ra, double dec)
{
	double _lst;
	double currentRa, newRA, currentHA, newHA, currentDEC, newDEC;
	double DECp1, HAp1;  // initial point
	double DECp2, HAp2;  // final point
	double DECpS = 53.39095314569674; // critical declination and hour angle in telescope internal coordinates
	double HApS = 115.58068825790751; // S: superior, I: inferior
	double DECpI = -53.39095314569674;
	double HApI = 180. - 115.58068825790751;

	if(tel_read_sidereal_time() < 0)
		return -1;
	_lst = lst->getValueDouble();
	
	if(tel_read_ra())
		return -1;
	
	currentRa = getTelRa();
	newRA = ra;
	
	currentHA = fmod(360.+_lst-currentRa, 360.);
	newHA = fmod(360.+_lst-ra, 360.);
	
	currentDEC = getTelDec();
	newDEC = dec;

	// find telescope internal coordinates
	if (currentHA <= 180)
	{
		DECp1 = fabs(currentDEC + 90.);
		HAp1 = currentHA;
	}
	else
	{
		DECp1 = -fabs(currentDEC + 90.);
		HAp1 = currentHA - 180.;
	}

	if (newHA <= 180)
	{
		DECp2 = fabs(newDEC + 90.);
		HAp2 = currentHA;
	}
	else
	{
		DECp2 = -fabs(newDEC + 90.);
		HAp2 = newHA - 180.;
	}
	  
	// first point is below superior critical line and above inferior critical line
	if (HAp(DECp1,DECpS,HApS) >= HAp1 && HAp(DECp1,DECpI,HApI) <= HAp1)
	{
		logStream (MESSAGE_DEBUG) << "APGTO::moveTo Initial Target in safe zone" << sendLog;

		// move to final target
		if(tel_slew_to(newRA, newDEC) < 0)
			return -1;
		set_move_timeout(100);
		return 0;
	}
	else
	{  // first point is either above superior critical line or below inferior critical line
		if (HAp(DECp1,DECpS,HApS) < HAp1) {  // initial point is above superior critical line:
			// final point is inside point's safe cone or above point's critical line and in the same excluded region
			if ((HAp2 <= HAp1 && DECp2 <= DECp1) || (HAp(DECp2,DECp1,HAp1) <= HAp2 && (DECp2 - DECpS) * (DECp1 - DECpS) >= 0))
			{
				// move to final target
				if(tel_slew_to(newRA, newDEC) < 0)
					return -1;
				set_move_timeout(100);
				return 0;
			}
			else
			{  // initial point is above superior critical line, but final point is not in safe cone
				// final point is below superior critical line or above superior critical line and in different excluded region
				if (HAp(DECp2,DECpS,HApS) > HAp2 || (HAp(DECp2,DECpS,HApS) <= HAp2 && (DECp2 - DECpS) * (DECp1 - DECpS) <= 0))
				{
					// go to superior critical line:
					if (DECp2 > DECp1) {  // with constant DECp...
						if (!moveandconfirm(HAp(DECp1,DECpS,HApS), DECp1))
							return -1; 
	  				} else { // or with constant HAp.
						if (!moveandconfirm(HAp1, DECp1 - (HAp1 - HAp(DECp1,DECpS,HApS))))
							return -1;
	  				}
	  
					// move to final target
					if(tel_slew_to(newRA, newDEC) < 0)
						return -1;
					set_move_timeout(100);
					return 0;
				}
				else
				{ // the final point is above the superior critical line, outside the safe cone and in the same region

					if (DECp2 >= DECp1)
					{ // move with constant DECp
						if (!moveandconfirm(HAp(DECp1,DECp2,HAp2), DECp1))
							return -1;
	 				}
					else
					{ // move with constant HAp
						if (!moveandconfirm(HAp1, DECp1 - (HAp1 - HAp(DECp1,DECp2,HAp2))))
							return -1;
					}
					// move to final target
					if(tel_slew_to(newRA, newDEC) < 0)
						return -1;
					set_move_timeout(100);
					return 0;
				}
			}
		}
		else
		{  // initial point is below inferior critical line
			// final point inside point's safe cone or below point's critical line and in the same excluded region
			if ((HAp2 >= HAp1 && DECp2 >= DECp1) || (HAp(DECp2,DECp1,HAp1) >= HAp2 && (DECp2 - DECpI) * (DECp1 - DECpI) >= 0))
			{
				// move to final target
				if(tel_slew_to(newRA, newDEC) < 0)
					return -1;
				set_move_timeout(100);
				return 0;
			}
			else
			{ 
				// final point above inferior critical line or below inferior critical line and in different excluded region
				if (HAp(DECp2,DECpI,HApI) <  HAp2 || (HAp(DECp2,DECpI,HApI) >= HAp2 && (DECp2 - DECpI) * (DECp1 - DECpI) <= 0))
				{
					// go to inferior critical line
					if (DECp2 > DECp1)
					{ // with constant DECp
						if (!moveandconfirm(HAp(DECp1,DECpI,HApI), DECp1))
							return -1;
					}
					else
					{ // with constant HAp
						if (!moveandconfirm(HAp1, DECp1 + (HAp(DECp1,DECpI,HApI) - HAp1)))
							return -1;
					}
					// move to final target
					if(tel_slew_to(newRA, newDEC) < 0)
						return -1;
					set_move_timeout(100);
					return 0;
				}
				else
				{ // final point below the inferior critical line, outside the safe cone and in the same region
					// go to inferior critical line
					if (DECp2 >= DECp1)
					{ // with constant DECp
						if (!moveandconfirm(HAp(DECp1,DECp2,HAp2), DECp1))
							return -1;
					}
					else
					{ // with constant HAp
						if (!moveandconfirm(HAp1, DECp1 + (HAp(DECp1,DECp2,HAp2) - HAp1)))
							return -1;
					}
					// move to final target
					if (tel_slew_to(newRA, newDEC) < 0)
						return -1;
					set_move_timeout(100);
					return 0;
				}
			}
		}
	}
	
	return 0;
}

// critical line given a reference point and an arbitrary DECp value
double APGTO::HAp(double DECp, double DECpref, double HApref)
{
  return HApref - (DECp - DECpref);
}

bool APGTO::moveandconfirm(double interHAp, double interDECp)
{
	double _lst;

	// given internal telescope coordinates, move and wait until telescope arrival

	logStream (MESSAGE_DEBUG) << "APGTO::moveTo moving to intermediate point"<< sendLog;

	// obtain DEC and RA from telescope internal coordinates DECp and HAp
	if (interDECp >= 0)
	{
		interDECp = interDECp - 90.;
		interHAp = interHAp;
	}
	else
	{
		interDECp = -(90. + interDECp);
		interHAp = interHAp + 180.;
	}    
	_lst = lst->getValueDouble();
	interHAp = fmod(360. + (_lst - interHAp), 360.);

	// move to new auxiliar position
	if (tel_slew_to(interHAp, interDECp) < 0)
		return false;
	while (isInPosition(interHAp, interDECp, .1, .1, 'c') != 0)
	{
		logStream (MESSAGE_DEBUG) << "APGTO::moveTo waiting to arrive to intermediate point"<< sendLog;
		usleep(100000);
	}
	stopMove ();
	return true;
}

// check whether the telescope is in a given position
// coord is 'a' = ALTAZ (altitude, azimuth), 'c' = RADEC (right ascension, declination) or 'h' = HADEC (hour angle, declination)
// Units are degrees

int APGTO::isInPosition(double coord1, double coord2, double err1, double err2, char coord)
{
	double _lst, currentHa;
  
	switch (coord)
	{
		case 'a': // ALTAZ
			if (tel_read_altitude() < 0)
			{
				logStream (MESSAGE_ERROR) << "APGTO::isInPosition tel_read_altitude error" << sendLog;
				return -1;
			}
			if (tel_read_azimuth() < 0)
			{
				logStream (MESSAGE_ERROR) << "APGTO::isInPosition tel_read_altitude error" << sendLog;
				return -1;
			}
			logStream (MESSAGE_DEBUG) << "APGTO::isInPosition distances : alt "<< fabs(telAltAz->getAlt()-coord1)<<
				"tolerance "<< err1<< " az "<< fabs(telAltAz->getAz() - coord2)<< " tolerance "<< err2<< sendLog;

			if(fabs(telAltAz->getAlt()-coord1) < err1 && fabs(telAltAz->getAz()-coord2) < err2)
				return 0;
			return  -1;
		case 'c': // RADEC
			if (tel_read_ra() < 0)
			{
				logStream (MESSAGE_ERROR) << "APGTO::isInPosition tel_read_ra error" << sendLog;
				return -1;
			}
			if (tel_read_dec() < 0)
			{
				logStream (MESSAGE_ERROR) << "APGTO::isInPosition tel_read_dec error" << sendLog;
				return -1;
			}
			logStream (MESSAGE_DEBUG) << "APGTO::isInPosition distances : current ra "<< getTelRa()<< " obj ra "<<coord1 <<
				" dist "<< fabs(getTelRa()-coord1)<< " tolerance "<< err1<<
				" current dec "<< getTelDec()<< " obj dec "<< coord2<< 
				" dist "<< fabs(getTelDec() - coord2)<< " tolerance "<< err2<< sendLog;

			if (fabs(getTelRa()-coord1) < err1 && fabs(getTelDec()-coord2) < err2)
				return 0;
			return -1;
		case 'h': // HADEC
			if (tel_read_sidereal_time() < 0)
				return -1;

			_lst = lst->getValueDouble();
			if (tel_read_ra() < 0)
			{
				logStream (MESSAGE_ERROR) << "APGTO::isInPosition tel_read_ra error" << sendLog;
				return -1;
			}
			if (tel_read_dec() < 0)
			{
				logStream (MESSAGE_ERROR) << "APGTO::isInPosition tel_read_dec error" << sendLog;
				return -1;
			}

			currentHa = fmod(360.+_lst-getTelRa(), 360.);
			logStream (MESSAGE_DEBUG) << "APGTO::isInPosition distances : current ha "<< currentHa<< " next ha "<<coord1<<
				" dist "<< fabs(currentHa-coord1)<< " tolerance "<< err1<<
				" current dec "<< getTelDec()<< " obj dec "<< coord2<<
				" dist "<< fabs(getTelDec() - coord2)<< " tolerance "<< err2<< sendLog;

			if (fabs(currentHa-coord1) < err1 && fabs(getTelDec()-coord2) < err2)
				return 0;
      
		default:
			return  -1;

	}
}

int APGTO::startResync ()
{
	int ret;

	if (avoidBellowHorizon->getValueBool ())
	{
		if (moveAvoidingHorizon (getTelTargetRa (), getTelTargetDec ()) < 0)
			return -1;
	}
	else
	{
		ret = tel_slew_to (getTelTargetRa (), getTelTargetDec ());
		if (ret)
			return -1;
		set_move_timeout (100);
	}
	return 0;
}

int APGTO::applyCorrectionsFixed (double ra, double dec)
{
	if (fixedOffsets->getRa () == 0 && fixedOffsets->getDec () == 0)
	{
		if (telFlip->getValueInteger () != 0)
			dec *= -1;
		fixedOffsets->setValueRaDec (ra, dec);
		logStream (MESSAGE_INFO) << "applying offsets as fixed offsets (" << LibnovaDegDist (ra) << " " << LibnovaDegDist (dec) << sendLog;
		return 0;
	}
	return TelLX200::applyCorrectionsFixed (ra, dec);
}

void APGTO::applyCorrections (double &tar_ra, double &tar_dec)
{
	TelLX200::applyCorrections (tar_ra, tar_dec);
	tar_ra -= fixedOffsets->getRa ();
	if (telFlip->getValueInteger () == 0)
		tar_dec -= fixedOffsets->getDec ();
	else
		tar_dec += fixedOffsets->getDec ();
}

void APGTO::startCupolaSync ()
{
  struct ln_equ_posn target_equ;
  getTarget (&target_equ);

  target_equ.ra = fmod( target_equ.ra + 360., 360.) ;
  target_equ.dec= fmod( target_equ.dec, 90.) ; 
  if( !( strcmp( "West", DECaxis_HAcoordinate->getValue()))) {
    postEvent (new Rts2Event (EVENT_CUP_START_SYNC, (void*) &target_equ));
  } else if( !( strcmp( "East", DECaxis_HAcoordinate->getValue()))) {
    //tel_equ.dec += 180. ;
    struct ln_equ_posn t_equ;
    t_equ.ra = target_equ.ra ;
    t_equ.dec= target_equ.dec + 180. ;
    postEvent (new Rts2Event (EVENT_CUP_START_SYNC, (void*) &t_equ));
  }
  logStream (MESSAGE_INFO) << "APGTO::startCupolaSync sync cupola" << sendLog;
}

void APGTO::notMoveCupola ()
{
  postEvent (new Rts2Event (EVENT_CUP_NOT_MOVE));
}

int APGTO::isMoving ()
{
	int ret = tel_check_coords (getTelTargetRa (), getTelTargetDec ());
	switch (ret)
	{
		case -1:
			return -1;
		case 1:
		case 2:
			return -2;
		default:
			return USEC_SEC / 10;
	}
}

int APGTO::endMove ()
{
	sleep (5);
	return Telescope::endMove ();
}

int APGTO::stopMove ()
{
	int error_type;
    
	if ((error_type = serConn->writePort ("#:Q#", 4)) < 0)
		return error_type;

	mount_tracking->setValueBool (false);
	notMoveCupola ();
	sleep (1);
	maskState (TEL_MASK_CORRECTING | TEL_MASK_MOVING | BOP_EXPOSURE, TEL_NOT_CORRECTING | TEL_OBSERVING, "move stopped");
	return 0;
}

/*!
 * Set mount to match given coordinates (sync)
 *
 * AP GTO remembers the last position only occasionally it looses it.
 *
 * @param ra		setting right ascscension
 * @param dec		setting declination
 *
 * @return -1 and set errno on error, otherwise 0
 */
int APGTO::setTo (double ra, double dec)
{
	char readback[101];

	if ((getState () & TEL_MASK_MOVING) != TEL_OBSERVING)
	{
		logStream (MESSAGE_INFO) << "APGTO::setTo mount is slewing, ignore sync command to RA " << ra << "Dec "<< dec << sendLog;
		return -1 ;
	}
	if ( !( collision_detection->getValueBool()))
	{
		logStream (MESSAGE_INFO) << "APGTO::setTo collision detection is disabled, enable it" << sendLog;
		return -1;
	}

	ra = fmod (ra + 360., 360.);
	dec = fmod (dec, 90.);
	if ((tel_write_ra (ra) < 0) || (tel_write_dec (dec) < 0))
		return -1;

	//AP manual:
	//Position
	//Command:  :CM#
	//Response: “Coordinates matched.     #”
	//          (there are 5 spaces between “Coordinates” and “matched”, and 8 trailing spaces before the “#”, 
	//          the total response length is 32 character plus the “#”.	  

	//logStream (MESSAGE_ERROR) <<"APGTO::setTo #:CM# doing SYNC" << sendLog;

	if (serConn->writeRead ("#:CM#", 5, readback, 100, '#') < 0)
		return -1;
	return 0 ;
}

/**
 * Park mount to neutral location.
 *
 * @return -1 and errno on error, 0 otherwise
 */
int APGTO::startPark ()
{
	setTargetAltAz (0, 355);
	return moveAltAz () ? -1 : 1;
}

int APGTO::isParking ()
{
  int ret= isMoving() ;

  if( ret > 0) {
    return USEC_SEC; // still moving
  }
  switch (ret) {
  case -2 :
    return -2 ; // target reached
    break ;
  case -1: // read error 
    logStream (MESSAGE_ERROR) << "APGTO::isParking coordinates read error" << sendLog;
    return -1 ;
    break ;
  default: 
    logStream (MESSAGE_ERROR) << "APGTO::isParking no valid case :" <<ret << sendLog;
    break ;
  }
  return -1 ;
}
int
APGTO::endPark ()
{
  logStream (MESSAGE_DEBUG) << "APGTO::endPark" << sendLog;
  ParkDisconnect() ;
  return 0;
}
void 
APGTO::ParkDisconnect()
{
  // The AP mount will not surely stop with #:KA alone
  if (stopMove () < 0) {
    logStream (MESSAGE_ERROR) << "APGTO::ParkDisconnect motion stop failed #:Q#" << sendLog;
    return;
  }
  if ( selectAPTrackingMode(TRACK_MODE_ZERO) < 0 ) { 
    logStream (MESSAGE_ERROR) << "APGTO::ParkDisconnect setting tracking mode ZERO failed." << sendLog;
    return;
  }
  // wildi ToDo: not yet decided
  // issuing #:KA# means a power cycle of the mount (not always)
  // From experience I know that the Astro-Physics controller saves the last position
  // without #:KA#, but not always!
  // so no #:KA# is sent and I leave the file handle open
  //  if (setAPPark() < 0) {
  //  logStream (MESSAGE_ERROR) << "APGTO::ParkDisconnect parking failed #:KA#" << sendLog;
  //  return;
  //}
  logStream (MESSAGE_INFO) << "APGTO::ParkDisconnect motion stopped, the mount is parked but still connected (no #:KA#)." << sendLog; 
  return;
}

int APGTO::abortAnyMotion ()
{
  int ret = 0 ;
  int failed= 0 ;

    if((ret = stopMove()) < 0) {
      logStream (MESSAGE_ERROR) << "APGTO::abortAnyMotion stop motion #:Q# failed, SWITCH the mount OFF" << sendLog;
      failed = 1 ;
    }
    if (( ret= selectAPTrackingMode(TRACK_MODE_ZERO)) < 0 ) {
      logStream (MESSAGE_ERROR) << "APGTO::abortAnyMotion setting tracking mode ZERO failed." << sendLog;
      failed = 1 ;
    }
    if( failed == 0) {
      logStream (MESSAGE_ERROR) << "APGTO::abortAnyMotion successfully stoped motion #:Q# and tracking" << sendLog;
      return 0 ;
    } else {
      logStream (MESSAGE_ERROR) << "APGTO::abortAnyMotion failed: check motion and tracking now" << sendLog;
      return -1 ;
    }
}

void 
APGTO::valueChanged (Rts2Value * changed_value)
{
  int ret= -1 ;
  int slew_rate= -1 ;
  int move_rate= -1 ;
  char command ;

  if (changed_value ==APslew_rate){
    if(( slew_rate= APslew_rate->getValueInteger())== 1200) {
      command= SLEW_RATE_1200 ;
    } else if( APslew_rate->getValueInteger()== 900) {
      command= SLEW_RATE_0900 ;
    } else if( APslew_rate->getValueInteger()== 600) {
      command= SLEW_RATE_0600 ;
    } else {
      APslew_rate->setValueInteger(-1);  
      logStream (MESSAGE_ERROR) << "APGTO::valueChanged wrong slew rate " << APslew_rate->getValue() << ", valid: 1200, 900, 600" << sendLog;
      return ;
    }

    if(( ret= tel_set_slew_rate (command)) !=5) {
      // wildi ToDo: thinking about what to do in this case
      APslew_rate->setValueInteger(-1); 
      logStream (MESSAGE_ERROR) << "APGTO::valueChanged tel_set_slew_rate failed" << sendLog;
      // return -1 ;
    }
    return ;
  } else if (changed_value ==APmove_rate) {

    if(( move_rate= APmove_rate->getValueInteger())== 600) {
      command= MOVE_RATE_060000 ;
    } else if( APmove_rate->getValueInteger()== 64) {
      	  command= MOVE_RATE_006400 ;
    } else if( APmove_rate->getValueInteger()== 12) {
      command= MOVE_RATE_001200 ;
    } else if( APmove_rate->getValueInteger()== 1) {
      command= MOVE_RATE_000100 ;
    } else {
      APmove_rate->setValueInteger(-1);  
      logStream (MESSAGE_ERROR) << "APGTO::valueChanged wrong move rate " << APmove_rate->getValue() << ", valid: 600, 64, 12, 1" << sendLog;
      return ;
    }

    if(( ret= tel_set_move_rate (command)) !=5) {
      // wildi ToDo: thinking about what to do in this case
      APmove_rate->setValueInteger(-1); 
      logStream (MESSAGE_ERROR) << "APGTO::valueChanged tel_set_move_rate failed" << sendLog;
      // return -1 ;
    }
    return ;
  }
  TelLX200::valueChanged (changed_value);
}

#define ERROR_IN_INFO 1
int APGTO::info ()
{
  int ret ;
  int flip= -1 ;
  int error= -1 ;

  if (tel_read_ra () || tel_read_dec ()) {
    error = ERROR_IN_INFO ;
    logStream (MESSAGE_ERROR) << "APGTO::info could not retrieve ra, dec  " << sendLog;
  }

  if(( ret= tel_read_local_time()) != 0) {
    error = ERROR_IN_INFO ;
    logStream (MESSAGE_ERROR) << "APGTO::info could not retrieve localtime  " << sendLog;
  }
  if(( ret= tel_read_sidereal_time()) != 0) {
    error = ERROR_IN_INFO ;
    logStream (MESSAGE_ERROR) << "APGTO::info could not retrieve sidereal time  " << sendLog;
  }
  if(( ret= tel_read_declination_axis()) != 0) {
    error = ERROR_IN_INFO ;
    logStream (MESSAGE_ERROR) << "APGTO::info could not retrieve position of declination axis  " << sendLog;
  }

  if( !( strcmp( "West", DECaxis_HAcoordinate->getValue()))) {
    flip = 1 ;
  } else if( !( strcmp( "East", DECaxis_HAcoordinate->getValue()))) {
    flip= 0 ;
  } else {
    error = ERROR_IN_INFO ;
    logStream (MESSAGE_ERROR) << "APGTO::info could not retrieve angle (declination axis, hour axis)" << sendLog;
  }
  
  telFlip->setValueInteger (flip);
  if (tel_read_azimuth () || tel_read_altitude ()) {
    error = ERROR_IN_INFO ;
    logStream (MESSAGE_ERROR) << "APGTO::info could not retrieve  azimuth altitude " << sendLog;
  }
  if( error== ERROR_IN_INFO) {
    return -1 ;
  }
  // while the telecope is tracking Astro-Physics controller does not
  // carry out any checks, meaning that it turns for ever
  // 
  // 0 <= HA <=180.: check if the mount approaches horizon
  // 180 < HA < 360: check if the mount crosses the meridian
  // Astro-Physics controller has the ability to delay the meridian flip.
  // Here I assume that the flip takes place on HA=0 on slew.
  // If the telescope does not collide, it may track as long as:
  // West:   HA < 15.
  // East:  Alt > 10.

  struct ln_equ_posn object;
  struct ln_lnlat_posn observer;
  object.ra = fmod( getTelRa () + 360., 360.);
  object.dec= fmod( getTelDec (), 90.);
  observer.lng = telLongitude->getValueDouble ();
  observer.lat = telLatitude->getValueDouble ();

  double JD= ln_get_julian_from_sys ();
  double local_sidereal_time= fmod((ln_get_mean_sidereal_time( JD) * 15. + observer.lng + 360.), 360.);  // longitude positive to the East
  double HA= fmod( local_sidereal_time- object.ra+ 360., 360.) ;
  // HA= [0.,360.], HA monoton increasing, but not strictly
  // man fmod: On success, these functions return the value x - n*y
  // e.g. if set on_set_HA=360.: 360. - 1 * 360.= 0.
  // after a slew HA > 0, therefore no transition occurs
  // wildi ToDo: verify the cases
  // move_ha_sg 00:00:00  0:
  // move_ha_sg 24:00:00  0:
  // with the Astro-Physics mount
  // wildi ToDo: This is the real stuff
//   if ((getState () & TEL_MASK_MOVING) == TEL_MOVING) {
//     logStream (MESSAGE_INFO) << "APGTO::info  moving" << sendLog ;
//   } else if ((getState () & TEL_MASK_MOVING) == TEL_PARKING) {
//     logStream (MESSAGE_INFO) << "APGTO::info  parking" << sendLog ;
//   } else {
//     logStream (MESSAGE_INFO) << "APGTO::info  not ( moving || parking)" << sendLog ;
//   }
	if ((getState () & TEL_MOVING) || (getState () & TEL_PARKING))
	{
		int stop= 0 ;
		if (!(strcmp("West", DECaxis_HAcoordinate->getValue())))
		{
			ret = pier_collision( &object, &observer) ;
			if (ret != NO_COLLISION)
			{
				// stop= 1;
				logStream (MESSAGE_ERROR) << "APGTO::info collision detected (West)" << sendLog;
				ret = NO_COLLISION;
			}
		}
		else if (!(strcmp("East", DECaxis_HAcoordinate->getValue())))
		{
			//really target_equ.dec += 180. ;
			struct ln_equ_posn t_equ;
			t_equ.ra = object.ra;
			t_equ.dec= object.dec + 180.;
			ret = pier_collision (&t_equ, &observer);
			if (ret != NO_COLLISION)
			{
				//stop = 1;
				logStream (MESSAGE_ERROR) << "APGTO::info collision detected (East)" << sendLog;
			}
		} 
		if (stop !=0 )
		{
			// if a collision is detected it is necessary that the mount can be moved with rot e, w, n, s commands
			// set collision_detection to true if that occurs
			if (collision_detection->getValueBool())
			{
				if ((abortAnyMotion () !=0))
				{
					logStream (MESSAGE_ERROR) << "APGTO::info failed to stop any tracking and motion" << sendLog;
					return -1;
				}
			}
			else
			{
				if (selectAPTrackingMode(TRACK_MODE_ZERO) < 0 )
				{
					logStream (MESSAGE_ERROR) << "APGTO::info setting tracking mode ZERO failed." << sendLog;
					return -1;
				}
				logStream (MESSAGE_ERROR) << "APGTO::info stop tracking but not motion" << sendLog;
			}
		}
	}
	else
	{
		ret = isMoving ();
		if (ret > 0)
		{
			// still moving, ignore that here
		}
		else
		{
			switch (ret)
			{
				case -2:
					break;
				case -1: // read error 
					logStream (MESSAGE_ERROR) << "APGTO::info coordinates read error" << sendLog;
					break ;
				default: 
					logStream (MESSAGE_ERROR) << "APGTO::info no valid case :" << ret << sendLog;
					break ;
			}
		}
	}
	// There is a bug in the revision D Astro-Physics controller
	// find out, when the local sidereal time gets wrong, difference is 237 sec
	// Check if the sidereal time read from the mount is correct 
	JD = ln_get_julian_from_sys ();
	double lng = telLongitude->getValueDouble ();
	local_sidereal_time = fmod ((ln_get_mean_sidereal_time (JD) * 15. + lng + 360.), 360.);  // longitude positive to the East
	double diff_loc_time = local_sidereal_time - lst->getValueDouble ();
	if (diff_loc_time >= 180.)
	{
		diff_loc_time -=360. ;
	}
	else if ((diff_loc_time) <= -180.)
	{
		diff_loc_time += 360. ;
	}
	if (fabs( diff_loc_time) > (1./8.) )
	{ // 30 time seconds
		logStream (MESSAGE_DEBUG) << "APGTO::info  local sidereal time, calculated time " 
			<< local_sidereal_time << " mount: "
			<< lst->getValueDouble() 
			<< " difference " << diff_loc_time <<sendLog;

		logStream (MESSAGE_DEBUG) << "APGTO::info ra " << getTelRa() << " dec " << getTelDec() << " alt " <<   telAltAz->getAlt() << " az " << telAltAz->getAz()  <<sendLog;

		char date_time[256];
    struct ln_date utm;
    struct ln_zonedate ltm;
    ln_get_date_from_sys( &utm) ;
    ln_date_to_zonedate(&utm, &ltm, -1 * timezone + 3600 * daylight); // Adds "only" offset to JD and converts back (see DST below)

    if(( ret= setAPLocalTime(ltm.hours, ltm.minutes, (int) ltm.seconds) < 0)) {
      logStream (MESSAGE_ERROR) << "APGTO::info setting local time failed" << sendLog;
      return -1;
    }
    if (( ret= setCalenderDate(ltm.days, ltm.months, ltm.years) < 0) ) {
      logStream (MESSAGE_ERROR) << "APGTO::info setting local date failed" << sendLog;
      return -1;
    }
    sprintf( date_time, "%4d-%02d-%02dT%02d:%02d:%02d", ltm.years, ltm.months, ltm.days, ltm.hours, ltm.minutes, (int) ltm.seconds) ;
    logStream (MESSAGE_DEBUG) << "APGTO::info local date and time set :" << date_time << sendLog ;

    // read the coordinates times again
    if (tel_read_ra () || tel_read_dec ())
      return -1;
    if (tel_read_azimuth () || tel_read_altitude ())
      return -1;
    if(( ret= tel_read_local_time()) != 0)
      return -1 ;
    if(( ret= tel_read_sidereal_time()) != 0)
      return -1 ;
    diff_loc_time = local_sidereal_time- lst->getValueDouble() ;

    logStream (MESSAGE_DEBUG) << "APGTO::info ra " << getTelRa() << " dec " << getTelDec() << " alt " <<   telAltAz->getAlt() << " az " << telAltAz->getAz()  <<sendLog;

    logStream (MESSAGE_DEBUG) << "APGTO::info  local sidereal time, calculated time " 
			      << local_sidereal_time << " mount: "
			      << lst->getValueDouble() 
			      << " difference " << diff_loc_time <<sendLog;
  } 
  // The mount unexpectedly starts to track, stop that
  if( ! mount_tracking->getValueBool()) {
    double RA= getTelRa() ;
    JD= ln_get_julian_from_sys ();
    local_sidereal_time= fmod((ln_get_mean_sidereal_time( JD) * 15. + lng + 360.), 360.);  // longitude positive to the East
    HA= fmod( local_sidereal_time- RA+ 360., 360.) ;

    double diff= HA -on_zero_HA ;
    // Shortest path
    if( diff >= 180.) {
      diff -=360. ;
    } else if( diff <= -180.) {
      diff += 360. ;
    }
    if( fabs( diff) > DIFFERENCE_MAX_WHILE_NOT_TRACKING) {
      logStream (MESSAGE_INFO) << "APGTO::info HA changed while mount is not tracking." << sendLog;
      notMoveCupola() ;
      if ( selectAPTrackingMode(TRACK_MODE_ZERO) < 0 ) {
	logStream (MESSAGE_ERROR) << "APGTO::info setting tracking mode ZERO failed." << sendLog;
	return -1;
      }
    }
  }
  return TelLX200::info ();
}
int
APGTO::idle ()
{
  // make sure that the checks in ::info are done
  info() ;

  return TelLX200::idle() ;
}

double APGTO::siderealTime()
{
	double JD  = ln_get_julian_from_sys ();
	double lng = telLongitude->getValueDouble ();
	return fmod((ln_get_mean_sidereal_time( JD) * 15. + lng + 360.), 360.);  // longitude positive to the East
}

int APGTO::checkSiderealTime( double limit) 
{
  int ret ;
  int error= -1 ;
  // Check if the sidereal time read from the mount is correct 
  double local_sidereal_time= siderealTime() ;

  if(( ret= tel_read_sidereal_time()) != 0) {
    error = ERROR_IN_INFO ;
    logStream (MESSAGE_ERROR) << "APGTO::info could not retrieve sidereal time  " << sendLog;
  }
  logStream (MESSAGE_DEBUG) << "APGTO::checkSiderealTime  local sidereal time, calculated time " 
			    << local_sidereal_time << " mount: "
			    << lst->getValueDouble() 
			    << " difference " << local_sidereal_time- lst->getValueDouble()<<sendLog;
	
  if( fabs(local_sidereal_time- lst->getValueDouble()) > limit ) { // usually 30 time seconds
    logStream (MESSAGE_INFO) << "APGTO::checkSiderealTime AP sidereal time off by " << local_sidereal_time- lst->getValueDouble() << sendLog;
    return -1 ;
  } 
  return 0 ;
}

int APGTO::setBasicData()
{
	int ret ;
	struct ln_date utm;
	struct ln_zonedate ltm;
	if ((ret = tel_set_slew_rate(SLEW_RATE_0600)) < 0)
	{
		/* slew rate 2 = 600x, this the slowest */
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting slew rate failed." << sendLog;
		return -1;
	}
	APslew_rate->setValueInteger(600);

	if ((ret = tel_set_move_rate(MOVE_RATE_000100)) < 0)
	{ /* move rate MOVE_RATE_000100 = 1x */
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting tracking mode sidereal failed." << sendLog;
		return -1;
	}
	APmove_rate->setValueInteger(1);
	//if the sidereal time read from the mount is correct then consider it as a warm start 
	if (checkSiderealTime( 1./60.) == 0)
	{
		logStream (MESSAGE_DEBUG) << "APGTO::setBasicData performing warm start due to correct sidereal time" << sendLog;
		return 0 ;
	}

	logStream (MESSAGE_DEBUG) << "APGTO::setBasicData performing cold start due to incorrect sidereal time" << sendLog;

	if (setAPLongFormat() < 0)
	{
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting long format failed" << sendLog;
		return -1;
	}
	if (setAPBackLashCompensation(0,0,0) < 0)
	{
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting back lash compensation failed" << sendLog;
		return -1;
	}
	ln_get_date_from_sys( &utm) ;
	ln_date_to_zonedate(&utm, &ltm, -1 * timezone + 3600 * daylight); // Adds "only" offset to JD and converts back (see DST below)

	if (setAPLocalTime(ltm.hours, ltm.minutes, (int) ltm.seconds) < 0)
	{
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting local time failed" << sendLog;
		return -1;
	}
	if (setCalenderDate(ltm.days, ltm.months, ltm.years) < 0)
	{
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting local date failed" << sendLog;
		return -1;
	}
	// RTS2 counts positive to the East, AP positive to the West
	double APlng = fmod (360. - telLongitude->getValueDouble(), 360.0);
	if (setAPSiteLongitude( APlng) < 0)
	{ // AP mount: positive and only to the to west 
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting site coordinates failed" << sendLog;
		return -1;
	}
	if (setAPSiteLatitude( telLatitude->getValueDouble()) < 0)
	{
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting site coordinates failed" << sendLog;
		return -1;
	}
	if (setAPUTCOffset((int) (timezone / 3600) - daylight) < 0)
	{
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData setting AP UTC offset failed" << sendLog;
		return -1;
	}

	logStream (MESSAGE_DEBUG) << "APGTO::setBasicData performing a cold start" << sendLog;
	if (setAPUnPark() < 0)
	{
		logStream (MESSAGE_ERROR) << "APGTO::setBasicData unparking failed" << sendLog;
		return -1;
	}
	logStream (MESSAGE_DEBUG) << "APGTO::setBasicData unparking (cold start) successful" << sendLog;
  
	return 0 ;
}
// further discussion with Petr required:
// int APGTO::changeMasterState (int new_state)
// {
// 	// do NOT park us during day..
// 	if (
// 		   ((new_state & SERVERD_STATUS_MASK) == SERVERD_SOFT_OFF)
// 		|| ((new_state & SERVERD_STATUS_MASK) == SERVERD_HARD_OFF)
// 		|| ((new_state & SERVERD_STANDBY_MASK) && standbyPark))
// 	{
// 		if ((getState () & TEL_MASK_MOVING) == 0)
// 		  startPark ();
// 	}

// 	if (blockOnStandby->getValueBool () == true)
// 	{
// 		if ((new_state & SERVERD_STATUS_MASK) == SERVERD_SOFT_OFF
// 		  || (new_state & SERVERD_STATUS_MASK) == SERVERD_HARD_OFF
// 		  || (new_state & SERVERD_STANDBY_MASK))
// 			blockMove->setValueBool (true);
// 		else
// 			blockMove->setValueBool (false);
// 	}
// 	// do not call Telescope::changeMasterState()
// 	return Rts2Device::changeMasterState (new_state);
// }
/*!
 * Init mount, connect on given apgto_fd.
 *
 *
 * @return 0 on succes, -1 & set errno otherwise
 */
int APGTO::init ()
{
	int status;
	on_set_HA= 0.;
	force_start= false ;
	time(&slew_start_time) ;
	status = TelLX200::init ();
  
	if (status)
		return status;

	tzset ();

	logStream (MESSAGE_DEBUG) << "timezone " << timezone << " daylight " << daylight << sendLog;

	logStream (MESSAGE_DEBUG) << "APGTO::init RS 232 initialization complete" << sendLog;
	return 0;
}

int APGTO::initValues ()
{
	int ret = -1;

	strcpy (telType, "APGTO");

	Rts2Config *config = Rts2Config::instance ();
	ret = config->loadFile ();
	if (ret)
		return -1;

	telLongitude->setValueDouble (config->getObserver ()->lng);
	telLatitude->setValueDouble (config->getObserver ()->lat);
	telAltitude->setValueDouble (config->getObservatoryAltitude ());

	if(( ret= setBasicData()) != 0)
		return -1 ;
	if (getAPVersionNumber() != 0)
		return -1;
	if (getAPUTCOffset() != 0)
		return -1 ;
	if (tel_read_sidereal_time() != 0)
		return -1 ;

	// Check if the sidereal time read from the mount is correct 
	if (checkSiderealTime (1./120.) != 0)
	{
		logStream (MESSAGE_ERROR) << "initValues sidereal time larger than 1./120, exiting" << sendLog;
		//exit (1) ; // do not go beyond, at least for the moment
	}

	return TelLX200::initValues ();
}

APGTO::~APGTO (void)
{
}

int APGTO::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_APGTO_ASSUME_PARKED:
			assume_parked->setValueBool(true);
			break;
		case OPT_APGTO_FORCE_START:
			force_start= true;
			break;
		case OPT_APGTO_KEEP_HORIZON:
			avoidBellowHorizon->setValueBool (true);
			break;
		default:
			return TelLX200::processOption (in_opt);
	}
	return 0;
}

APGTO::APGTO (int in_argc, char **in_argv):TelLX200 (in_argc,in_argv)
{
	createValue (fixedOffsets, "FIXED_OFFSETS", "fixed (not reseted) offsets, set after first sync", true, RTS2_VALUE_WRITABLE | RTS2_DT_DEGREES);
	fixedOffsets->setValueRaDec (0, 0);
	
	createValue (mount_tracking, "TRACKING", "true: mount is tracking", false);
	createValue (DECaxis_HAcoordinate, "DECXHA", "DEC axis HA coordinate, West/East",true);
	createValue (APslew_rate, "APSLEWRATE", "AP slew rate (1200, 900, 600)", false, RTS2_VALUE_WRITABLE);
	createValue (APmove_rate, "APMOVERATE", "AP move rate (600, 64, 12, 1)", false, RTS2_VALUE_WRITABLE);
	createValue (APutc_offset, "APUTCOFFSET", "AP mount UTC offset", true,  RTS2_DT_RA);
	createValue (APfirmware, "APVERSION", "AP mount firmware revision", true);
	createValue (assume_parked, "ASSUME_PARKED", "true check initial position",    false);
	createValue (collision_detection, "COLLILSION_DETECTION", "true: mount stop if it collides", true, RTS2_VALUE_WRITABLE);
	createValue (avoidBellowHorizon, "AVOID_HORIZON", "avoid movements bellow horizon", true, RTS2_VALUE_WRITABLE);
	avoidBellowHorizon->setValueBool (false);

	collision_detection->setValueBool (true) ;
	assume_parked->setValueBool (false);

	addOption (OPT_APGTO_ASSUME_PARKED, "parked", 0, "assume a regularly parked mount");
	addOption (OPT_APGTO_FORCE_START, "force_start", 0, "start with wrong declination axis orientation");
	addOption (OPT_APGTO_KEEP_HORIZON, "avoid-horizon", 0, "avoid movements bellow horizon");
}

int main (int argc, char **argv)
{
	APGTO device = APGTO (argc, argv);
	return device.run ();
}
