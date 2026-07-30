/* Minimal sharedspice.h for runtime dynamic loading of libngspice.
 * Derived from the public ngspice shared-library interface.
 */

#ifndef SHAREDSPICE_H
#define SHAREDSPICE_H

#ifdef __cplusplus
extern "C" {
#endif

/* No import/export decoration needed when loading the library dynamically. */
#ifndef SHAREDSPICE_IMPEXP
#define SHAREDSPICE_IMPEXP
#endif

/* Portable bool/pstdint macros. In C++ bool is a keyword; in C on Windows we
 * typedef it to int to match ngspice's internal bool.h.
 */
#if !defined(__cplusplus)
#  if defined(_MSC_VER) || defined(__MINGW32__) || defined(__CYGWIN__)
     typedef int bool;
#    ifndef true
#      define true 1
#    endif
#    ifndef false
#      define false 0
#    endif
#  else
#    include <stdbool.h>
#  endif
#endif

#include <stdint.h>

/* Complex numbers (used by vector_info). */
struct ngcomplex {
    double cx_real;
    double cx_imag;
};
typedef struct ngcomplex ngcomplex_t;

/* Direct vector info returned by ngGet_Vec_Info. */
typedef struct vector_info {
    char* v_name;
    int v_type;
    short v_flags;
    double* v_realdata;
    ngcomplex_t* v_compdata;
    int v_length;
} vector_info, *pvector_info;

/* Values for a single vector at the current time point. */
typedef struct vecvalues {
    char* name;
    double creal;
    double cimag;
    bool is_scale;
    bool is_complex;
} vecvalues, *pvecvalues;

/* Values for all vectors at the current time point. */
typedef struct vecvaluesall {
    int veccount;
    int vecindex;
    pvecvalues* vecsa;
} vecvaluesall, *pvecvaluesall;

/* Metadata for a single vector. */
typedef struct vecinfo {
    int number;
    char* vecname;
    bool is_real;
    void* pdvec;
    void* pdvecscale;
} vecinfo, *pvecinfo;

/* Metadata for all vectors in the current plot. */
typedef struct vecinfoall {
    char* name;
    char* title;
    char* date;
    char* type;
    int veccount;
    pvecinfo* vecs;
} vecinfoall, *pvecinfoall;

/* Callbacks passed to ngSpice_Init. */
typedef int (SendChar)(char* output, int ident, void* userdata);
typedef int (SendStat)(char* output, int ident, void* userdata);
typedef int (ControlledExit)(int exitstatus, bool immediate_unloading,
                              bool quit_on_exit, int ident, void* userdata);
typedef int (SendData)(pvecvaluesall data, int structcount, int ident,
                        void* userdata);
typedef int (SendInitData)(pvecinfoall data, int ident, void* userdata);
typedef int (BGThreadRunning)(bool running, int ident, void* userdata);

/* Callbacks passed to ngSpice_Init_Sync. */
typedef int (GetVSRCData)(double* vval, double timeval, char* node, int ident,
                           void* userdata);
typedef int (GetISRCData)(double* ival, double timeval, char* node, int ident,
                           void* userdata);
typedef int (GetSyncData)(double actualtime, double* deltatime,
                           double olddelta, int redostep, int ident,
                           int location, void* userdata);

SHAREDSPICE_IMPEXP int ngSpice_Init(SendChar* printfcn, SendStat* statfcn,
                                     ControlledExit* ngexit, SendData* sdata,
                                     SendInitData* sinitdata,
                                     BGThreadRunning* bgtrun, void* userdata);

SHAREDSPICE_IMPEXP int ngSpice_Init_Sync(GetVSRCData* vsrcdat,
                                          GetISRCData* isrcdat,
                                          GetSyncData* syncdat, int* ident,
                                          void* userdata);

SHAREDSPICE_IMPEXP int ngSpice_Command(char* command);
SHAREDSPICE_IMPEXP int ngSpice_Circ(char** circarray);
SHAREDSPICE_IMPEXP bool ngSpice_running(void);
SHAREDSPICE_IMPEXP char* ngSpice_CurPlot(void);
SHAREDSPICE_IMPEXP char** ngSpice_AllPlots(void);
SHAREDSPICE_IMPEXP char** ngSpice_AllVecs(char* plotname);
SHAREDSPICE_IMPEXP bool ngSpice_SetBkpt(double time);
SHAREDSPICE_IMPEXP pvector_info ngGet_Vec_Info(char* vecname);

#ifdef __cplusplus
}
#endif

#endif /* SHAREDSPICE_H */
