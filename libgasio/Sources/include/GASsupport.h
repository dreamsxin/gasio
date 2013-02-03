
#ifndef _GAS_SUPPORT_H_
#define _GAS_SUPPORT_H_


#include <stdio.h>

#define GAS_NONE     0
#define GAS_IO       1
#define GAS_DETAIL   2
#define GAS_ALWAYS   3


#ifdef __cplusplus
extern "C" {
#endif

void gas_set_debug_level  ( int _visualize, int _debugLevel, FILE *_fdlog );
int  gas_error_message    ( const char *fmt, ... );
int  gas_debug_message    ( int level, const char *fmt, ... );

void gas_reset_stats   ();
void gas_adjust_stats  ();
void gas_compute_stats ();

int  gas_get_processors_count ();

#ifdef __cplusplus
}
#endif


#endif // _GAS_SUPPORT_H_
