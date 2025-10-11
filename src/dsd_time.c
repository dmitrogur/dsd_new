/*-------------------------------------------------------------------------------
 * dsd_time.c
 * Time and Date Functions
 *
 * LWVMOBILE
 * 2024-04 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

//TODO: Make sure everything still works as intended, every free(timestr) has a NULL check first,
//make sure no other loose or random time functions embedded in other files or functions, etc

#include "dsd.h"

//IPP
#define IPP_DMR_SAMPLE_NUM 1

#if defined(IPP_DMR_SAMPLE_NUM)
//get HHmmss timestamp no colon (file operations)

static void IPP_dmr_sample(char *buf)
{
    double sample_time;
    sample_time = (double)dmr_filter_sample_num / 48000.0;
//    sprintf(buf, "%*u/%.3f", 8, dmr_filter_sample_num, sample_time);
    sprintf(buf, "%u%9.3f | ", dmr_filter_sample_num, sample_time);
}

char * getTime()
{
  char buf[32];
  IPP_dmr_sample(buf);
  
  char * curr = calloc(47, sizeof(char));
//  time_t t = time(NULL);
//  struct tm * ptm = localtime(& t);
//  sprintf(curr,"%s %02d%02d%02d", buf, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  sprintf(curr,"%s", buf);
  return curr;
}

//get HH:mm:ss timestamp with colon (Sync/Console Display)
char * getTimeC()
{
  char buf[32];
  IPP_dmr_sample(buf);
  
  char * curr = calloc(49, sizeof(char));
//  time_t t = time(NULL);
//  struct tm * ptm = localtime(& t);
//  sprintf(curr, "%s %02d:%02d:%02d", buf, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  sprintf(curr, "%s", buf);
  return curr;
}
#else
//get HHmmss timestamp no colon (file operations)
char * getTime()
{
  char * curr = calloc(7, sizeof(char));
  time_t t = time(NULL);
  struct tm * ptm = localtime(& t);
  sprintf(curr,"%02d%02d%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return curr;
}

//get HH:mm:ss timestamp with colon (Sync/Console Display)
char * getTimeC()
{
  char * curr = calloc(9, sizeof(char));
  time_t t = time(NULL);
  struct tm * ptm = localtime(& t);
  sprintf(curr, "%02d:%02d:%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return curr;
}
#endif

//get HH:mm:ss timestamp with colon (Ncurses Call History)
char * getTimeN(time_t t)
{
  char * curr = calloc(9, sizeof(char));
  struct tm * ptm = localtime(& t);
  sprintf(curr, "%02d:%02d:%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return curr;
}

//get YYYYMMDD without hyphen (file operations)
char * getDate()
{
  char * curr = calloc(25, sizeof(char));
  time_t t = time(NULL);
  struct tm * ptm = localtime(& t);
  sprintf(curr,"%04d%02d%02d", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
  return curr;
}

//get YYYY-MM-DD with hyphen (Sync/Console Display)
char * getDateH()
{
  char * curr = calloc(27, sizeof(char));
  time_t t = time(NULL);
  struct tm * ptm = localtime(& t);
  sprintf(curr, "%04d-%02d-%02d", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
  return curr;
}

//get YYYY/MM/DD with forward slash (LRRP files)
char * getDateS()
{
  char * curr = calloc(27, sizeof(char));
  time_t t = time(NULL);
  struct tm * ptm = localtime(& t);
  sprintf(curr, "%04d/%02d/%02d", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
  return curr;
}

//get YYYY-MM-DD with hyphen (Ncurses Call History)
char * getDateN(time_t t)
{
  char * curr = calloc(27, sizeof(char));
  struct tm * ptm = localtime(& t);
  sprintf(curr, "%04d-%02d-%02d", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
  return curr;
}

//get HHmmss timestamp no colon (file operations)
char * getTimeF(time_t t)
{
  char * curr = calloc(7, sizeof(char));
  struct tm * ptm = localtime(& t);
  sprintf(curr,"%02d%02d%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return curr;
}

//get YYYYMMDD without hyphen (file operations)
char * getDateF(time_t t)
{
  char * curr = calloc(25, sizeof(char));
  struct tm * ptm = localtime(& t);
  sprintf(curr,"%04d%02d%02d", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
  return curr;
}
