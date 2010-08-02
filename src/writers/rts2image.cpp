/* 
 * Class which represents image.
 * Copyright (C) 2005-2010 Petr Kubanek <petr@kubanek.net>
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

#include <libnova/libnova.h>

#include "rts2image.h"
#include "imghdr.h"

#include "../utils/libnova_cpp.h"
#include "../utils/rts2config.h"
#include "../utils/timestamp.h"
#include "../utils/utilsfunc.h"
#include "../utilsdb/target.h"

#include "imgdisplay.h"

#include <iomanip>
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

using namespace rts2image;

#if defined(HAVE_LIBJPEG) && HAVE_LIBJPEG == 1
using namespace Magick;
#endif // HAVE_LIBJPEG

// TODO remove this once Libnova 0.13.0 becomes mainstream
#if !HAVE_DECL_LN_GET_HELIOCENTRIC_TIME_DIFF
double ln_get_heliocentric_time_diff (double JD, struct ln_equ_posn *object)
{
	struct ln_nutation nutation;
	struct ln_helio_posn earth;

	ln_get_nutation (JD, &nutation);
	ln_get_earth_helio_coords (JD, &earth);

	double theta = ln_deg_to_rad (ln_range_degrees (earth.L + 180));
	double ra = ln_deg_to_rad (object->ra);
	double dec = ln_deg_to_rad (object->dec);
	double c_dec = cos (dec);
	double obliq = ln_deg_to_rad (nutation.ecliptic);

	/* L.Binnendijk Properties of Double Stars, Philadelphia, University of Pennselvania Press, pp. 228-232, 1960 */
	return -0.0057755 * earth.R * (
		cos (theta) * cos (ra) * c_dec
		+ sin (theta) * (sin (obliq) * sin (dec) + cos (obliq) * c_dec * sin (ra)));
}
#endif

void Rts2Image::initData ()
{
	exposureLength = nan ("f");

	loadHeader = false;
	verbose = true;
	
	setFileName (NULL);
	setFitsFile (NULL);
	targetId = -1;
	targetIdSel = -1;
	targetType = TYPE_UNKNOW;
	targetName = NULL;
	imgId = -1;
	obsId = -1;
	cameraName = NULL;
	mountName = NULL;
	focName = NULL;
	filter_i = -1;
	filter = NULL;
	average = 0;
	stdev = 0;
	bg_stdev = 0;
	min = max = mean = 0;
	imageType = RTS2_DATA_USHORT;
	sexResults = NULL;
	sexResultNum = 0;
	focPos = -1;
	signalNoise = 17;
	getFailed = 0;
	expNum = 1;

	pos_astr.ra = nan ("f");
	pos_astr.dec = nan ("f");
	ra_err = nan ("f");
	dec_err = nan ("f");
	img_err = nan ("f");

	isAcquiring = 0;

	total_rotang = nan ("f");

	shutter = SHUT_UNKNOW;

	flags = 0;

	xoa = nan ("f");
	yoa = nan ("f");

	mnt_flip = 0;
}


Rts2Image::Rts2Image ():Rts2FitsFile ()
{
	initData ();
	flags = IMAGE_KEEP_DATA;
}

Rts2Image::Rts2Image (Rts2Image * in_image):Rts2FitsFile (in_image)
{
	flags = in_image->flags;
	filter_i = in_image->filter_i;
	filter = in_image->filter;
	in_image->filter = NULL;
	exposureLength = in_image->exposureLength;
	channels = in_image->channels;
	imageType = in_image->imageType;
	in_image->channels.clear ();
	focPos = in_image->focPos;
	signalNoise = in_image->signalNoise;
	getFailed = in_image->getFailed;
	average = in_image->average;
	stdev = in_image->stdev;
	bg_stdev = in_image->bg_stdev;
	min = in_image->min;
	max = in_image->max;
	mean = in_image->mean;
	isAcquiring = in_image->isAcquiring;
	total_rotang = in_image->total_rotang;

	targetId = in_image->targetId;
	targetIdSel = in_image->targetIdSel;
	targetType = in_image->targetType;
	targetName = in_image->targetName;
	in_image->targetName = NULL;
	obsId = in_image->obsId;
	imgId = in_image->imgId;
	cameraName = in_image->cameraName;
	in_image->cameraName = NULL;
	mountName = in_image->mountName;
	in_image->mountName = NULL;
	focName = in_image->focName;
	in_image->focName = NULL;

	pos_astr.ra = in_image->pos_astr.ra;
	pos_astr.dec = in_image->pos_astr.dec;
	ra_err = in_image->ra_err;
	dec_err = in_image->dec_err;
	img_err = in_image->img_err;

	sexResults = in_image->sexResults;
	in_image->sexResults = NULL;
	sexResultNum = in_image->sexResultNum;

	shutter = in_image->getShutter ();

	// other image will be saved!
	flags = in_image->flags;
	in_image->flags &= ~IMAGE_SAVE;

	xoa = in_image->xoa;
	yoa = in_image->yoa;

	mnt_flip = in_image->mnt_flip;
}

Rts2Image::Rts2Image (const struct timeval *in_exposureStart):Rts2FitsFile (in_exposureStart)
{
	initData ();
	flags = IMAGE_KEEP_DATA;
	writeExposureStart ();
}

Rts2Image::Rts2Image (const struct timeval *in_exposureStart, float in_img_exposure):Rts2FitsFile (in_exposureStart)
{
	initData ();
	writeExposureStart ();
	exposureLength = in_img_exposure;
}

Rts2Image::Rts2Image (long in_img_date, int in_img_usec, float in_img_exposure):Rts2FitsFile ()
{
	struct timeval tv;
	initData ();
	flags |= IMAGE_NOT_SAVE;
	tv.tv_sec = in_img_date;
	tv.tv_usec = in_img_usec;
	setExposureStart (&tv);
	exposureLength = in_img_exposure;
}

Rts2Image::Rts2Image (char *_filename, const struct timeval *in_exposureStart):Rts2FitsFile (in_exposureStart)
{
	initData ();

	createImage (_filename);
	writeExposureStart ();
}

Rts2Image::Rts2Image (const char *in_expression, int in_expNum, const struct timeval *in_exposureStart, Rts2Conn * in_connection):Rts2FitsFile (in_exposureStart)
{
	initData ();
	setCameraName (in_connection->getName ());
	expNum = in_expNum;

	createImage (expandPath (in_expression));
	writeExposureStart ();
}

Rts2Image::Rts2Image (Rts2Target * currTarget, rts2core::Rts2DevClientCamera * camera, const struct timeval *in_exposureStart):Rts2FitsFile (in_exposureStart)
{
	std::string in_filename;

	initData ();

	targetId = currTarget->getTargetID ();
	targetIdSel = currTarget->getObsTargetID ();
	targetType = currTarget->getTargetType ();
	obsId = currTarget->getObsId ();
	imgId = currTarget->getNextImgId ();
	setExposureStart (in_exposureStart);

	setCameraName (camera->getName ());

	mountName = NULL;
	focName = NULL;

	isAcquiring = currTarget->isAcquiring ();
	if (isAcquiring)
	{
		// put acqusition images to acqusition que
		in_filename = expandPath ("%b/acqusition/que/%c/%f");
	}
	else
	{
		in_filename = expandPath (Rts2Config::instance ()->observatoryQuePath ());
	}

	createImage (in_filename);

	writeExposureStart ();

	setValue ("TARGET", targetId, "target id");
	setValue ("TARSEL", targetIdSel, "selector target id");
	setValue ("TARTYPE", targetType, "target type");
	setValue ("OBSID", obsId, "observation id");
	setValue ("IMGID", imgId, "image id");
	setValue ("PROC", 0, "image processing status");

	if (currTarget->getTargetName ())
	{
		targetName = new char[strlen (currTarget->getTargetName ()) + 1];
		strcpy (targetName, currTarget->getTargetName ());
		setValue ("OBJECT", currTarget->getTargetName (), "target name");
	}
	else
	{
		setValue ("OBJECT", "(null)", "target name was null");
	}

	setValue ("CCD_NAME", camera->getName (), "camera name");

	setEnvironmentalValues ();

	currTarget->writeToImage (this, getExposureJD ());
}


Rts2Image::Rts2Image (const char *_filename, bool _verbose, bool readOnly):Rts2FitsFile ()
{
	initData ();

	loadHeader = true;
	verbose = _verbose;

	setFileName (_filename);
}

Rts2Image::~Rts2Image (void)
{
	saveImage ();

	for (std::map <int, TableData *>::iterator iter = arrayGroups.begin (); iter != arrayGroups.end ();)
	{
		delete iter->second;
		arrayGroups.erase (iter++);
	}

	delete[]targetName;
	delete[]cameraName;
	delete[]mountName;
	delete[]focName;

	imageType = RTS2_DATA_USHORT;
	delete[]filter;
	if (sexResults)
		free (sexResults);
}

std::string Rts2Image::expandVariable (char expression)
{
	switch (expression)
	{
		case 'b':
			return getImageBase ();
		case 'c':
			return getCameraName ();
		case 'E':
			return getExposureLengthString ();
		case 'F':
			return getFilter ();
		case 'f':
			return getOnlyFileName ();
		case 'i':
			return getImgIdString ();
		case 'o':
			return getObsString ();
		case 'p':
			return getProcessingString ();
		case 'T':
			return getTargetString ();
		case 't':
			return getTargetSelString ();
		case 'n':
			return getExposureNumberString ();
		default:
			return rts2core::Expander::expandVariable (expression);
	}
}

std::string Rts2Image::expandVariable (std::string expression)
{
	std::string ret;
	char valB[200];

	try
	{
		getValue (expression.c_str (), valB, 200, NULL, true);
	}
	catch (rts2core::Error &er)
	{
		return rts2core::Expander::expandVariable (expression);
	}
	return std::string (valB);
}

int Rts2Image::createImage ()
{
	if (createFile ())
		return -1;

	flags = IMAGE_NOT_SAVE;
	shutter = SHUT_UNKNOW;

	fits_create_hdu (getFitsFile (), &fits_status);
	if (fits_status)
	{
		logStream (MESSAGE_ERROR) << "cannot create primary HDU: " << getFitsErrors () << sendLog;
		return -1;
	}

	fits_write_key_log (getFitsFile (), "SIMPLE", 1, "conform to FITS standard", &fits_status);
	fits_write_key_lng (getFitsFile (), "BITPIX", 16, "unsigned short data", &fits_status);
	fits_write_key_lng (getFitsFile (), "NAXIS", 0, "number of axes", &fits_status);
	fits_write_key_log (getFitsFile (), "EXTEND", 1, "this is FITS with extensions", &fits_status);
	if (fits_status)
	{
		logStream (MESSAGE_ERROR) << "cannot write keys to primary HDU: " << getFitsErrors () << sendLog;
		return -1;
	}

	// add history
	writeHistory ("Created with RTS2 version " VERSION " build on " __DATE__ " " __TIME__ ".");

	logStream (MESSAGE_DEBUG) << "creating image " << getFileName () << sendLog;

	flags = IMAGE_SAVE;
	return 0;
}

int Rts2Image::createImage (std::string in_filename)
{
	setFileName (in_filename.c_str ());

	return createImage ();
}

int Rts2Image::createImage (char *in_filename)
{
	setFileName (in_filename);

	return createImage ();
}

void Rts2Image::openImage (const char *_filename, bool readOnly)
{
	if (_filename)
		setFileName (_filename);

	openFile (getFileName (), readOnly);

	if (readOnly == false)
		flags |= IMAGE_SAVE;
	if (loadHeader)
		getHeaders ();
}

void Rts2Image::getHeaders ()
{
	struct timeval tv;

	getValue ("CTIME", tv.tv_sec, verbose);
	getValue ("USEC", tv.tv_usec, verbose);
	setExposureStart (&tv);
	// if EXPTIM fails..
	try
	{
		getValue ("EXPTIME", exposureLength, false);
	}
	catch (rts2core::Error)
	{
		getValue ("EXPOSURE", exposureLength, verbose);
	}

	cameraName = new char[DEVICE_NAME_SIZE + 1];
	getValue ("CCD_NAME", cameraName, DEVICE_NAME_SIZE, "UNK");

	mountName = new char[DEVICE_NAME_SIZE + 1];
	getValue ("MNT_NAME", mountName, DEVICE_NAME_SIZE, "UNK");

	focName = new char[DEVICE_NAME_SIZE + 1];
	getValue ("FOC_NAME", focName, DEVICE_NAME_SIZE, "UNK");

	focPos = -1;
	getValue ("FOC_POS", focPos, false);

	getValue ("CAM_FILT", filter_i, verbose);

	filter = new char[5];
	getValue ("FILTER", filter, 5, "UNK", verbose);
	getValue ("AVERAGE", average, verbose);
	getValue ("STDEV", stdev, false);
	getValue ("BGSTDEV", bg_stdev, false);
	getValue ("RA_ERR", ra_err, false);
	getValue ("DEC_ERR", dec_err, false);
	getValue ("POS_ERR", img_err, false);
	// astrometry get !!
	getValue ("CRVAL1", pos_astr.ra, false);
	getValue ("CRVAL2", pos_astr.dec, false);
	// xoa..
	xoa = yoa = 0;
	getValue ("CAM_XOA", xoa, false);
	getValue ("CAM_YOA", yoa, false);

	mnt_flip = 0;
	getValue ("MNT_FLIP", mnt_flip, false);
}

void Rts2Image::getTargetHeaders ()
{
	// get info..
	getValue ("TARGET", targetId, verbose);
	getValue ("TARSEL", targetIdSel, verbose);
	getValue ("TARTYPE", targetType, verbose);
	targetName = new char[FLEN_VALUE];
	getValue ("OBJECT", targetName, FLEN_VALUE, NULL, verbose);
	getValue ("OBSID", obsId, verbose);
	getValue ("IMGID", imgId, verbose);
}

void Rts2Image::setTargetHeaders (int _tar_id, int _obs_id, int _img_id, char _obs_subtype)
{
	targetId = _tar_id;
	targetIdSel = _tar_id;
	targetType = _obs_subtype;
	targetName = NULL;
	obsId = _obs_id;
	imgId = _img_id;
}

int Rts2Image::closeFile ()
{
	if (shouldSaveImage () && getFitsFile ())
	{
		try
		{
			// save astrometry error
			if (!isnan (ra_err) && !isnan (dec_err))
			{
				setValue ("RA_ERR", ra_err, "RA error in position");
				setValue ("DEC_ERR", dec_err, "DEC error in position");
				setValue ("POS_ERR", getAstrometryErr (), "error in position");
			}
			if (!isnan (total_rotang))
			{
				setValue ("ROTANG", total_rotang, "Image rotang over X axis");
			}
			// save array data
			for (std::map <int, TableData *>::iterator iter = arrayGroups.begin (); iter != arrayGroups.end ();)
			{
				writeConnArray (iter->second);
				delete iter->second;
				arrayGroups.erase (iter++);
			}

			moveHDU (1);
			setCreationDate ();
		}
		catch (rts2core::Error &er)
		{
			logStream (MESSAGE_WARNING) << "error saving " << getAbsoluteFileName () << ":" << er << sendLog;
		}
	}
	return Rts2FitsFile::closeFile ();
}

int Rts2Image::toQue ()
{
	return renameImageExpand (Rts2Config::instance ()->observatoryQuePath ());
}

int Rts2Image::toAcquisition ()
{
	return renameImageExpand (Rts2Config::instance ()->observatoryAcqPath ());
}

int Rts2Image::toArchive ()
{
	return renameImageExpand (Rts2Config::instance ()->observatoryArchivePath ());
}

// move to dark images area..
int Rts2Image::toDark ()
{
	return renameImageExpand (Rts2Config::instance ()->observatoryDarkPath ());
}

int Rts2Image::toFlat ()
{
	return renameImageExpand (Rts2Config::instance ()->observatoryFlatPath ());
}

int Rts2Image::toMasterFlat ()
{
	return renameImageExpand ("%b/flat/%c/master/%f");
}

int Rts2Image::toTrash ()
{
	return renameImageExpand (Rts2Config::instance ()->observatoryTrashPath ());
}

img_type_t Rts2Image::getImageType ()
{
	char t_type[50];
	try
	{
		getValue ("IMAGETYP", t_type, 50, NULL, true);
	}
	catch (rts2core::Error &er)
	{
		return IMGTYPE_UNKNOW;
	}
	if (!strcmp (t_type, "flat"))
		return IMGTYPE_FLAT;
	if (!strcmp (t_type, "dark"))
	  	return IMGTYPE_DARK;
	return IMGTYPE_OBJECT;
}

int Rts2Image::renameImage (const char *new_filename)
{
	int ret = 0;
	if (strcmp (new_filename, getFileName ()))
	{
		saveImage ();
		ret = mkpath (new_filename, 0777);
		if (ret)
			return ret;
		ret = rename (getFileName (), new_filename);
		if (!ret)
		{
			openImage (new_filename);
		}
		else
		{
			// it is "problem" with external device..
			if (errno == EXDEV)
			{
				ret = copyImage (new_filename);
				if (ret)
				{
					logStream (MESSAGE_ERROR) << "Cannot copy image to external directory: " << ret << sendLog;
				}
				else
				{
					unlink (getFileName ());
					openImage (new_filename);
				}
			}
			else
			{
				logStream (MESSAGE_ERROR) << "Cannot move " << getFileName () << " to " << new_filename << ": " << strerror (errno) << sendLog;
			}
		}
	}
	return ret;
}

int Rts2Image::renameImageExpand (std::string new_ex)
{
	std::string new_filename;

	if (!getFileName ())
		return -1;

	new_filename = expandPath (new_ex);
	return renameImage (new_filename.c_str ());
}

int Rts2Image::copyImage (const char *copy_filename)
{
	int ret = saveImage ();
	if (ret)
		return ret;
	ret = mkpath (copy_filename, 0777);
	if (ret)
		return ret;

	int fd_from = open (getAbsoluteFileName (), O_RDONLY);
	if (fd_from == -1)
	{
		logStream (MESSAGE_ERROR) << "Cannot open " << getAbsoluteFileName () << ": " << strerror (errno) << sendLog;
		return -1;
	}

	struct stat stb;
	ret = fstat (fd_from, &stb);
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "Cannot get file mode " << getAbsoluteFileName () << ": " << strerror (errno) << sendLog;
		return -1;
	}

	int fd_to = open (copy_filename, O_WRONLY | O_CREAT | O_TRUNC, stb.st_mode);
	if (fd_to == -1)
	{
		logStream (MESSAGE_ERROR) << "Cannot create " << copy_filename << ": " << strerror (errno) << sendLog;
		close (fd_from);
		return -1;
	}

	char buf[4086];
	while (true)
	{
		ret = read (fd_from, buf, 4086);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			logStream (MESSAGE_ERROR) << "Cannot read from file: " << strerror (errno) << sendLog;
			break;
		}
		if (ret == 0)
		{
			close (fd_to);
			close (fd_from);
			return 0;
		}
		int wl;
		while (ret > 0)
		{
			wl = write (fd_to, buf, ret);
			if (wl < 0)
			{
				logStream (MESSAGE_ERROR) << "Cannot write to target file: " << strerror (errno) << sendLog;
				break;
			}
			ret -= wl;
		}
		if (wl < 0)
			break;
	}
	close (fd_to);
	close (fd_from);
	return -1;
}

int Rts2Image::copyImageExpand (std::string copy_ex)
{
	std::string copy_filename;

	if (!getFileName ())
		return -1;

	copy_filename = expandPath (copy_ex);
	return copyImage (copy_filename.c_str ());
}

int Rts2Image::symlinkImage (const char *link_filename)
{
	int ret;

	if (!getFileName ())
		return -1;

	ret = mkpath (link_filename, 0777);
	if (ret)
		return ret;
	return symlink (getFileName (), link_filename);
}

int Rts2Image::symlinkImageExpand (std::string link_ex)
{
	std::string link_filename;

	if (!getFileName ())
		return -1;

	link_filename = expandPath (link_ex);
	return symlinkImage (link_filename.c_str ());
}

int Rts2Image::linkImage (const char *link_filename)
{
	int ret;

	if (!getFileName ())
		return -1;

	ret = mkpath (link_filename, 0777);
	if (ret)
		return ret;
	return link (getFileName (), link_filename);
}

int Rts2Image::linkImageExpand (std::string link_ex)
{
	std::string link_filename;

	if (!getFileName ())
		return -1;

	link_filename = expandPath (link_ex);
	return linkImage (link_filename.c_str ());
}

/*int Rts2Image::saveImageData (const char *save_filename, unsigned short *in_data)
{
	fitsfile *fp;
	fits_status = 0;

	fits_open_file (&fp, save_filename, READWRITE, &fits_status);
	for (Channels::iterator iter = channels.begin (); iter != channels.end (); iter++)
	{
		fits_write_img (fp, TUSHORT, 1, iter->getNPixels (), in_data, &fits_status);
	}
	fits_close_file (fp, &fits_status);

	return 0;
}*/

int Rts2Image::writeExposureStart ()
{
	setValue ("CTIME", getCtimeSec (), "exposure start (seconds since 1.1.1970)");
	setValue ("USEC", getCtimeUsec (), "exposure start micro seconds");
	time_t tim = getCtimeSec ();
	setValue ("JD", getExposureJD (), "exposure JD");
	setValue ("DATE-OBS", &tim, getCtimeUsec (), "start of exposure");
	return 0;
}

char * Rts2Image::getImageBase ()
{
	static char buf[12];
	strcpy (buf, "/images/");
	return buf;
}

void Rts2Image::setValue (const char *name, bool value, const char *comment)
{
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	int i_val = value ? 1 : 0;
	fits_update_key (getFitsFile (), TLOGICAL, (char *) name, &i_val, (char *) comment, &fits_status);
	flags |= IMAGE_SAVE;
	return fitsStatusSetValue (name, true);
}

void Rts2Image::setValue (const char *name, int value, const char *comment)
{
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	fits_update_key (getFitsFile (), TINT, (char *) name, &value, (char *) comment, &fits_status);
	flags |= IMAGE_SAVE;
	fitsStatusSetValue (name, true);
}

void Rts2Image::setValue (const char *name, long value, const char *comment)
{
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	fits_update_key (getFitsFile (), TLONG, (char *) name, &value, (char *) comment, &fits_status);
	flags |= IMAGE_SAVE;
	fitsStatusSetValue (name);
}

void Rts2Image::setValue (const char *name, float value, const char *comment)
{
	float val = value;
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	if (isnan (val) || isinf (val))
		val = FLOATNULLVALUE;
	fits_update_key (getFitsFile (), TFLOAT, (char *) name, &val, (char *) comment, &fits_status);
	flags |= IMAGE_SAVE;
	fitsStatusSetValue (name);
}

void Rts2Image::setValue (const char *name, double value, const char *comment)
{
	double val = value;
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	if (isnan (val) || isinf (val))
		val = DOUBLENULLVALUE;
	fits_update_key (getFitsFile (), TDOUBLE, (char *) name, &val, (char *) comment, &fits_status);
	flags |= IMAGE_SAVE;
	fitsStatusSetValue (name);
}

void Rts2Image::setValue (const char *name, char value, const char *comment)
{
	char val[2];
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	val[0] = value;
	val[1] = '\0';
	fits_update_key (getFitsFile (), TSTRING, (char *) name, (void *) val, (char *) comment, &fits_status);
	flags |= IMAGE_SAVE;
	fitsStatusSetValue (name);
}

void Rts2Image::setValue (const char *name, const char *value, const char *comment)
{
	// we will not save null values
	if (!value)
		return;
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	fits_update_key_longstr (getFitsFile (), (char *) name, (char *) value, (char *) comment,
		&fits_status);
	flags |= IMAGE_SAVE;
	fitsStatusSetValue (name);
}

void Rts2Image::setValue (const char *name, time_t * sec, long usec, const char *comment)
{
	char buf[25];
	struct tm t_tm;
	gmtime_r (sec, &t_tm);
	strftime (buf, 25, "%Y-%m-%dT%H:%M:%S.", &t_tm);
	snprintf (buf + 20, 4, "%03li", usec / 1000);
	setValue (name, buf, comment);
}

void Rts2Image::setCreationDate (fitsfile * out_file)
{
	fitsfile *curr_ffile = getFitsFile ();

	if (out_file)
	{
		setFitsFile (out_file);
	}

	struct timeval now;
	gettimeofday (&now, NULL);
	setValue ("DATE", &(now.tv_sec), now.tv_usec, "creation date");

	if (out_file)
	{
		setFitsFile (curr_ffile);
	}
}

void Rts2Image::getValue (const char *name, bool & value, bool required, char *comment)
{
	if (!getFitsFile ())
		openImage (NULL, true);

	int i_val;
	fits_read_key (getFitsFile (), TLOGICAL, (char *) name, (void *) &i_val, comment, &fits_status);
	value = i_val == TRUE;
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValue (const char *name, int &value, bool required, char *comment)
{
	if (!getFitsFile ())
		openImage (NULL, true);

	fits_read_key (getFitsFile (), TINT, (char *) name, (void *) &value, comment, &fits_status);
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValue (const char *name, long &value, bool required, char *comment)
{
	if (!getFitsFile ())
		openImage (NULL, true);
	
	fits_read_key (getFitsFile (), TLONG, (char *) name, (void *) &value, comment,
		&fits_status);
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValue (const char *name, float &value, bool required, char *comment)
{
	if (!getFitsFile ())
		openImage (NULL, true);
	
	fits_read_key (getFitsFile (), TFLOAT, (char *) name, (void *) &value, comment,	&fits_status);
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValue (const char *name, double &value, bool required, char *comment)
{
	if (!getFitsFile ())
		openImage (NULL, true);
	
	fits_read_key (getFitsFile (), TDOUBLE, (char *) name, (void *) &value, comment, &fits_status);
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValue (const char *name, char &value, bool required, char *comment)
{
	static char val[FLEN_VALUE];
	if (!getFitsFile ())
		openImage (NULL, true);

	fits_read_key (getFitsFile (), TSTRING, (char *) name, (void *) val, comment,
		&fits_status);
	value = *val;
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValue (const char *name, char *value, int valLen, const char* defVal, bool required, char *comment)
{
	try
	{
		static char val[FLEN_VALUE];
		if (!getFitsFile ())
			openImage (NULL, true);
	
		fits_read_key (getFitsFile (), TSTRING, (char *) name, (void *) val, comment,
			&fits_status);
		strncpy (value, val, valLen);
		value[valLen - 1] = '\0';
		fitsStatusGetValue (name, true);
	}
	catch (rts2core::Error &er)
	{
		if (defVal)
		{
			strncpy (value, defVal, valLen);
			return;
		}
		if (required)
			throw (er);
	}
}

void Rts2Image::getValue (const char *name, char **value, int valLen, bool required, char *comment)
{
	if (!getFitsFile ())
		openImage (NULL, true);

	fits_read_key_longstr (getFitsFile (), (char *) name, value, comment, &fits_status);
	fitsStatusGetValue (name, required);
}

double Rts2Image::getValue (const char *name)
{
	if (!getFitsFile ())
		openImage (NULL, true);

	double ret;
	fits_read_key_dbl (getFitsFile (), (char *) name, &ret, NULL, &fits_status);
	if (fits_status != 0)
	{
		fits_status = 0;
		throw KeyNotFound (this, name);
	}
	return ret;
}

void Rts2Image::getValues (const char *name, int *values, int num, bool required, int nstart)
{
	if (!getFitsFile ())
		throw ErrorOpeningFitsFile (getFileName ());

	int nfound;
	fits_read_keys_log (getFitsFile (), (char *) name, nstart, num, values, &nfound,
		&fits_status);
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValues (const char *name, long *values, int num, bool required, int nstart)
{
	if (!getFitsFile ())
		throw ErrorOpeningFitsFile (getFileName ());

	int nfound;
	fits_read_keys_lng (getFitsFile (), (char *) name, nstart, num, values, &nfound,
		&fits_status);
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValues (const char *name, double *values, int num, bool required, int nstart)
{
	if (!getFitsFile ())
		throw ErrorOpeningFitsFile (getFileName ());

	int nfound;
	fits_read_keys_dbl (getFitsFile (), (char *) name, nstart, num, values, &nfound,
		&fits_status);
	fitsStatusGetValue (name, required);
}

void Rts2Image::getValues (const char *name, char **values, int num, bool required, int nstart)
{
	if (!getFitsFile ())
		throw ErrorOpeningFitsFile (getFileName ());
	
	int nfound;
	fits_read_keys_str (getFitsFile (), (char *) name, nstart, num, values, &nfound,
		&fits_status);
	fitsStatusGetValue (name, required);
}

int Rts2Image::writeImgHeader (struct imghdr *im_h)
{
	writePhysical (ntohs (im_h->x), ntohs (im_h->y), ntohs (im_h->binnings[0]), ntohs (im_h->binnings[1]));
	return 0;
}

void Rts2Image::writePhysical (int x, int y, int bin_x, int bin_y)
{
	setValue ("LTV1", -1 * (double) x, "image beginning - detector X coordinate");
	setValue ("LTM1_1", ((double) 1) / bin_x, "delta along X axis");
	setValue ("LTV2", -1 * (double) y, "image beginning - detector Y coordinate");
	setValue ("LTM2_2", ((double) 1) / bin_y, "delta along Y axis");
}

void Rts2Image::writeMetaData (struct imghdr *im_h)
{
	if (!getFitsFile () || !(flags & IMAGE_SAVE))
	{
		#ifdef DEBUG_EXTRA
		logStream (MESSAGE_DEBUG) << "not saving data " << getFitsFile () << " "
			<< (flags & IMAGE_SAVE) << sendLog;
		#endif					 /* DEBUG_EXTRA */
		return;
	}

	filter_i = ntohs (im_h->filter);

	setValue ("CAM_FILT", filter_i, "filter used for image");
	setValue ("SHUTTER", ntohs (im_h->shutter), "shutter state (1 - open, 2 - closed, 3 - synchro)");
	// dark images don't need to wait till imgprocess will pick them up for reprocessing
	switch (ntohs (im_h->shutter))
	{
		case 0:
			shutter = SHUT_OPENED;
			break;
		case 1:
			shutter = SHUT_CLOSED;
			break;
		default:
			shutter = SHUT_UNKNOW;
	}
}

int Rts2Image::writeData (char *in_data, char *fullTop, int nchan)
{
	struct imghdr *im_h = (struct imghdr *) in_data;
	int ret;

	average = 0;
	stdev = 0;
	bg_stdev = 0;

	// we have to copy data to FITS anyway, so let's do it right now..
	if (ntohs (im_h->naxes) != 2)
	{
		logStream (MESSAGE_ERROR) << "Rts2Image::writeDate not 2D image " << ntohs (im_h->naxes) << sendLog;
		return -1;
	}
	flags |= IMAGE_SAVE;
	imageType = ntohs (im_h->data_type);

	long sizes[2];
	sizes[0] = ntohl (im_h->sizes[0]);
	sizes[1] = ntohl (im_h->sizes[1]);

	long dataSize = (fullTop - in_data) - sizeof (struct imghdr);
	char *pixelData = in_data + sizeof (struct imghdr);

	Channel *ch;

	if (flags & IMAGE_KEEP_DATA)
	{
		ch = new Channel (pixelData, dataSize, 2, sizes);
	}
	else
	{
		ch = new Channel (pixelData, 2, sizes);
	}

	channels.push_back (ch);

	computeStatistics ();

	if (!getFitsFile () || !(flags & IMAGE_SAVE))
	{
		#ifdef DEBUG_EXTRA
		logStream (MESSAGE_DEBUG) << "not saving data " << getFitsFile () << " "
			<< (flags & IMAGE_SAVE) << sendLog;
		#endif					 /* DEBUG_EXTRA */
		return 0;
	}

	// either put it as a new extension, or keep it in primary..

	if (nchan == 1)
	{
		if (imageType == RTS2_DATA_SBYTE)
		{
			fits_resize_img (getFitsFile (), RTS2_DATA_BYTE, 2, sizes, &fits_status);
		}
		else
		{
			fits_resize_img (getFitsFile (), imageType, 2, sizes, &fits_status);
		}
		if (fits_status)
		{
			logStream (MESSAGE_ERROR) << "cannot resize image: " << getFitsErrors ()
				<< "imageType " << imageType << sendLog;
			return -1;
		}
	}
	else
	{
		if (imageType == RTS2_DATA_SBYTE)
		{
			fits_create_img (getFitsFile (), RTS2_DATA_BYTE, 2, sizes, &fits_status);
		}
		else
		{
			fits_create_img (getFitsFile (), imageType, 2, sizes, &fits_status);
		}
		if (fits_status)
		{
			logStream (MESSAGE_ERROR) << "cannot create new image: " << getFitsErrors ()
				<< "imageType " << imageType << sendLog;
			return -1;
		}
	}

	ret = writeImgHeader (im_h);

	long pixelSize = dataSize / getPixelByteSize ();
	switch (imageType)
	{
		case RTS2_DATA_BYTE:
			fits_write_img_byt (getFitsFile (), 0, 1, pixelSize, (unsigned char *) pixelData, &fits_status);
			break;
		case RTS2_DATA_SHORT:
			fits_write_img_sht (getFitsFile (), 0, 1, pixelSize, (int16_t *) pixelData, &fits_status);
			break;
		case RTS2_DATA_LONG:
			fits_write_img_lng (getFitsFile (), 0, 1, pixelSize, (long int *) pixelData, &fits_status);
			break;
		case RTS2_DATA_LONGLONG:
			fits_write_img_lnglng (getFitsFile (), 0, 1, pixelSize, (LONGLONG *) pixelData, &fits_status);
			break;
		case RTS2_DATA_FLOAT:
			fits_write_img_flt (getFitsFile (), 0, 1, pixelSize, (float *) pixelData, &fits_status);
			break;
		case RTS2_DATA_DOUBLE:
			fits_write_img_dbl (getFitsFile (), 0, 1, pixelSize, (double *) pixelData, &fits_status);
			break;
		case RTS2_DATA_SBYTE:
			fits_write_img_sbyt (getFitsFile (), 0, 1, pixelSize, (signed char *) pixelData, &fits_status);
			break;
		case RTS2_DATA_USHORT:
			fits_write_img_usht (getFitsFile (), 0, 1, pixelSize, (short unsigned int *) pixelData, &fits_status);
			break;
		case RTS2_DATA_ULONG:
			fits_write_img_ulng (getFitsFile (), 0, 1, pixelSize, (unsigned long *) pixelData, &fits_status);
			break;
		default:
			logStream (MESSAGE_ERROR) << "Unknow imageType " << imageType << sendLog;
			return -1;
	}
	if (fits_status)
	{
		logStream (MESSAGE_ERROR) << "cannot write data: " << getFitsErrors () << sendLog;
		return -1;
	}
	setValue ("AVERAGE", average, "average value of image");
	setValue ("STDEV", stdev, "standard deviation value of image");
	setValue ("BGSTDEV", stdev, "standard deviation value of background");
	#ifdef DEBUG_EXTRA
	logStream (MESSAGE_DEBUG) << "writeDate returns " << ret << sendLog;
	#endif						 /* DEBUG_EXTRA */
	return ret;
}

void Rts2Image::getHistogram (long *histogram, long nbins)
{
	memset (histogram, 0, nbins * sizeof(int));
	int bins;
	if (channels.size () == 0)
		loadChannels ();

	switch (imageType)
	{
		case RTS2_DATA_USHORT:
			bins = 65535 / nbins;
			for (Channels::iterator iter = channels.begin (); iter != channels.end (); iter++)
			{
				for (uint16_t *d = (uint16_t *)((*iter)->getData ()); d < ((uint16_t *)((*iter)->getData ())) + ((*iter)->getNPixels ()); d++)
				{
					histogram[*d / bins]++;
				}
			}
			break;
		default:
			break;
	}
}

void Rts2Image::getChannelHistogram (int chan, long *histogram, long nbins)
{
	memset (histogram, 0, nbins * sizeof(int));
	int bins;
	if (channels.size () == 0)
		loadChannels ();

	switch (imageType)
	{
		case RTS2_DATA_USHORT:
			bins = 65535 / nbins;
			for (uint16_t *d = (uint16_t *)(channels[chan]->getData ()); d < ((uint16_t *)(channels[chan]->getData ()) + channels[chan]->getNPixels ()); d++)
			{
				histogram[*d / bins]++;
			}
			break;
		default:
			break;
	}
}


template <typename bt> void Rts2Image::getChannelGrayscaleBuffer (int chan, bt * &buf, bt black, float quantiles)
{
	long hist[65535];
	getChannelHistogram (chan, hist, 65535);

	long psum = 0;
	int low = -1;
	int high = -1;

	int i;

	int s = getChannelNPixels (chan);

	// find quantiles
	for (i = 0; i < 65535; i++)
	{
		psum += hist[i];
		if (psum > s * quantiles)
		{
			low = i;
			break;
		}
	}

	if (low == -1)
	{
		low = 65535;
		high = 65535;
	}
	else
	{
		for (; i < 65535; i++)
		{
			psum += hist[i];
			if (psum > s * (1 - quantiles))
			{
				high = i;
				break;
			}
		}
		if (high == -1)
		{
			high = 65535;
		}
	}

	buf = new bt[s];

	long k = 0;

	std::cout << "low " << low << " high " << high << std::endl;

	const void *imageData = getChannelData (chan);

	for (i = 0; i < s; i++)
	{
		bt n;
		uint16_t pix = ((uint16_t *)imageData)[i];
		if (pix <= low)
		{
			n = black;
		}
		else if (pix >= high)
		{
			n = 0;
		}
		else
		{
			// linear scaling
			n = black - black * ((double (pix - low)) / (high - low));
		}
		buf[k++] = n;
	}
}


#if defined(HAVE_LIBJPEG) && HAVE_LIBJPEG == 1
Image Rts2Image::getMagickImage (const char *label, float quantiles, int chan)
{
	unsigned char *buf = NULL;
	try
	{
		getChannelGrayscaleBuffer (chan, buf, (unsigned char) 255);
		Image image = Image (getChannelWidth (chan), getChannelHeight (chan), "K", CharPixel, buf);
		image.font("helvetica");
		image.strokeColor (Color (MaxRGB, MaxRGB, MaxRGB));
		image.fillColor (Color (MaxRGB, MaxRGB, MaxRGB));
		if (label)
			writeLabel (image, 2, image.size ().height () - 2, 20, label);
		delete[] buf;
		return image;
	}
	catch (Exception &ex)
	{
		delete[] buf;
		throw ex;
	}
}

void Rts2Image::writeLabel (Magick::Image &mimage, int x, int y, unsigned int fs, const char *labelText)
{
	mimage.fontPointsize (fs);
	mimage.fillColor (Magick::Color (0, 0, 0, MaxRGB / 2));
	mimage.draw (Magick::DrawableRectangle (x, y - fs - 4, mimage.size (). width () - x - 2, y));

	mimage.fillColor (Magick::Color (MaxRGB, MaxRGB, MaxRGB));
	mimage.draw (Magick::DrawableText (x + 2, y - 3, expand (labelText)));
}

void Rts2Image::writeAsJPEG (std::string expand_str, const char *label, float quantiles)
{
	std::string new_filename = expandPath (expand_str);
	
	int ret = mkpath (new_filename.c_str (), 0777);
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "Cannot create directory for file '" << new_filename << "'" << sendLog;
		throw Exception ("Cannot create directory");
	}

	try {
		Image image = getMagickImage (label, quantiles);
		image.write (new_filename.c_str ());
	}
	catch (Exception &ex)
	{
		logStream (MESSAGE_ERROR) << "Cannot create image " << new_filename << ", " << ex.what () << sendLog;
		throw ex;
	}
}

void Rts2Image::writeAsBlob (Blob &blob, const char * label, float quantiles)
{
	try {
		Image image = getMagickImage (label, quantiles);
		image.write (&blob, "jpeg");
	}
	catch (Exception &ex)
	{
		logStream (MESSAGE_ERROR) << "Cannot create image " << ex.what () << sendLog;
		throw ex;
	}
}

#endif /* HAVE_LIBJPEG */

double Rts2Image::getAstrometryErr ()
{
	struct ln_equ_posn pos2;
	pos2.ra = pos_astr.ra + ra_err;
	pos2.dec = pos_astr.dec + dec_err;
	return ln_get_angular_separation (&pos_astr, &pos2);
}

int Rts2Image::saveImage ()
{
	return closeFile ();
}

int Rts2Image::deleteImage ()
{
	int ret;
	fits_close_file (getFitsFile (), &fits_status);
	setFitsFile (NULL);
	flags &= ~IMAGE_SAVE;
	ret = unlink (getFileName ());
	return ret;
}

void Rts2Image::setMountName (const char *in_mountName)
{
	delete[]mountName;
	mountName = new char[strlen (in_mountName) + 1];
	strcpy (mountName, in_mountName);
	setValue ("MNT_NAME", mountName, "name of mount");
}

void Rts2Image::setCameraName (const char *new_name)
{
	delete[]cameraName;
	cameraName = new char[strlen (new_name) + 1];
	strcpy (cameraName, new_name);
}

std::string Rts2Image::getTargetString ()
{
	std::ostringstream _os;
	_os.fill ('0');
	_os << std::setw (5) << getTargetId ();
	return _os.str ();
}

std::string Rts2Image::getTargetSelString ()
{
	std::ostringstream _os;
	_os.fill ('0');
	_os << std::setw (5) << getTargetIdSel ();
	return _os.str ();
}

std::string Rts2Image::getExposureNumberString ()
{
	std::ostringstream _os;
	_os.fill ('0');
	_os << std::setw (6) << expNum;
	return _os.str ();
}

std::string Rts2Image::getObsString ()
{
	std::ostringstream _os;
	_os.fill ('0');
	_os << std::setw (5) << getObsId ();
	return _os.str ();
}

std::string Rts2Image::getImgIdString ()
{
	std::ostringstream _os;
	_os.fill ('0');
	_os << std::setw (5) << getImgId ();
	return _os.str ();
}

// Rts2Image can be only raw..
std::string Rts2Image::getProcessingString ()
{
	return std::string ("RA");
}

std::string Rts2Image::getExposureLengthString ()
{
	std::ostringstream _os;
	_os << getExposureLength ();
	return _os.str ();
}

void Rts2Image::setFocuserName (const char *in_focuserName)
{
	if (focName)
		delete[]focName;
	focName = new char[strlen (in_focuserName) + 1];
	strcpy (focName, in_focuserName);
	setValue ("FOC_NAME", focName, "name of focuser");
}

void Rts2Image::setFilter (const char *in_filter)
{
	filter = new char[strlen (in_filter) + 1];
	strcpy (filter, in_filter);
	setValue ("FILTER", filter, "camera filter as string");
}

void Rts2Image::computeStatistics ()
{
	if (channels.size () == 0)
		loadChannels ();

	const unsigned short *pixel;
	const unsigned short *fullTop;

	long totalSize = 0;

	// calculate average of all channels..
	for (Channels::iterator iter = channels.begin (); iter != channels.end (); iter++)
	{
		pixel = (const unsigned short *) (*iter)->getData ();
		fullTop = pixel + (*iter)->getNPixels ();
		totalSize += (*iter)->getNPixels ();
		while (pixel < fullTop)
		{
			average += *pixel;
			pixel++;
		}
	}

	int bg_size = 0;

	if (totalSize > 0)
	{
		average /= totalSize;
		// calculate stdev
		for (Channels::iterator iter = channels.begin (); iter != channels.end (); iter++)
		{
			pixel = (const unsigned short *) (*iter)->getData ();
			fullTop = pixel + (*iter)->getNPixels ();
			while (pixel < fullTop)
			{
				long double tmp_s = *pixel - average;
				long double tmp_ss = tmp_s * tmp_s;
				if (fabs (tmp_s) < average / 10)
				{
					bg_stdev += tmp_ss;
					bg_size++;
				}
				stdev += tmp_ss;
				pixel++;
			}
		}
		stdev = sqrt (stdev / totalSize);
		bg_stdev = sqrt (bg_stdev / bg_size);
	}
	else
	{
		average = 0;
		stdev = 0;
	}
}

void Rts2Image::loadChannels ()
{
	// try to load data..
	int anyNull = 0;
	if (!getFitsFile ())
		openImage (NULL, true);

	imageType = 0;
	int hdutype;

	// open all channels
	for (moveHDU (1, &hdutype); fits_status == 0; fits_movrel_hdu (getFitsFile (), 1, &hdutype, &fits_status))
	{
		// first check that it is image
		if (hdutype != IMAGE_HDU)
			continue;

		// check that it has some axis..
		int naxis = 0;
		getValue ("NAXIS", naxis, true);
		if (naxis == 0)
			continue;

		// check it type (bits per pixel)
		int it;
		fits_get_img_equivtype (getFitsFile (), &it, &fits_status);
		if (fits_status)
		{
			logStream (MESSAGE_ERROR) << "cannot retrieve image type: " << getFitsErrors () << sendLog;
			return;
		}
		if (imageType == 0)
		{
			imageType = it;
		}
		else if (imageType != it)
		{
			logStream (MESSAGE_ERROR) << getFileName () << " has extension with different image type, which is not supported" << sendLog;
			return;
		}

		// get its size..
		long sizes[naxis];
		getValues ("NAXIS", sizes, naxis, true);

		long pixelSize = sizes[0];
		for (int i = 1; i < naxis; i++)
			pixelSize *= sizes[i];

		char *imageData = new char[pixelSize * getPixelByteSize ()];
		switch (imageType)
		{
			case RTS2_DATA_BYTE:
				fits_read_img_byt (getFitsFile (), 0, 1, pixelSize, 0, (unsigned char *) imageData, &anyNull, &fits_status);
				break;
			case RTS2_DATA_SHORT:
				fits_read_img_sht (getFitsFile (), 0, 1, pixelSize, 0, (int16_t *) imageData, &anyNull, &fits_status);
				break;
			case RTS2_DATA_LONG:
				fits_read_img_lng (getFitsFile (), 0, 1, pixelSize, 0, (long int *) imageData, &anyNull, &fits_status);
				break;
	
			case RTS2_DATA_LONGLONG:
				fits_read_img_lnglng (getFitsFile (), 0, 1, pixelSize, 0, (LONGLONG *) imageData, &anyNull, &fits_status);
				break;
			case RTS2_DATA_FLOAT:
				fits_read_img_flt (getFitsFile (), 0, 1, pixelSize, 0, (float *) imageData, &anyNull, &fits_status);
				break;
			case RTS2_DATA_DOUBLE:
				fits_read_img_dbl (getFitsFile (), 0, 1, pixelSize, 0, (double *) imageData, &anyNull, &fits_status);
				break;
			case RTS2_DATA_SBYTE:
				fits_read_img_sbyt (getFitsFile (), 0, 1, pixelSize, 0, (signed char *) imageData, &anyNull, &fits_status);
				break;
			case RTS2_DATA_USHORT:
				fits_read_img_usht (getFitsFile (), 0, 1, pixelSize, 0,(short unsigned int *) imageData, &anyNull, &fits_status);
				break;
			case RTS2_DATA_ULONG:
				fits_read_img_ulng (getFitsFile (), 0, 1, pixelSize, 0, (unsigned long *) imageData, &anyNull, &fits_status);
				break;
			default:
				logStream (MESSAGE_ERROR) << "Unknow imageType " << imageType << sendLog;
				delete[] imageData;
				imageType = 0;
				throw ErrorOpeningFitsFile (getFileName ());
		}
		if (fits_status)
		{
			delete[] imageData;
			imageType = 0;
			throw ErrorOpeningFitsFile (getFileName ());
		}
		fitsStatusGetValue ("image loadChannels", true);

		channels.push_back (new Channel (imageData, naxis, sizes));
	}
}

const void* Rts2Image::getChannelData (int chan)
{
	if (channels.size () == 0)
	{
		try
		{
			loadChannels ();
		}
		catch (rts2core::Error er)
		{
			logStream (MESSAGE_ERROR) << er << sendLog;
			return NULL;
		}
	}
	return channels[chan]->getData ();
}

unsigned short * Rts2Image::getChannelDataUShortInt (int chan)
{
	if (getChannelData (0) == NULL)
		return NULL;
	// switch by format
	if (imageType == RTS2_DATA_USHORT)
		return (unsigned short *) getChannelData (0);
	// convert type to ushort int
	return NULL;
}

/*void Rts2Image::setDataUShortInt (unsigned short *in_data, long in_naxis[2])
{
	imageData = (char *) in_data;
	imageType = RTS2_DATA_USHORT;
	naxis[0] = in_naxis[0];
	naxis[1] = in_naxis[1];
	flags |= IMAGE_DONT_DELETE_DATA;
} */

/*int Rts2Image::substractDark (Rts2Image * darkImage)
{
	unsigned short *img_data;
	unsigned short *dark_data;
	if (getWidth () != darkImage->getWidth ()
		|| getHeight () != darkImage->getHeight ())
		return -1;
	img_data = getDataUShortInt ();
	dark_data = darkImage->getDataUShortInt ();
	for (long i = 0; i < (getWidth () * getHeight ());
		i++, img_data++, dark_data++)
	{
		if (*img_data <= *dark_data)
			*img_data = 0;
		else
			*img_data = *img_data - *dark_data;
	}
	return 0;
}*/

int Rts2Image::setAstroResults (double in_ra, double in_dec, double in_ra_err, double in_dec_err)
{
	pos_astr.ra = in_ra;
	pos_astr.dec = in_dec;
	ra_err = in_ra_err;
	dec_err = in_dec_err;
	flags |= IMAGE_SAVE;
	return 0;
}

int Rts2Image::addStarData (struct stardata *sr)
{
	if (sr->F <= 0)				 // flux is not significant..
		return -1;
	// do not take stars on edge..
	if (sr->X == 0 || sr->X == getChannelWidth (0) || sr->Y == 0 || sr->Y == getChannelHeight (0))
		return -1;

	sexResultNum++;
	if (sexResultNum > 1)
	{
		sexResults =
			(struct stardata *) realloc ((void *) sexResults,
			sexResultNum * sizeof (struct stardata));
	}
	else
	{
		sexResults = (struct stardata *) malloc (sizeof (struct stardata));
	}
	sexResults[sexResultNum - 1] = *sr;
	return 0;
}

/* static int
sdcompare (struct stardata *x1, struct stardata *x2)
{
  if (x1->fwhm < x2->fwhm)
	return 1;
  if (x1->fwhm > x2->fwhm)
	return -1;
  return 0;
} */
int Rts2Image::isGoodForFwhm (struct stardata *sr)
{
	return (sr->flags == 0 && sr->F > signalNoise * sr->Fe);
}

double Rts2Image::getFWHM ()
{
	double avg;
	struct stardata *sr;
	int i;
	int numStars;
	if (sexResultNum < 4)
		return nan ("f");

	// qsort (sexResults, sexResultNum, sizeof (struct stardata), sdcompare);
	// get average
	avg = 0;
	numStars = 0;
	sr = sexResults;
	for (i = 0; i < sexResultNum; i++, sr++)
	{
		if (isGoodForFwhm (sr))
		{
			avg += sr->fwhm;
			numStars++;
		}
	}
	avg /= numStars;
	return avg;
}

static int shortcompare (const void *val1, const void *val2)
{
	return (*(unsigned short *) val1 < *(unsigned short *) val2) ? -1 :
	((*(unsigned short *) val1 == *(unsigned short *) val2) ? 0 : 1);
}

unsigned short getShortMean (unsigned short *averageData, int sub)
{
	qsort (averageData, sub, sizeof (unsigned short), shortcompare);
	if (sub % 2)
		return averageData[sub / 2];
	return (averageData[(sub / 2) - 1] + averageData[(sub / 2)]) / 2;
}

/*
int Rts2Image::getCenterRow (long row, int row_width, double &x)
{
	unsigned short *tmp_data;
	tmp_data = getDataUShortInt ();
	unsigned short *rowData;
	unsigned short *averageData;
	unsigned short data_max;
	long i;
	long j;
	// long k;
	long maxI;
	if (!tmp_data)
		return -1;
	// compute average collumn
	rowData = new unsigned short[getWidth ()];
	averageData = new unsigned short[row_width];
	tmp_data += row * getWidth ();
	if (row_width < 2)
	{
		return -1;
	}
	for (j = 0; j < getWidth (); j++)
	{
		for (i = 0; i < row_width; i++)
		{
			averageData[i] = tmp_data[j + i * getWidth ()];
		}
		rowData[j] = getShortMean (averageData, row_width);
	}
	// bin data in row..
	/ i = j = k = 0;
	   while (j < getWidth ())
	   {
	   // last bit..
	   k += row_width;
	   rowData[i] =
	   getShortMean (rowData + j,
	   (k > getWidth ())? (k - getWidth ()) : row_width);
	   i++;
	   j = k;
	   } /
	// decide, which part of slope we are in
	// i hold size of reduced rowData array
	// find maximum..
	data_max = rowData[0];
	maxI = 0;
	for (j = 1; j < i; j++)
	{
		if (rowData[j] > data_max)
		{
			data_max = rowData[j];
			maxI = j;
		}
	}
	// close to left border - left
	if (maxI < (i / 10))
		x = -1;
	// close to right border - right
	else if (maxI > (i - (i / 10)))
		x = getWidth () + 1;
	// take value in middle
	else
		x = (maxI * row_width) + row_width / 2;
	delete[]rowData;
	delete[]averageData;
	return 0;
}

int Rts2Image::getCenterCol (long col, int col_width, double &y)
{
	unsigned short *tmp_data;
	tmp_data = getDataUShortInt ();
	unsigned short *colData;
	unsigned short *averageData;
	unsigned short data_max;
	long i;
	long j;
	// long k;
	long maxI;
	if (!tmp_data)
		return -1;
	// smooth image..
	colData = new unsigned short[getHeight ()];
	averageData = new unsigned short[col_width];
	tmp_data += col * getHeight ();
	if (col_width < 2)
	{
		// special handling..
		return -1;
	}
	for (j = 0; j < getHeight (); j++)
	{
		for (i = 0; i < col_width; i++)
		{
			averageData[i] = tmp_data[j + i * getHeight ()];
		}
		colData[j] = getShortMean (averageData, col_width);
	}
	// bin data in row..
	/ i = j = k = 0;
	   while (j < getHeight ())
	   {
	   // last bit..
	   k += col_width;
	   colData[i] =
	   getShortMean (colData + j,
	   (k > getHeight ())? (k - getHeight ()) : col_width);
	   i++;
	   j = k;
	   }
	 /
	// find gauss on image..
	//ret = findGauss (colData, i, y);
	data_max = colData[0];
	maxI = 0;
	for (j = 1; j < i; j++)
	{
		if (colData[j] > data_max)
		{
			data_max = colData[j];
			maxI = j;
		}
	}
	// close to left border - left
	if (maxI < (i / 10))
		y = -1;
	// close to right border - right
	else if (maxI > (i - (i / 10)))
		y = getHeight () + 1;
	// take value in middle
	else
		y = (maxI * col_width) + col_width / 2;
	delete[]colData;
	delete[]averageData;
	return 0;
}

int Rts2Image::getCenter (double &x, double &y, int bin)
{
	int ret;
	long i;
	i = 0;
	while ((i + bin) < getHeight ())
	{
		ret = getCenterRow (i, bin, x);
		if (ret)
			return -1;
		if (x > 0 && x < getWidth ())
			break;
		i += bin;
	}
	i = 0;
	while ((i + bin) < getWidth ())
	{
		ret = getCenterCol (i, bin, y);
		if (ret)
			return -1;
		if (y > 0 && y < getHeight ())
			break;
		i += bin;
	}
	return 0;
}*/

long Rts2Image::getSumNPixels ()
{
	long ret = 0;
	for (Channels::iterator iter = channels.begin (); iter != channels.end (); iter++)
		ret += (*iter)->getNPixels ();
	return ret;
}

int Rts2Image::getError (double &eRa, double &eDec, double &eRad)
{
	if (isnan (ra_err) || isnan (dec_err) || isnan (img_err))
		return -1;
	eRa = ra_err;
	eDec = dec_err;
	eRad = img_err;
	return 0;
}

std::string Rts2Image::getOnlyFileName ()
{
	return expandPath ("%y%m%d%H%M%S-%s-%p.fits");
}

void Rts2Image::printFileName (std::ostream & _os)
{
	_os << getFileName ();
}

void Rts2Image::print (std::ostream & _os, int in_flags)
{
	if (in_flags & DISPLAY_FILENAME)
	{
		printFileName (_os);
		_os << SEP;
	}
	if (in_flags & DISPLAY_SHORT)
	{
		printFileName (_os);
		_os << std::endl;
		return;
	}

	if (in_flags & DISPLAY_OBS)
		_os << std::setw (5) << getObsId () << SEP;

	if (!(in_flags & DISPLAY_ALL))
		return;

	std::ios_base::fmtflags old_settings = _os.flags ();
	int old_precision = _os.precision (2);

	_os
		<< std::setw (5) << getCameraName () << SEP
		<< std::setw (4) << getImgId () << SEP
		<< Timestamp (getExposureSec () + (double) getExposureUsec () /	USEC_SEC) << SEP
		<< std::setw (3) << getFilter () << SEP
		<< std::setw (8) << std::fixed << getExposureLength () << "'" << SEP
		<< LibnovaDegArcMin (ra_err) << SEP << LibnovaDegArcMin (dec_err) << SEP
		<< LibnovaDegArcMin (img_err) << std::endl;

	_os.flags (old_settings);
	_os.precision (old_precision);
}

void Rts2Image::writeConnBaseValue (const char* name, Rts2Value * val, const char *desc)
{
	switch (val->getValueBaseType ())
	{
		case RTS2_VALUE_STRING:
			setValue (name, val->getValue (), desc);
			break;
		case RTS2_VALUE_INTEGER:
			setValue (name, val->getValueInteger (), desc);
			break;
		case RTS2_VALUE_TIME:
			setValue (name, val->getValueDouble (), desc);
			break;
		case RTS2_VALUE_DOUBLE:
			setValue (name, val->getValueDouble (), desc);
			break;
		case RTS2_VALUE_FLOAT:
			setValue (name, val->getValueFloat (), desc);
			break;
		case RTS2_VALUE_BOOL:
			setValue (name, ((Rts2ValueBool *) val)->getValueBool (), desc);
			break;
		case RTS2_VALUE_SELECTION:
			setValue (name, ((Rts2ValueSelection *) val)->getSelName (), desc);
			break;
		case RTS2_VALUE_LONGINT:
			setValue (name, ((Rts2ValueLong *) val)->getValueLong (), desc);
			break;
		case RTS2_VALUE_RADEC:
		{
			// construct RADEC string and desc, write it down
			char *v_name = new char[strlen (name) + 4];
			char *v_desc = new char[strlen (desc) + 5];
			// write RA
			strcpy (v_name, name);
			strcat (v_name, "RA");
			strcpy (v_desc, desc);
			strcat (v_desc, " RA");
			setValue (v_name, ((Rts2ValueRaDec *) val)->getRa (), v_desc);
			// now DEC
			strcpy (v_name, name);
			strcat (v_name, "DEC");
			strcpy (v_desc, desc);
			strcat (v_desc, " DEC");
			setValue (v_name, ((Rts2ValueRaDec *) val)->getDec (), v_desc);
			// if it is mount ra dec - write heliocentric time
			if (!strcmp ("TEL", name))
			{
				double JD = getMidExposureJD ();
				struct ln_equ_posn equ;
				equ.ra = ((Rts2ValueRaDec *) val)->getRa ();
				equ.dec = ((Rts2ValueRaDec *) val)->getDec ();
				setValue ("JD_HELIO", JD + ln_get_heliocentric_time_diff (JD, &equ), "helioceentric JD");
			}

			// free memory
			delete[] v_name;
			delete[] v_desc;
		}
		break;
		case RTS2_VALUE_ALTAZ:
		{
			// construct RADEC string and desc, write it down
			char *v_name = new char[strlen (name) + 4];
			char *v_desc = new char[strlen (desc) + 10];
			// write RA
			strcpy (v_name, name);
			strcat (v_name, "ALT");
			strcpy (v_desc, desc);
			strcat (v_desc, " altitude");
			setValue (v_name, ((Rts2ValueAltAz *) val)->getAlt (), v_desc);
			// now DEC
			strcpy (v_name, name);
			strcat (v_name, "AZ");
			strcpy (v_desc, desc);
			strcat (v_desc, " azimuth");
			setValue (v_name, ((Rts2ValueAltAz *) val)->getAz (), v_desc);
			// free memory
			delete[] v_name;
			delete[] v_desc;
		}
		break;
		default:
			logStream (MESSAGE_ERROR) <<
				"Don't know how to write to FITS file header value '" << name
				<< "' of type " << val->getValueType () << sendLog;
			break;
	}
}

void Rts2Image::writeConnArray (TableData *tableData)
{
	if (!getFitsFile ())
	{
		if (flags & IMAGE_NOT_SAVE)
			return;
		openImage ();
	}
	writeArray (tableData->getName (), tableData);
	setValue ("TSTART", tableData->getDate (), "data are recorded from this time");
}

void Rts2Image::writeConnValue (Rts2Conn * conn, Rts2Value * val)
{
	const char *desc = val->getDescription ().c_str ();
	char *name = (char *) val->getName ().c_str ();
	char *name_stat;
	char *n_top;

	// array groups to write. First they are created, then they are written at the end

	if (conn->getOtherType () == DEVICE_TYPE_SENSOR || val->prefixWithDevice () || val->getValueExtType () == RTS2_VALUE_ARRAY)
	{
		name = new char[strlen (name) + strlen (conn->getName ()) + 2];
		strcpy (name, conn->getName ());
		strcat (name, ".");
		strcat (name, val->getName ().c_str ());
	}

	std::map <int, TableData *>::iterator ai;

	switch (val->getValueExtType ())
	{
		case 0:
			writeConnBaseValue (name, val, desc);
			break;
		case RTS2_VALUE_ARRAY:
			ai = arrayGroups.find (val->getWriteGroup ());

			if (ai == arrayGroups.end ())
			{
				Rts2Value *infoTime = conn->getValue (RTS2_VALUE_INFOTIME);
				if (infoTime)
				{
					TableData *td = new TableData (name, infoTime->getValueDouble ());
					td->push_back (new ColumnData (name, ((rts2core::DoubleArray *) val)->getValueVector ()));
					arrayGroups[val->getWriteGroup ()] = td;
				}
			}
			else
			{
				ai->second->push_back (new ColumnData (name, ((rts2core::DoubleArray *) val)->getValueVector ()));
			}

			break;
		case RTS2_VALUE_STAT:
			writeConnBaseValue (name, val, desc);
			setValue (name, val->getValueDouble (), desc);
			name_stat = new char[strlen (name) + 6];
			n_top = name_stat + strlen (name);
			strcpy (name_stat, name);
			*n_top = '.';
			n_top++;
			strcpy (n_top, "MODE");
			setValue (name_stat, ((Rts2ValueDoubleStat *) val)->getMode (), desc);
			strcpy (n_top, "MIN");
			setValue (name_stat, ((Rts2ValueDoubleStat *) val)->getMin (), desc);
			strcpy (n_top, "MAX");
			setValue (name_stat, ((Rts2ValueDoubleStat *) val)->getMax (), desc);
			strcpy (n_top, "STD");
			setValue (name_stat, ((Rts2ValueDoubleStat *) val)->getStdev (), desc);
			strcpy (n_top, "NUM");
			setValue (name_stat, ((Rts2ValueDoubleStat *) val)->getNumMes (), desc);
			delete[]name_stat;
			break;
		case RTS2_VALUE_RECTANGLE:
			name_stat = new char[strlen(name) + 8];
			n_top = name_stat + strlen (name);
			strcpy (name_stat, name);
			*n_top = '.';
			n_top++;
			strcpy (n_top, "X");
			writeConnBaseValue (
				name_stat,
				((Rts2ValueRectangle *)val)->getX (),
				((Rts2ValueRectangle *)val)->getX ()->getDescription ().c_str ()
				);

			strcpy (n_top, "Y");
			writeConnBaseValue (
				name_stat,
				((Rts2ValueRectangle *)val)->getY (),
				((Rts2ValueRectangle *)val)->getY ()->getDescription ().c_str ()
				);

			strcpy (n_top, "HEIGHT");
			writeConnBaseValue (
				name_stat,
				((Rts2ValueRectangle *)val)->getHeight (),
				((Rts2ValueRectangle *)val)->getHeight ()->getDescription ().c_str ()
				);

			strcpy (n_top, "WIDTH");
			writeConnBaseValue (
				name_stat,
				((Rts2ValueRectangle *)val)->getWidth (),
				((Rts2ValueRectangle *)val)->getWidth ()->getDescription ().c_str ()
				);

			delete[]name_stat;
			break;
		case RTS2_VALUE_MMAX:
			writeConnBaseValue (name, val, desc);
			break;
	}
	if (conn->getOtherType () == DEVICE_TYPE_SENSOR || val->prefixWithDevice ())
	{
		delete[]name;
	}
}

void Rts2Image::recordChange (Rts2Conn * conn, Rts2Value * val)
{
	char *name;
	// construct name
	if (conn->getOtherType () == DEVICE_TYPE_SENSOR || val->prefixWithDevice ())
	{
		name = new char[strlen (conn->getName ()) + strlen (val->getName ().c_str ()) + 10];
		strcpy (name, conn->getName ());
		strcat (name, ".");
		strcat (name, val->getName ().c_str ());
	}
	else
	{
		name = new char[strlen (val->getName ().c_str ()) + 9];
		strcpy (name, val->getName ().c_str ());
	}
	strcat (name, ".CHANGED");
	setValue (name, val->wasChanged (), "true if value was changed during exposure");
	delete[]name;
}


void
Rts2Image::setEnvironmentalValues ()
{
	// record any environmental variables..
	std::vector <std::string> envWrite;
	Rts2Config::instance ()->deviceWriteEnvVariables (getCameraName (), envWrite);
	for (std::vector <std::string>::iterator iter = envWrite.begin (); iter != envWrite.end (); iter++)
	{
		char *value = getenv ((*iter).c_str ());
		std::string comment;
		std::string section =  std::string ("env_") + (*iter) + std::string ("_comment");
		// check for comment
		if (Rts2Config::instance ()->getString (getCameraName (), section.c_str (), comment) != 0)
			comment = std::string ("enviromental variable");

		if (value != NULL)
			setValue ((*iter).c_str (), value, comment.c_str ());
	}
}

void Rts2Image::writeConn (Rts2Conn * conn, imageWriteWhich_t which)
{
	for (Rts2ValueVector::iterator iter = conn->valueBegin ();
		iter != conn->valueEnd (); iter++)
	{
		Rts2Value *val = *iter;
		if (val->getWriteToFits ())
		{
			switch (which)
			{
				case EXPOSURE_START:
					if (val->getValueWriteFlags () == RTS2_VWHEN_BEFORE_EXP)
						writeConnValue (conn, val);
					val->resetValueChanged ();
					if (val->getValueDisplayType () == RTS2_DT_ROTANG)
						addRotang (val->getValueDouble ());
					break;
				case INFO_CALLED:
					if (val->getValueWriteFlags () == RTS2_VWHEN_BEFORE_END)
						writeConnValue (conn, val);
					break;
				case TRIGGERED:
					if (val->getValueWriteFlags () == RTS2_VWHEN_TRIGGERED)
					  	writeConnValue (conn, val);
					break;
				case EXPOSURE_END:
					// check to write change of value
					if (val->writeWhenChanged ())
						recordChange (conn, val);
					break;
			}
		}

	}
}

double Rts2Image::getLongtitude ()
{
	double lng = nan ("f");
	getValue ("LONGITUD", lng, true);
	return lng;
}

double Rts2Image::getExposureJD ()
{
	time_t tim = getCtimeSec ();
	return ln_get_julian_from_timet (&tim) +
		getCtimeUsec () / USEC_SEC / 86400.0;
}

double Rts2Image::getExposureLST ()
{
	double ret;
	ret = ln_get_apparent_sidereal_time (getExposureJD () * 15.0 + getLongtitude ());
	return ln_range_degrees (ret) / 15.0;
}

std::ostream & operator << (std::ostream & _os, Rts2Image & image)
{
	_os << "C " << image.getCameraName ()
		<< " [" << image.getChannelWidth (0) << ":"
		<< image.getChannelHeight (0) << "]"
		<< " RA " << image.getCenterRa () << " (" << LibnovaDegArcMin (image.
		ra_err)
		<< ") DEC " << image.getCenterDec () << " (" << LibnovaDegArcMin (image.
		dec_err)
		<< ") IMG_ERR " << LibnovaDegArcMin (image.img_err);
	return _os;
}
