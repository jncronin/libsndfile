/* TODO: Add copyright statement here */

#include "sfconfig.h"

#include "sndfile.h"
#include "sfendian.h"
#include "common.h"

#if HAVE_MPG123

#include <mpg123.h>

/* Default readers just return zero and log an unimplemented error */
static sf_count_t mp3_read_short_def (SF_PRIVATE *psf, short *ptr, sf_count_t len) ;
static sf_count_t mp3_read_int_def (SF_PRIVATE *psf, int *ptr, sf_count_t len) ;
static sf_count_t mp3_read_float_def (SF_PRIVATE *psf, float *ptr, sf_count_t len) ;
static sf_count_t mp3_read_double_def (SF_PRIVATE *psf, double *ptr, sf_count_t len) ;

#define read_conv(FROMTYPE, TOTYPE, POST_CONV, PRE_CONV) \
static sf_count_t mp3_read_ ## FROMTYPE ## _ ## TOTYPE (SF_PRIVATE *psf, TOTYPE *ptr, sf_count_t len) \
{ \
	MP3_PRIVATE *mdata = (MP3_PRIVATE *) psf->codec_data ; \
	int samps_read = 0 ; \
	off_t foffset ; \
\
	while (samps_read < len) \
	{ \
		if (mdata->buf && mdata->bufptr < mdata->buflen) \
		{ \
			/* read from the current frame \
			 * The double cast here allows buf to be implemented as \
			 * unsigned char * so that byte indexing works, but \
			 * prevents compiler cast-align errors.  We always \
			 * increment bufptr by the size of the data type so \
			 * the alignment is valid */ \
			FROMTYPE val = * (FROMTYPE *) (void*) &mdata->buf [mdata->bufptr] ; \
			mdata->bufptr += sizeof (FROMTYPE) ; \
			ptr [samps_read++] = (TOTYPE) (val * PRE_CONV) * POST_CONV ; \
		} \
		else \
		{ \
			/* Load up next frame */ \
			mdata->error = mpg123_decode_frame (mdata->mh, \
					&foffset, \
					&mdata->buf, \
					&mdata->buflen) ; \
			mdata->bufptr = 0 ; \
 \
			if (mdata->error != MPG123_OK || mdata->buflen == 0) \
				return samps_read ; \
		} \
	} \
	return samps_read ; \
}

static ssize_t mp3_read_wrapper (void *fildes, void *buf, size_t nbyte) ;
static off_t mp3_seek_wrapper (void *fildes, off_t val, int dir) ;

typedef struct
{
	mpg123_handle *mh ;
	int error ;
	int encoding, channels ;
	long rate ;

	/* Store the current frame here */
	unsigned char *buf ;
	size_t buflen ;
	off_t bufptr ;

	struct mpg123_frameinfo mi ;
} MP3_PRIVATE ;

read_conv (short, double, 1.0 / 32767.0, 1) ;
read_conv (short, float, 1.0f / 32767.0f, 1) ;
read_conv (short, int, 65536, 1) ;
read_conv (short, short, 1, 1) ;

read_conv (int, double, 1.0 / 2147483647.0, 1) ;
read_conv (int, float, 1.0f / 2147483647.0f, 1) ;
read_conv (int, int, 1, 1) ;
read_conv (int, short, 1, 1 / 65536) ;

read_conv (float, double, 1.0, 1.0f) ;
read_conv (float, float, 1.0f, 1.0f) ;
read_conv (float, int, 1, 2147483647.0f) ;
read_conv (float, short, 1, 32767.0f) ;

read_conv (double, double, 1.0, 1.0) ;
read_conv (double, float, 1.0f, 1.0) ;
read_conv (double, int, 1, 2147483647.0) ;
read_conv (double, short, 1, 32767.0) ;

int
mp3_open	(SF_PRIVATE *psf)
{
	int ret ;
	MP3_PRIVATE* mdata ;

	ret = mpg123_init () ;
	if (ret != MPG123_OK)
	{
		psf_log_printf (psf, "mpg123_init() failed with %i.\n", ret) ;
		return SFE_INTERNAL ;
	}

	mdata = calloc (1, sizeof (MP3_PRIVATE)) ;
	if (mdata == NULL)
	{
		psf_log_printf (psf, "out of memory.\n") ;
		return SFE_INTERNAL ;
	}

	psf->codec_data = mdata ;

	mdata->mh = mpg123_new (NULL, &mdata->error) ;
	if (!mdata->mh)
	{
		psf_log_printf (psf, "mpg123_new failed with %i.\n", mdata->error) ;
		return SFE_INTERNAL ;
	}

	switch (psf->file.mode)
	{
		case SFM_WRITE:
		case SFM_RDWR:
			psf_log_printf (psf, "mp3 write support unimplemented.\n") ;
			return SFE_UNIMPLEMENTED ;

		case SFM_READ:
			psf->read_short = mp3_read_short_def ;
			psf->read_int = mp3_read_int_def ;
			psf->read_float = mp3_read_float_def ;
			psf->read_double = mp3_read_double_def ;

			/* Use our own reader functions and open the
			 * supplied file */
			ret = mpg123_replace_reader_handle (mdata->mh,
					mp3_read_wrapper,
					mp3_seek_wrapper,
					NULL) ;
			if (ret != MPG123_OK)
			{
				psf_log_printf (psf, "mpg123_replace_reader_handle failed with %i.\n", ret) ;
				return SFE_INTERNAL ;
			}
			ret = mpg123_open_handle (mdata->mh, psf) ;
			if (ret != MPG123_OK)
			{
				psf_log_printf (psf, "mpg123_open_handle failed with %i.\n", ret) ;
				return SFE_INTERNAL ;
			}
			psf_fseek (psf, 0, SEEK_SET) ;

			ret = mpg123_info (mdata->mh, &mdata->mi) ;
			if (ret != MPG123_OK)
			{
				psf_log_printf (psf, "mpg123_info failed with %i.\n", ret) ;
				return SFE_INTERNAL ;
			}

			psf->sf.samplerate = mdata->mi.rate ;
			psf->sf.format = SF_FORMAT_MP3 ;

			psf->sf.channels = mdata->mi.mode == MPG123_M_MONO ? 1 : 2 ;

			mpg123_getformat (mdata->mh, &mdata->rate, &mdata->channels, &mdata->encoding) ;
			switch (mdata->encoding)
			{
				case MPG123_ENC_UNSIGNED_8:
					psf->sf.format |= SF_FORMAT_PCM_U8 ;
					break ;
				case MPG123_ENC_SIGNED_8:
					psf->sf.format |= SF_FORMAT_PCM_S8 ;
					break ;
				case MPG123_ENC_SIGNED_16:
					psf->sf.format |= SF_FORMAT_PCM_16 ;
					psf->read_double = mp3_read_short_double ;
					psf->read_float = mp3_read_short_float ;
					psf->read_int = mp3_read_short_int ;
					psf->read_short = mp3_read_short_short ;
					break ;
				case MPG123_ENC_SIGNED_24:
					psf->sf.format |= SF_FORMAT_PCM_24 ;
					break ;
				case MPG123_ENC_SIGNED_32:
					psf->sf.format |= SF_FORMAT_PCM_32 ;
					psf->read_double = mp3_read_int_double ;
					psf->read_float = mp3_read_int_float ;
					psf->read_int = mp3_read_int_int ;
					psf->read_short = mp3_read_int_short ;
					break ;
				case MPG123_ENC_FLOAT_32:
					psf->sf.format |= SF_FORMAT_FLOAT ;
					psf->read_double = mp3_read_float_double ;
					psf->read_float = mp3_read_float_float ;
					psf->read_int = mp3_read_float_int ;
					psf->read_short = mp3_read_float_short ;
					break ;
				case MPG123_ENC_FLOAT_64:
					psf->sf.format |= SF_FORMAT_DOUBLE ;
					psf->read_double = mp3_read_double_double ;
					psf->read_float = mp3_read_double_float ;
					psf->read_int = mp3_read_double_int ;
					psf->read_short = mp3_read_double_short ;
					break ;
			}

			mpg123_scan (mdata->mh) ;
			psf->datalength = mpg123_length (mdata->mh) * MPG123_SAMPLESIZE (mdata->encoding) ;
			psf->dataoffset = 0 ;
			psf->sf.frames = mpg123_length (mdata->mh) ;

			/*mp3_read_header(psf);*/
			break ;
	}

	return SFE_NO_ERROR ;
} /* mp3_open */

static sf_count_t mp3_read_short_def (SF_PRIVATE *psf, short *ptr, sf_count_t len)
{
	psf_log_printf (psf, "read_short not implemented for mp3 encoding %x.\n",
			((MP3_PRIVATE *) psf->codec_data) ->encoding) ;
	(void) psf ;
	(void) ptr ;
	(void) len ;
	return 0 ;
}

static sf_count_t mp3_read_int_def (SF_PRIVATE *psf, int *ptr, sf_count_t len)
{
	psf_log_printf (psf, "read_int not implemented for mp3 encoding %x.\n",
			((MP3_PRIVATE *) psf->codec_data) ->encoding) ;
	(void) psf ;
	(void) ptr ;
	(void) len ;
	return 0 ;
}

static sf_count_t mp3_read_float_def (SF_PRIVATE *psf, float *ptr, sf_count_t len)
{
	psf_log_printf (psf, "read_float not implemented for mp3 encoding %x.\n",
			((MP3_PRIVATE *) psf->codec_data) ->encoding) ;
	(void) psf ;
	(void) ptr ;
	(void) len ;
	return 0 ;
}

static sf_count_t mp3_read_double_def (SF_PRIVATE *psf, double *ptr, sf_count_t len)
{
	psf_log_printf (psf, "read_double not implemented for mp3 encoding %x.\n",
			((MP3_PRIVATE *) psf->codec_data) ->encoding) ;
	(void) psf ;
	(void) ptr ;
	(void) len ;
	return 0 ;
}


/* wrap psf_fread/seek */
static ssize_t mp3_read_wrapper (void *fildes, void *buf, size_t nbyte)
{
	return psf_fread (buf, 1, nbyte, (SF_PRIVATE *) fildes) ;
}
static off_t mp3_seek_wrapper (void *fildes, off_t val, int dir)
{
	return psf_fseek ((SF_PRIVATE *) fildes, (sf_count_t) val, dir) ;
}

#else /* HAVE_MPG123 */

int
mp3_open	(SF_PRIVATE *psf)
{
	psf_log_printf (psf, "This version of libsndfile was compiled without MP3 support.\n") ;
	return SFE_UNIMPLEMENTED ;
} /* mp3_open */

#endif

