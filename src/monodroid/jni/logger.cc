#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifdef ANDROID
#include <android/log.h>
#endif

#include "logger.hh"

#include "monodroid.h"
#include "monodroid-glue.hh"
#include "debug.hh"
#include "util.hh"
#include "globals.hh"

#undef DO_LOG
#define DO_LOG(_level_,_category_,_format_,_args_)						                        \
	va_start ((_args_), (_format_));									                        \
	__android_log_vprint ((_level_), CATEGORY_NAME((_category_)), (_format_), (_args_)); \
	va_end ((_args_));

using namespace xamarin::android;

// Must match the same ordering as LogCategories
static const char* log_names[] = {
	"*none*",
	"monodroid",
	"monodroid-assembly",
	"monodroid-debug",
	"monodroid-gc",
	"monodroid-gref",
	"monodroid-lref",
	"monodroid-timing",
	"monodroid-bundle",
	"monodroid-network",
	"monodroid-netlink",
	"*error*",
};

// Keep in sync with LogLevel defined in JNIEnv.cs
enum class LogLevel : unsigned int
{
	Unknown = 0x00,
	Default = 0x01,
	Verbose = 0x02,
	Debug   = 0x03,
	Info    = 0x04,
	Warn    = 0x05,
	Error   = 0x06,
	Fatal   = 0x07,
	Silent  = 0x08
};

#if defined(__i386__) && defined(__GNUC__)
#define ffs(__value__) __builtin_ffs ((__value__))
#elif defined(__x86_64__) && defined(__GNUC__)
#define ffs(__value__) __builtin_ffsll ((__value__))
#endif

// ffs(value) returns index of lowest bit set in `value`
#define CATEGORY_NAME(value) (value == 0 ? log_names [0] : log_names [ffs (value)])

#ifndef ANDROID
static void
__android_log_vprint (int prio, const char* tag, const char* fmt, va_list ap)
{
  printf ("%d [%s] ", prio, tag);
  vprintf (fmt, ap);
  putchar ('\n');
  fflush (stdout);
}
#endif

unsigned int log_categories;
unsigned int log_timing_categories;
int gc_spew_enabled;

static FILE*
open_file (LogCategories category, const char *path, const char *override_dir, const char *filename)
{
	char *p = NULL;
	FILE *f;

	if (path && access (path, W_OK) < 0) {
		log_warn (category, "Could not open path '%s' for logging (\"%s\"). Using '%s/%s' instead.",
				path, strerror (errno), override_dir, filename);
		path  = NULL;
	}

	if (!path) {
		utils.create_public_directory (override_dir);
		p     = utils.path_combine (override_dir, filename);
		path  = p;
	}

	unlink (path);

	f = utils.monodroid_fopen (path, "a");

	if (f) {
		utils.set_world_accessable (path);
	} else {
		log_warn (category, "Could not open path '%s' for logging: %s",
				path, strerror (errno));
	}

	free (p);

	return f;
}

static const char *gref_file = NULL;
static const char *lref_file = NULL;
static int light_gref  = 0;
static int light_lref  = 0;

void
init_reference_logging (const char *override_dir)
{
	if ((log_categories & LOG_GREF) != 0 && !light_gref) {
		gref_log  = open_file (LOG_GREF, gref_file, override_dir, "grefs.txt");
	}

	if ((log_categories & LOG_LREF) != 0 && !light_lref) {
		// if both lref & gref have files specified, and they're the same path, reuse the FILE*.
		if (lref_file != NULL && strcmp (lref_file, gref_file != NULL ? gref_file : "") == 0) {
			lref_log  = gref_log;
		} else {
			lref_log  = open_file (LOG_LREF, lref_file, override_dir, "lrefs.txt");
		}
	}
}

void
init_logging_categories ()
{
	char *value;
	char **args, **ptr;

#if !ANDROID
	log_categories = LOG_DEFAULT;
#endif
	log_timing_categories = LOG_TIMING_DEFAULT;
	if (monodroid_get_namespaced_system_property (Debug::DEBUG_MONO_LOG_PROPERTY, &value) == 0)
		return;

	args = utils.monodroid_strsplit (value, ",", 0);
	free (value);
	value = NULL;

	for (ptr = args; ptr && *ptr; ptr++) {
		const char *arg = *ptr;

		if (!strcmp (arg, "all")) {
			log_categories = 0xFFFFFFFF;
			break;
		}

#define CATEGORY(name,entry) do { \
		if (!strncmp (arg, name, sizeof(name)-1)) \
			log_categories |= entry; \
	} while (0)

		CATEGORY ("assembly", LOG_ASSEMBLY);
		CATEGORY ("default",  LOG_DEFAULT);
		CATEGORY ("debugger", LOG_DEBUGGER);
		CATEGORY ("gc",       LOG_GC);
		CATEGORY ("gref",     LOG_GREF);
		CATEGORY ("lref",     LOG_LREF);
		CATEGORY ("timing",   LOG_TIMING);
		CATEGORY ("bundle",   LOG_BUNDLE);
		CATEGORY ("network",  LOG_NET);
		CATEGORY ("netlink",  LOG_NETLINK);

#undef CATEGORY

		if (!strncmp (arg, "gref=", 5)) {
			log_categories  |= LOG_GREF;
			gref_file        = arg + 5;
		} else if (!strncmp (arg, "gref-", 5)) {
			log_categories  |= LOG_GREF;
			light_gref       = 1;
		} else if (!strncmp (arg, "lref=", 5)) {
			log_categories  |= LOG_LREF;
			lref_file        = arg + 5;
		} else if (!strncmp (arg, "lref-", 5)) {
			log_categories  |= LOG_LREF;
			light_lref       = 1;
		} else if (!strncmp (arg, "timing=bare", 11)) {
			log_timing_categories |= LOG_TIMING_BARE;
		}
#if !defined (WINDOWS) && defined (DEBUG)
		else if (!strncmp (arg, "debugger-log-level=", 19)) {
			debug.set_debugger_log_level (arg + 19);
		}
#endif
	}

	utils.monodroid_strfreev (args);

#if DEBUG
	if ((log_categories & LOG_GC) != 0)
		gc_spew_enabled = 1;
#endif  /* DEBUG */
}

void
log_error (LogCategories category, const char *format, ...)
{
	va_list args;

	DO_LOG (ANDROID_LOG_ERROR, category, format, args);
}

void
log_fatal (LogCategories category, const char *format, ...)
{
	va_list args;

	DO_LOG (ANDROID_LOG_FATAL, category, format, args);
}

void
log_info_nocheck (LogCategories category, const char *format, ...)
{
	va_list args;

	if ((log_categories & category) == 0)
		return;

	DO_LOG (ANDROID_LOG_INFO, category, format, args);
}

void
log_warn (LogCategories category, const char *format, ...)
{
	va_list args;

	DO_LOG (ANDROID_LOG_WARN, category, format, args);
}

void
log_debug_nocheck (LogCategories category, const char *format, ...)
{
	va_list args;

	if ((log_categories & category) == 0)
		return;

	DO_LOG (ANDROID_LOG_DEBUG, category, format, args);
}

//
// DO NOT REMOVE: used by Android.Runtime.Logger
//
MONO_API unsigned int
monodroid_get_log_categories ()
{
	return log_categories;
}

//
// DO NOT REMOVE: used by Android.Runtime.JNIEnv
//
MONO_API void
monodroid_log (LogLevel level, LogCategories category, const char *message)
{
	switch (level) {
		case LogLevel::Verbose:
		case LogLevel::Debug:
			log_debug_nocheck (category, message);
			break;

		case LogLevel::Info:
			log_info_nocheck (category, message);
			break;

		case LogLevel::Warn:
		case LogLevel::Silent: // warn is always printed
			log_warn (category, message);
			break;

		case LogLevel::Error:
			log_error (category, message);
			break;

		case LogLevel::Fatal:
			log_fatal (category, message);
			break;

		default:
		case LogLevel::Unknown:
		case LogLevel::Default:
			log_info_nocheck (category, message);
			break;
	}
}
