/* Host save files are explicit (--save-file), keeping unattended comparisons
 * reproducible. Write a complete temporary file and atomically replace only
 * after flushing it. Failed loads never reach this write path. */
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define save_open(path) _open(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE)
#define save_fdopen _fdopen
#define save_close _close
#define save_flush _commit
#define save_pid _getpid
#else
#include <unistd.h>
#define save_open(path) open(path,O_WRONLY|O_CREAT|O_EXCL,0600)
#define save_fdopen fdopen
#define save_close close
#define save_flush fsync
#define save_pid getpid
#endif
static bool save_paths_distinct(const char *a, const char *b)
{
    if (!a || !b) return true;
#ifdef _WIN32
    char *x=_fullpath(NULL,a,0), *y=_fullpath(NULL,b,0);
    bool distinct=x && y && _stricmp(x,y)!=0;
#else
    /* Resolve parent directories even when a save does not exist yet. */
    char *x=NULL,*y=NULL;
    const char *paths[2]={a,b}; char **resolved[2]={&x,&y};
    for (unsigned i=0;i<2;++i) {
        *resolved[i]=realpath(paths[i],NULL);
        if (*resolved[i]) continue;
        char *copy=strdup(paths[i]); if (!copy) continue;
        char *slash=strrchr(copy,'/'); const char *name=slash?slash+1:copy;
        if (slash) *slash=0;
        char *parent=realpath(slash?(slash==copy?"/":copy):".",NULL);
        if (parent) {
            size_t n=strlen(parent)+strlen(name)+2; *resolved[i]=(char *)malloc(n);
            if (*resolved[i]) snprintf(*resolved[i],n,"%s/%s",parent,name);
            free(parent);
        }
        free(copy);
    }
    bool distinct=x && y && strcmp(x,y)!=0;
#endif
    free(x);free(y);return distinct;
}
static bool save_load(const char *path, unsigned region)
{
    if (!path) return true;
    size_t n=cyc_nvram_size(region);
    if (!n) { fprintf(stderr,"save region %u has no nonvolatile storage\n",region); return false; }
    FILE *f=fopen(path,"rb");
    if (!f) {
        if (errno==ENOENT) return true;
        fprintf(stderr,"cannot read save %s: %s\n",path,strerror(errno)); return false;
    }
    uint8_t *buffer=(uint8_t *)malloc(n);
    bool ok=buffer && fread(buffer,1,n,f)==n && fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if (ok) ok=cyc_nvram_import(region,buffer,n);
    free(buffer);
    if (!ok) fprintf(stderr,"invalid save %s: expected exactly %zu bytes\n",path,n);
    return ok;
}
static bool save_write(const char *path, unsigned region)
{
    if (!path) return true;
    size_t n=cyc_nvram_size(region);
    uint8_t *buffer=(uint8_t *)malloc(n);
    char *temporary=(char *)malloc(strlen(path)+64);
    if (!n || !buffer || !temporary) { free(buffer); free(temporary); return false; }
    bool ok=cyc_nvram_export(region,buffer,n);
    int fd=-1;
    for (unsigned attempt=0;ok && attempt<100;++attempt) {
        snprintf(temporary,strlen(path)+64,"%s.tmp-%lu-%u",path,(unsigned long)save_pid(),attempt);
        fd=save_open(temporary);
        if (fd>=0 || errno!=EEXIST) break;
    }
    if (fd<0) ok=false;
    if (ok) {
        FILE *f=save_fdopen(fd,"wb");
        if (!f) { save_close(fd); ok=false; }
        else {
            ok=fwrite(buffer,1,n,f)==n && fflush(f)==0 && save_flush(fd)==0;
            if (fclose(f)) ok=false;
        }
    }
    if (ok) {
#ifdef _WIN32
        ok=MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
        ok=rename(temporary,path)==0;
#endif
    }
    if (fd>=0 && !ok) remove(temporary);
    if (!ok) fprintf(stderr,"cannot write save %s; previous file retained\n",path);
    free(buffer); free(temporary); return ok;
}
