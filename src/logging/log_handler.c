/*
 * log_handler.c: log management daemon handler
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library;  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include "log_handler.h"
#include "virerror.h"
#include "virobject.h"
#include "virfile.h"
#include "viralloc.h"
#include "virstring.h"
#include "virlog.h"
#include "virrotatingfile.h"

#include <unistd.h>
#include <fcntl.h>

#include "configmake.h"

VIR_LOG_INIT("logging.log_handler");

#define VIR_FROM_THIS VIR_FROM_LOGGING

#define DEFAULT_FILE_SIZE (128 * 1024)
#define DEFAULT_MAX_BACKUP 3
#define DEFAULT_MODE 0600

typedef struct _virLogHandlerLogFile virLogHandlerLogFile;
typedef virLogHandlerLogFile *virLogHandlerLogFilePtr;

struct _virLogHandlerLogFile {
    virRotatingFileWriterPtr file;
    int watch;
    int pipefd; /* Read from QEMU via this */
};

struct _virLogHandler {
    virObjectLockable parent;

    bool privileged;
    virLogHandlerLogFilePtr *files;
    size_t nfiles;
};

static virClassPtr virLogHandlerClass;
static void virLogHandlerDispose(void *obj);

static int
virLogHandlerOnceInit(void)
{
    if (!(virLogHandlerClass = virClassNew(virClassForObjectLockable(),
                                          "virLogHandler",
                                          sizeof(virLogHandler),
                                          virLogHandlerDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virLogHandler)


static void
virLogHandlerLogFileFree(virLogHandlerLogFilePtr file)
{
    if (!file)
        return;

    VIR_FORCE_CLOSE(file->pipefd);
    virRotatingFileWriterFree(file->file);

    if (file->watch != -1)
        virEventRemoveHandle(file->watch);
    VIR_FREE(file);
}


static void
virLogHandlerLogFileClose(virLogHandlerPtr handler,
                          virLogHandlerLogFilePtr file)
{
    size_t i;

    for (i = 0; i < handler->nfiles; i++) {
        if (handler->files[i] == file) {
            VIR_DELETE_ELEMENT(handler->files, i, handler->nfiles);
            virLogHandlerLogFileFree(file);
            break;
        }
    }
}


static virLogHandlerLogFilePtr
virLogHandlerGetLogFileFromWatch(virLogHandlerPtr handler,
                                 int watch)
{
    size_t i;

    for (i = 0; i < handler->nfiles; i++) {
        if (handler->files[i]->watch == watch)
            return handler->files[i];
    }

    return NULL;
}


static void
virLogHandlerDomainLogFileEvent(int watch,
                                int fd,
                                int events,
                                void *opaque)
{
    virLogHandlerPtr handler = opaque;
    virLogHandlerLogFilePtr logfile;
    char buf[1024];
    ssize_t len;

    virObjectLock(handler);
    logfile = virLogHandlerGetLogFileFromWatch(handler, watch);
    if (!logfile || logfile->pipefd != fd) {
        virEventRemoveHandle(watch);
        virObjectUnlock(handler);
        return;
    }

 reread:
    len = read(fd, buf, sizeof(buf));
    if (len < 0) {
        if (errno == EINTR)
            goto reread;

        virReportSystemError(errno, "%s",
                             _("Unable to read from log pipe"));
        goto error;
    }

    if (virRotatingFileWriterAppend(logfile->file, buf, len) != len)
        goto error;

    if (events & VIR_EVENT_HANDLE_HANGUP)
        goto error;

    virObjectUnlock(handler);
    return;

 error:
    virLogHandlerLogFileClose(handler, logfile);
    virObjectUnlock(handler);
}


virLogHandlerPtr
virLogHandlerNew(bool privileged)
{
    virLogHandlerPtr handler;

    if (virLogHandlerInitialize() < 0)
        goto error;

    if (!(handler = virObjectLockableNew(virLogHandlerClass)))
        goto error;

    handler->privileged = privileged;

    return handler;

 error:
    return NULL;
}


static virLogHandlerLogFilePtr
virLogHandlerLogFilePostExecRestart(virJSONValuePtr object)
{
    virLogHandlerLogFilePtr file;
    const char *path;

    if (VIR_ALLOC(file) < 0)
        return NULL;

    if ((path = virJSONValueObjectGetString(object, "path")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Missing file path in JSON document"));
        goto error;
    }

    if ((file->file = virRotatingFileWriterNew(path,
                                               DEFAULT_FILE_SIZE,
                                               DEFAULT_MAX_BACKUP,
                                               false,
                                               DEFAULT_MODE)) == NULL)
        goto error;

    if (virJSONValueObjectGetNumberInt(object, "pipefd", &file->pipefd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Missing file pipefd in JSON document"));
        goto error;
    }
    if (virSetInherit(file->pipefd, false) < 0) {
        virReportSystemError(errno, "%s",
                             _("Cannot enable close-on-exec flag"));
        goto error;
    }

    return file;

 error:
    virLogHandlerLogFileFree(file);
    return NULL;
}


virLogHandlerPtr
virLogHandlerNewPostExecRestart(virJSONValuePtr object,
                                bool privileged)
{
    virLogHandlerPtr handler;
    virJSONValuePtr files;
    ssize_t n;
    size_t i;

    if (!(handler = virLogHandlerNew(privileged)))
        return NULL;

    if (!(files = virJSONValueObjectGet(object, "files"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Missing files data from JSON file"));
        goto error;
    }

    if ((n = virJSONValueArraySize(files)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Malformed files data from JSON file"));
        goto error;
    }

    for (i = 0; i < n; i++) {
        virLogHandlerLogFilePtr file;
        virJSONValuePtr child = virJSONValueArrayGet(files, i);

        if (!(file = virLogHandlerLogFilePostExecRestart(child)))
            goto error;

        if (VIR_APPEND_ELEMENT_COPY(handler->files, handler->nfiles, file) < 0)
            goto error;

        if ((file->watch = virEventAddHandle(file->pipefd,
                                             VIR_EVENT_HANDLE_READABLE,
                                             virLogHandlerDomainLogFileEvent,
                                             handler,
                                             NULL)) < 0) {
            VIR_DELETE_ELEMENT(handler->files, handler->nfiles - 1, handler->nfiles);
            goto error;
        }
    }


    return handler;

 error:
    virObjectUnref(handler);
    return NULL;
}


static void
virLogHandlerDispose(void *obj)
{
    virLogHandlerPtr handler = obj;
    size_t i;

    for (i = 0; i < handler->nfiles; i++)
        virLogHandlerLogFileFree(handler->files[i]);
    VIR_FREE(handler->files);
}


static char *
virLogHandlerGetLogFilePathForDomain(virLogHandlerPtr handler,
                                     const char *driver,
                                     const unsigned char *domuuid ATTRIBUTE_UNUSED,
                                     const char *domname)
{
    char *path;
    if (handler->privileged) {
        if (virAsprintf(&path,
                        LOCALSTATEDIR "/log/libvirt/%s/%s.log",
                        driver, domname) < 0)
            return NULL;
    } else {
        char *cachedir;

        cachedir = virGetUserCacheDirectory();
        if (!cachedir)
            return NULL;

        if (virAsprintf(&path,
                        "%s/%s/log/%s.log", cachedir, driver, domname) < 0) {
            VIR_FREE(cachedir);
            return NULL;
        }

    }
    return path;
}


int
virLogHandlerDomainOpenLogFile(virLogHandlerPtr handler,
                               const char *driver,
                               const unsigned char *domuuid ATTRIBUTE_UNUSED,
                               const char *domname,
                               ino_t *inode,
                               off_t *offset)
{
    size_t i;
    virLogHandlerLogFilePtr file = NULL;
    int pipefd[2] = { -1, -1 };
    char *path;

    virObjectLock(handler);

    if (!(path = virLogHandlerGetLogFilePathForDomain(handler,
                                                      driver,
                                                      domuuid,
                                                      domname)))
        goto error;

    for (i = 0; i < handler->nfiles; i++) {
        if (STREQ(virRotatingFileWriterGetPath(handler->files[i]->file),
                  path)) {
            virReportSystemError(EBUSY,
                                 _("Cannot open log file: '%s'"),
                                 path);
            goto error;
        }
    }

    if (pipe(pipefd) < 0) {
        virReportSystemError(errno, "%s",
                             _("Cannot open fifo pipe"));
        goto error;
    }
    if (VIR_ALLOC(file) < 0)
        goto error;

    file->watch = -1;
    file->pipefd = pipefd[0];
    pipefd[0] = -1;

    if ((file->file = virRotatingFileWriterNew(path,
                                               DEFAULT_FILE_SIZE,
                                               DEFAULT_MAX_BACKUP,
                                               false,
                                               DEFAULT_MODE)) == NULL)
        goto error;

    if (VIR_APPEND_ELEMENT_COPY(handler->files, handler->nfiles, file) < 0)
        goto error;

    if ((file->watch = virEventAddHandle(file->pipefd,
                                         VIR_EVENT_HANDLE_READABLE,
                                         virLogHandlerDomainLogFileEvent,
                                         handler,
                                         NULL)) < 0) {
        VIR_DELETE_ELEMENT(handler->files, handler->nfiles - 1, handler->nfiles);
        goto error;
    }

    VIR_FREE(path);

    *inode = virRotatingFileWriterGetINode(file->file);
    *offset = virRotatingFileWriterGetOffset(file->file);

    virObjectUnlock(handler);
    return pipefd[1];

 error:
    VIR_FREE(path);
    VIR_FORCE_CLOSE(pipefd[0]);
    VIR_FORCE_CLOSE(pipefd[1]);
    virLogHandlerLogFileFree(file);
    virObjectUnlock(handler);
    return -1;
}


int
virLogHandlerDomainGetLogFilePosition(virLogHandlerPtr handler,
                                      const char *driver,
                                      const unsigned char *domuuid,
                                      const char *domname,
                                      ino_t *inode,
                                      off_t *offset)
{
    char *path;
    virLogHandlerLogFilePtr file = NULL;
    int ret = -1;
    size_t i;

    virObjectLock(handler);

    if (!(path = virLogHandlerGetLogFilePathForDomain(handler,
                                                      driver,
                                                      domuuid,
                                                      domname)))
        goto cleanup;

    for (i = 0; i < handler->nfiles; i++) {
        if (STREQ(virRotatingFileWriterGetPath(handler->files[i]->file),
                  path)) {
            file = handler->files[i];
            break;
        }
    }

    if (!file) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("No open log file for domain %s"),
                       domname);
        goto cleanup;
    }

    *inode = virRotatingFileWriterGetINode(file->file);
    *offset = virRotatingFileWriterGetOffset(file->file);

    ret = 0;

 cleanup:
    VIR_FREE(path);
    virObjectUnlock(handler);
    return ret;
}


char *
virLogHandlerDomainReadLogFile(virLogHandlerPtr handler,
                               const char *driver,
                               const unsigned char *domuuid,
                               const char *domname,
                               ino_t inode,
                               off_t offset,
                               size_t maxlen)
{
    char *path;
    virRotatingFileReaderPtr file = NULL;
    char *data = NULL;
    ssize_t got;

    virObjectLock(handler);

    if (!(path = virLogHandlerGetLogFilePathForDomain(handler,
                                                      driver,
                                                      domuuid,
                                                      domname)))
        goto error;

    if (!(file = virRotatingFileReaderNew(path, DEFAULT_MAX_BACKUP)))
        goto error;

    if (virRotatingFileReaderSeek(file, inode, offset) < 0)
        goto error;

    if (VIR_ALLOC_N(data, maxlen + 1) < 0)
        goto error;

    got = virRotatingFileReaderConsume(file, data, maxlen);
    if (got < 0)
        goto error;
    data[got] = '\0';

    virRotatingFileReaderFree(file);
    virObjectUnlock(handler);
    VIR_FREE(path);
    return data;

 error:
    VIR_FREE(path);
    VIR_FREE(data);
    virRotatingFileReaderFree(file);
    virObjectUnlock(handler);
    return NULL;
}


virJSONValuePtr
virLogHandlerPreExecRestart(virLogHandlerPtr handler)
{
    virJSONValuePtr ret = virJSONValueNewObject();
    virJSONValuePtr files;
    size_t i;

    if (!ret)
        return NULL;

    if (!(files = virJSONValueNewArray()))
        goto error;

    if (virJSONValueObjectAppend(ret, "files", files) < 0) {
        virJSONValueFree(files);
        goto error;
    }

    for (i = 0; i < handler->nfiles; i++) {
        virJSONValuePtr file = virJSONValueNewObject();
        if (!file)
            goto error;

        if (virJSONValueArrayAppend(files, file) < 0) {
            virJSONValueFree(file);
            goto error;
        }

        if (virJSONValueObjectAppendNumberInt(file, "pipefd",
                                              handler->files[i]->pipefd) < 0)
            goto error;

        if (virJSONValueObjectAppendString(file, "path",
                                           virRotatingFileWriterGetPath(handler->files[i]->file)) < 0)
            goto error;

        if (virSetInherit(handler->files[i]->pipefd, true) < 0) {
            virReportSystemError(errno, "%s",
                                 _("Cannot disable close-on-exec flag"));
            goto error;
        }
    }

    return ret;

 error:
    virJSONValueFree(ret);
    return NULL;
}