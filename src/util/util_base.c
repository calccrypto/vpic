/*
 * Written by:
 *   Kevin J. Bowers, Ph.D.
 *   Plasma Physics Group (X-1)
 *   Applied Physics Division
 *   Los Alamos National Lab
 * March/April 2004 - Original version
 *
 */

#include "util_base.h" // Declarations
#include <stdio.h>     // For vfprintf
#include <stdarg.h>    // For va_list, va_start, va_end
#include <string.h>    // for strstr
#include <pthread.h>   // for pthread_mutex_t
#include "sicm_low.h"  // for SICM
#include <numa.h>      // for numa_node_size64

/****************************************************************************/

#define STRIP_CMDLINE( what, T, convert )                         \
T                                                                 \
strip_cmdline_##what( int * pargc,                                \
                      char *** pargv,                             \
                      const char * key,                           \
                      T val ) {                                   \
  int i, n = 0;                                                   \
  for( i=0; i<(*pargc); i++ )                                     \
    if( strcmp( (*pargv)[i], key ) ) (*pargv)[n++] = (*pargv)[i]; \
    else if( (++i)<(*pargc) ) val = convert( (*pargv)[i] );       \
  (*pargv)[n] = NULL, (*pargc) = n; /* ANSI -argv is NULL terminated */ \
  return val;                                                     \
}

int
strip_cmdline( int * pargc,
               char *** pargv,
               const char * key ) {
  int i, n = 0, val = 0;
  for( i=0; i<(*pargc); i++ )
    if( strcmp( (*pargv)[i], key ) ) (*pargv)[n++] = (*pargv)[i];
    else val++;
  (*pargv)[n] = NULL, (*pargc) = n; /* ANSI - argv is NULL terminated */
  return val;
}

STRIP_CMDLINE( int,    int,          atoi )
STRIP_CMDLINE( double, double,       atof )
STRIP_CMDLINE( string, const char *,      )

#undef STRIP_CMDLINE

/****************************************************************************/

int string_starts_with(const char *str, const char *pre)
{
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? 0 : strncmp(pre, str, lenpre) == 0;
}

int string_contains(const char *str, const char *substr)
{
    char *output = NULL;
    output = strstr(str,substr);

    char* pos = strstr(str, substr);

    if (pos) {
        return 1;
    }
    else {
        return 0;
    }
}

int string_matches(const char* str, const char* match)
{
    if (strcmp(str, match) != 0) {
        return 0;
    }
    else {
        return 1;
    }
}

void detect_old_style_arguments(int* pargc, char *** pargv)
{
  // FIXME: This could also warn for unknown options being passed (such as typos)
  int i;

  for (i=0; i<(*pargc); i++)
  {
      const int num_prefix_keys = 2;
      char* prefix_keys[num_prefix_keys];

      const int num_match_keys = 1;
      char* match_keys[num_match_keys];

      prefix_keys[0] = "-tpp";
      prefix_keys[1] = "-restore";

      match_keys[0] = "restart";

      char* arg = (*pargv)[i];

      // Set loop bound to 0
      int j = 0;

      // Check for invalid prefixes
      for (j = 0; j < num_prefix_keys; j++)
      {
          // Search for tpp
          if (string_starts_with(arg,prefix_keys[j]))
          {
              char output_message[64];

              sprintf(output_message,
                  "Aborting. Single dashed flag %1$s is invalid (needs '-%1$s').",
                  prefix_keys[j]
              );

              WARNING(( "Input Flags Look Like They Are Using Legacy Style."));
              ERROR((output_message));
          }
      }

      // Search for restore, single dash
      for (j = 0; j < num_match_keys; j++)
      {
          if (string_matches(arg,match_keys[j]))
          {
              // TODO: This could be tightened up more, as it disallows the use
                  // of a file called 'restart'
              char output_message[64];
              sprintf(output_message,
                  "Old Argument Syntax Detected: %s",
                  match_keys[j]
              );

              WARNING(( "Input Flags Look Like They Are Using Legacy Style."));
              ERROR((output_message));
          }
      }

		  // Check for "=" (equals)
      // TODO: Add an option to make this an error or a warning
			if (string_contains(arg, "="))
			{
         const int NUM_WARN_REPEAT = 5;
         for (j = 0; j < NUM_WARN_REPEAT; j++)
         {
            WARNING(( "Arguments contains '=', is this intentional? (use a space)" ));
         }
			}

  }

}

static void * use_sicm(const size_t n) {
  static sicm_device_list devs = {};
  static sicm_device **usable = NULL;
  static unsigned int usable_count = 0;
  static unsigned int selected = 0;
  static sicm_arena *arena = NULL;

  // only happens at init
  if (!usable) {
    devs = sicm_init();

    // get number of usable devices in order of preference
    static const sicm_device_tag preferences[] = {SICM_KNL_HBM, SICM_DRAM};
    static const unsigned int preference_count = sizeof(preferences) / sizeof(sicm_device_tag);

    if (!(usable = malloc(devs.count * sizeof(struct sicm_device *)))) {
      ERROR(( "Unable to allocate space for keeping track of usable devices" ));
    }

    // store usable devices in order of preference
    usable_count = 0;
    for(unsigned int p = 0; p < preference_count; p++) {
      for(unsigned int d = 0; d < devs.count; d++) {
        if (devs.devices[d].tag == preferences[p]) {
          usable[d] = &devs.devices[d];
          usable_count++;
        }
      }
    }

    if (!usable_count) {
      ERROR(( "Unable to find any devices with the given preferences" ));
    }

    // create an arena on the selected device (ignore error)
    arena = sicm_arena_create(0, &devs.devices[selected = 0]);
  }

  // try to allocate
  // If the allocation fails, try all of the devices, starting from the currently
  // selected device until memory is allocated or the devices run out.
  // This assumes that memory used in previously used devices is never deallocated.
  void *ptr = sicm_arena_alloc(arena, n);
  if (!ptr) {
    WARNING(( "Failed to allocate using old arena in device index %u (%s).", selected, sicm_device_tag_str(usable[selected]->tag) ));

    // another arena is created on the previously selected device before moving to the next device
    unsigned int i = selected;
    for(; i < usable_count; i++) {
      if (!(arena = sicm_arena_create(0, usable[i]))) {
        continue;
      }

      if ((ptr = sicm_arena_alloc(arena, n))) {
        selected = i;
        MESSAGE(( "Allocated %zu bytes in new arena on device index %u", n, selected ));
        break;
      }
    }

    // if there are no more devices to choose from, always fail
    if (i == usable_count) {
      selected = usable_count;
      arena = NULL;
    }
  }

  if (ptr) {
    int node = -1;
    numa_move_pages(0, 1, &ptr, NULL, &node, 0);
    MESSAGE(( "Allocated %zu bytes on numa node %u (%s)", n, node, sicm_device_tag_str(usable[selected]->tag) ));
  }
  else {
    WARNING(( "Failed to allocate %zu bytes", n));
  }

  return ptr;
}

void
util_malloc( const char * err,
             void * mem_ref,
             size_t n ) {
  char * mem;

  // If no err given, use a default error
  if( !err ) err = "malloc failed (n=%lu)";

  // Check that mem_ref is valid
  if( !mem_ref ) ERROR(( err, (unsigned long)n ));

  // A do nothing request
  if( n==0 ) { *(char **)mem_ref = NULL; return; }

  // Allocate the memory ... abort if the allocation fails
  mem = (char *)use_sicm(n);
  if( !mem ) ERROR(( err, (unsigned long)n ));
  *(char **)mem_ref = mem;
}

void
util_free( void * mem_ref ) {
  char * mem;
  if( !mem_ref ) return;
  mem = *(char **)mem_ref;
  if( mem ) sicm_free(mem);
  *(char **)mem_ref = NULL;
}

void
util_malloc_aligned( const char * err,
                     void * mem_ref,
                     size_t n,
                     size_t a )
{
  char *mem_u, *mem_a, **mem_p;

  // If no err given, use a default error.
  if ( !err )
    err = "malloc aligned failed (n=%lu, a=%lu)";

  // Check that mem_ref is valid and a is a power of two.
  if ( !mem_ref || a==0 || ( a & ( a - 1 ) ) != 0 )
    ERROR( ( err, (unsigned long) n, (unsigned long) a ) );

  // A do nothing request.
  if ( n == 0 )
  {
    *(char **) mem_ref = NULL;
    return;
  }

  // Adjust small alignments to a minimal valid alignment
  // and convert a into a mask of the address LSB.
  if ( a < 16 )
    a = 16;

  a--;

  // Allocate the raw unaligned memory.  Abort if the allocation fails.
  mem_u = (char *)use_sicm( n + a + sizeof(char *) );

  if ( !mem_u )
    ERROR( ( err, (unsigned long) n, (unsigned long) a ) );

  // Compute the pointer to the aligned memory and save a pointer to the
  // raw unaligned memory for use on free_aligned.
  mem_a = (char *) ( ( (unsigned long int) ( mem_u +
					     a +
					     sizeof( char * ) ) ) & ( ~a ) );

  mem_p = (char **) ( mem_a - sizeof( char * ) );

  mem_p[0] = mem_u;

  *(char **) mem_ref = mem_a;
}

void
util_free_aligned( void * mem_ref ) {
  char *mem_u, *mem_a, **mem_p;
  if( !mem_ref ) return;
  mem_a = *(char **)mem_ref;
  if( mem_a ) {
    mem_p = (char **)(mem_a - sizeof(char *));
    mem_u = mem_p[0];
    sicm_free(mem_u);
  }
  *(char **)mem_ref = NULL;
}

/*****************************************************************************/

void
log_printf( const char *fmt, ... ) {
  va_list ap;
  va_start( ap, fmt );
  vfprintf( stderr, fmt, ap );
  va_end( ap );
  fflush( stderr );
}

uint32_t
_nanodelay( uint32_t i ) {
  uint32_t a = 0;
  for( ; i; i-- ) a^=0xdeadbeef, a>>=1;
  return a;
}
