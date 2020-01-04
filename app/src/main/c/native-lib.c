#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <malloc.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <elf.h>
#include <dirent.h>
#include <ctype.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <asm/unistd.h>
#include <android/log.h>

#include "syscall_arch.h"
#include "syscalls.h"
#include "mylibc.h"

#define APPNAME "DetectFrida"
#define MAX_LINE 512
#define MAX_LENGTH 256
#define FRIDA_THREAD_GUM_JS_LOOP "gum-js-loop"
#define FRIDA_THREAD_GMAIN "gmain"
#define FRIDA_NAMEDPIPE_LINJECTOR "linjector"
#define PROC_MAPS "/proc/self/maps"
#define PROC_STATUS "/proc/self/task/%s/status"
#define PROC_FD "/proc/self/fd"
#define PROC_TASK "/proc/self/task"


//Structure to hold the checksum of executable section of library
typedef struct stTextSection{
    unsigned long memsize;
    unsigned long checksum;
}textSection;

#define NUM_LIBS 2
//Include more libs as per your need
static const char* libstocheck[NUM_LIBS] = {"libnative-lib.so", "libc.so"};
static textSection* elfSectionArr[NUM_LIBS] = {NULL};


#ifdef _32_BIT
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Shdr Elf_Shdr;
#elif _64_BIT
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Shdr Elf_Shdr;
#endif

static inline void parse_proc_maps_to_fetch_path(char** filepaths);
static inline bool fetch_checksum_of_library(const char* filePath, textSection** pTextSection);
static inline void detect_frida_loop(void* pargs);
static inline bool scan_executable_segments(char* map, textSection* pTextSection);
static inline ssize_t read_one_line(int fd, char *buf, unsigned int max_len);
static inline unsigned long checksum(void *buffer, size_t len);
static inline void detect_frida_threads();
static inline void detect_frida_namedpipe();

//Upon loading the library, this function annotated as constructor starts executing
__attribute__((constructor))
void detectfrida(){

    char* filePaths[NUM_LIBS];
    parse_proc_maps_to_fetch_path(filePaths);
    for(int i = 0; i < NUM_LIBS; i++) {
        fetch_checksum_of_library(filePaths[i], &elfSectionArr[i]);
        if(filePaths[i] != NULL)
            free(filePaths[i]);
    }
    pthread_t t;
    pthread_create(&t, NULL, (void*) detect_frida_loop, NULL);

}

static inline void parse_proc_maps_to_fetch_path(char** filepaths){
    int fd = 0;
    char map[MAX_LINE];
    int counter = 0;
    if ((fd = my_openat(AT_FDCWD, PROC_MAPS, O_RDONLY | O_CLOEXEC, 0)) != 0) {

        while ((read_one_line(fd, map, MAX_LINE)) > 0) {
            for(int i = 0; i < NUM_LIBS;i++) {
                if (my_strstr(map, libstocheck[i]) != NULL) {
                    char tmp[MAX_LENGTH]="";
                    char path[MAX_LENGTH]="";
                    char buf[5]="";
                    sscanf(map, "%s %s %s %s %s %s", tmp, buf, tmp, tmp, tmp, path);
                    if(buf[2] == 'x') {
                        size_t size = my_strlen(path)+1;
                        filepaths[i] = malloc(size);
                        my_strlcpy(filepaths[i], path, size);
                        counter++;
                    }
                }
            }
            if(counter == NUM_LIBS)
                break;
        }
        my_close(fd);
    }
}

static inline bool fetch_checksum_of_library(const char* filePath, textSection** pTextSection){

    Elf_Ehdr ehdr;
    Elf_Shdr sectHdr;
    int fd;

    fd = my_openat(AT_FDCWD, filePath, O_RDONLY | O_CLOEXEC, 0);
    if(fd < 0){
        return NULL;
    }

    my_read(fd, &ehdr, sizeof(Elf_Ehdr));
    my_lseek(fd, (off_t)ehdr.e_shoff, SEEK_SET);

    unsigned long memsize = 0;
    for(int i = 0; i < ehdr.e_shnum; i++){
        my_memset(&sectHdr,0,sizeof(Elf_Shdr));
        my_read(fd, &sectHdr, sizeof(Elf_Shdr));

        //__android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "SectionHeader[%d][%ld]", sectHdr.sh_name, sectHdr.sh_flags);

        //Typically PLT and Text Sections are executable sections which are protected
        if(sectHdr.sh_flags & SHF_ALLOC && sectHdr.sh_flags & SHF_EXECINSTR){
            if(sectHdr.sh_offset+sectHdr.sh_size > memsize){
                memsize = sectHdr.sh_offset + sectHdr.sh_size;
            }
        }
    }
    if(memsize == 0){
        __android_log_print(ANDROID_LOG_WARN, APPNAME, "No executable section found. Suspicious");
        my_close(fd);
        return false;
    }
    //This memory is not released as the checksum is checked in a thread
    *pTextSection = malloc(sizeof(textSection));
    my_lseek(fd, 0, SEEK_SET);
    uint8_t* buffer = malloc( memsize * sizeof(uint8_t));
    my_read(fd, buffer, memsize);
    (*pTextSection)->memsize = memsize;
    (*pTextSection)->checksum = checksum(buffer, memsize);
    free(buffer);
    my_close(fd);
    return true;
}



void detect_frida_loop(void* pargs) {
    int fd = 0;
    char map[MAX_LINE];
    struct timespec timereq;
    timereq.tv_sec = 1;
    timereq.tv_nsec = 0;

    while (1) {

        detect_frida_threads();
        detect_frida_namedpipe();

        if ((fd = my_openat(AT_FDCWD, PROC_MAPS, O_RDONLY | O_CLOEXEC, 0 )) != 0) {

            while ((read_one_line(fd, map, MAX_LINE)) > 0) {
                for(int i = 0; i < NUM_LIBS; i++) {
                    if (my_strstr(map, libstocheck[i]) != NULL) {
                        if (true == scan_executable_segments(map, elfSectionArr[i])) {
                            break;
                        }
                    }
                }
            }
        } else {
            __android_log_print(ANDROID_LOG_WARN, APPNAME, "Error opening /proc/self/maps. That's usually a bad sign.");

        }
        my_close(fd);

        my_nanosleep(&timereq, NULL);

    }
}

static inline bool scan_executable_segments(char* map, textSection* pElfSectArr){
    unsigned long start, end;
    char buf[MAX_LINE]="";
    char path[MAX_LENGTH]="";
    char tmp[100]="";

    sscanf(map, "%lx-%lx %s %s %s %s %s", &start, &end, buf, tmp, tmp, tmp, path );

    if (buf[2] == 'x') {
        uint8_t* buffer = (uint8_t*)start;
        unsigned long memsize = end - start;
        if(memsize >= pElfSectArr->memsize)
            memsize = pElfSectArr->memsize;
        unsigned long output = checksum(buffer, memsize);
        //__android_log_print(ANDROID_LOG_VERBOSE, APPNAME,  "Buffer Size[%ld][%ld]",pElfSectArr->checksum, output);
        if(output != pElfSectArr->checksum){
            __android_log_print(ANDROID_LOG_WARN, APPNAME,  "Executable Section Manipulated, "
                                                               "maybe due to Frida or other hooking framework."
                                                              "Act Now!!!");
        }
        return true;
    }

    return false;
}

static inline ssize_t read_one_line(int fd, char *buf, unsigned int max_len) {
    char b;
    ssize_t ret;
    ssize_t bytes_read = 0;

    my_memset(buf, 0, max_len);

    do {
        ret = my_read(fd, &b, 1);

        if (ret != 1) {
            if (bytes_read == 0) {
                // error or EOF
                return -1;
            } else {
                return bytes_read;
            }
        }

        if (b == '\n') {
            return bytes_read;
        }

        *(buf++) = b;
        bytes_read += 1;

    } while (bytes_read < max_len - 1);

    return bytes_read;
}

static inline unsigned long checksum(void *buffer, size_t len)
{
    unsigned long seed = 0;
    uint8_t* buf = (uint8_t*)buffer;
    size_t i;
    for (i = 0; i < len; ++i)
        seed += (unsigned long)(*buf++);
    return seed;
}

static inline void detect_frida_threads(){

    DIR *dir = opendir(PROC_TASK);

    if (dir != NULL) {
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            char filePath[MAX_LENGTH] = "";

            if (0 == my_strcmp(entry->d_name, ".") || 0 == my_strcmp(entry->d_name, "..")) {
                continue;
            }
            snprintf(filePath, sizeof(filePath), PROC_STATUS, entry->d_name);

            int fd = my_openat(AT_FDCWD, filePath, O_RDONLY | O_CLOEXEC, 0);
            if(fd != 0) {
                char buf[MAX_LENGTH] = "";
                read_one_line(fd, buf, MAX_LENGTH);
                if (my_strstr(buf, FRIDA_THREAD_GUM_JS_LOOP) || my_strstr(buf, FRIDA_THREAD_GMAIN)) {
                    //Kill the thread. This freezes the app. Check if it is an anticpated behaviour
                    //int tid = my_atoi(entry->d_name);
                    //int ret = my_tgkill(getpid(), tid, SIGSTOP);
                    __android_log_print(ANDROID_LOG_WARN, APPNAME,
                                        "Frida specific thread found. Act now!!!");
                }
                my_close(fd);
            }

        }
        closedir(dir);

    }

}

static inline void detect_frida_namedpipe(){

    DIR *dir = opendir(PROC_FD);
    if (dir != NULL) {
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            struct stat filestat;
            char buf[MAX_LENGTH] = "";
            char filePath[MAX_LENGTH] = "";
            snprintf(filePath, sizeof(filePath), "/proc/self/fd/%s", entry->d_name);

            lstat(filePath, &filestat);

            if((filestat.st_mode & S_IFMT) == S_IFLNK) {
                //TODO: Another way is to check if filepath belongs to a path not related to system or the app
                my_readlinkat(AT_FDCWD, filePath, buf, MAX_LENGTH);
                if (NULL != my_strstr(buf, FRIDA_NAMEDPIPE_LINJECTOR)) {
                    __android_log_print(ANDROID_LOG_WARN, APPNAME,
                                        "Frida specific named pipe found. Act now!!!");
                }
            }

        }
    }
    closedir(dir);
}

