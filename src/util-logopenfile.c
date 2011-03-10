/* vi: set et ts=4:
 */
#include <sys/socket.h>
#include <sys/un.h>

#include "suricata-common.h" /* errno.h, string.h, etc. */
#include "conf.h"            /* ConfNode, etc. */
#include "tm-modules.h"      /* LogFileCtx */

FILE * SCLogOpenSocketFp(const char *path)
{
    struct sockaddr_un sun = { AF_UNIX };
    FILE * ret;
    int s = -1;

    s = socket(PF_UNIX, SOCK_STREAM, 0);
    if (s < 0) goto err;

    strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);

    if (connect(s, &sun, sizeof(sun)) < 0)
        goto err;

    ret = fdopen(s, "w");
    if (ret == NULL)
        goto err;

    return ret;

err:
    if (s >= 0) {
        int _errno = errno; /* Hey, you never know. */
        close(s);
        errno = _errno;
    }
    return NULL;
}

FILE * SCLogOpenFileFp(const char *path, const char *mode)
{
    FILE *ret = NULL;

    if (strcasecmp(mode, "yes") == 0) {
        ret = fopen(log_path, "a");
    } else {
        ret = fopen(log_path, "w");
    }

    return ret;
}

int SCConfGenericOutput(ConfNode *conf,
                        LogFileCtx *file_ctx,
                        const char *default_filename)
{
    char log_path[PATH_MAX];
    char *filename, *filetype, *log_dir;

    filename = ConfNodeLookupChildValueDef(conf, "filename",
                                           default_filename);
    filetype = ConfNodeLookupChildValueDef(conf, "type",
                                           DEFAULT_LOG_FILETYPE);
    ConfGetDef("default-log-dir", &log_dir, DEFAULT_LOG_DIR);

    snprintf(log_path, PATH_MAX, "%s/%s", log_dir, filename);

    if (strcasecmp(filetype, "socket") == 0) {
        log_ctx->fp = SCLogOpenSocketFp(log_path);
    } else if (strcasecmp(filetype, DEFAULT_LOG_FILETYPE)) {
        const char *append;

        append = ConfNodeLookupChildValueDef(conf, "append",
                                             DEFAULT_LOG_MODE_APPEND);
        log_ctx = SCLogOpenFileFp(log_path, append);
    } else {
        SCLogError(SC_ERR_CONF, "%s: unrecognized file \"type\" \"%s\"",
                   conf->name, filetype);
        return -1;
    }

    if (log_ctx->fp == NULL)
        SCLogError(SC_ERR_CONF, "%s: error on output \"%s\": %s",
                   conf->name, filename, strerror(errno));

    return log_ctx;
}
