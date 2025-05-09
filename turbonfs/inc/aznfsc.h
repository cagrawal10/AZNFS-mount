#ifndef __AZNFSC_H__
#define __AZNFSC_H__

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>

#ifdef ENABLE_NO_FUSE
#include "nofuse.h"
#else
#define FUSE_USE_VERSION 312
#include <fuse3/fuse_lowlevel.h>
#include <fuse3/fuse.h>
#include <linux/fuse.h>
#endif

#include "libnfs.h"
#include "libnfs-raw.h"
#include "libnfs-raw-mount.h"
#include "libnfs-raw-nfs.h"

#include "aznfsc_config.h"
#include "log.h"
#include "util.h"

using namespace aznfsc;

// Max block size for a Blob (100MB).
#define AZNFSC_MAX_BLOCK_SIZE   (100 * 1024 * 1024)

// Min/Max values for various aznfsc_cfg options.
#define AZNFSCFG_NCONNECT_MIN   1
#define AZNFSCFG_NCONNECT_MAX   256
#define AZNFSCFG_TIMEO_MIN      100
#define AZNFSCFG_TIMEO_MAX      6000
#define AZNFSCFG_RSIZE_MIN      1048576
#define AZNFSCFG_RSIZE_MAX      AZNFSC_MAX_BLOCK_SIZE
#define AZNFSCFG_WSIZE_MIN      1048576
#define AZNFSCFG_WSIZE_MAX      AZNFSC_MAX_BLOCK_SIZE
static_assert(AZNFSCFG_WSIZE_MAX == AZNFSCFG_RSIZE_MAX);
#define AZNFSCFG_READDIR_MIN    8192
#define AZNFSCFG_READDIR_MAX    4194304
#define AZNFSCFG_READAHEAD_KB_MIN 128
#define AZNFSCFG_READAHEAD_KB_MAX 1048576
#define AZNFSCFG_READAHEAD_KB_DEF 16384
#define AZNFSCFG_FUSE_MAX_BG_MIN 1
#define AZNFSCFG_FUSE_MAX_BG_MAX 65536
#define AZNFSCFG_FUSE_MAX_BG_DEF 4096
#define AZNFSCFG_FUSE_MAX_THR_MIN -1 // Implies fuse default.
#define AZNFSCFG_FUSE_MAX_THR_MAX 65536
#define AZNFSCFG_FUSE_MAX_IDLE_THR_MIN -1 // Implies fuse default.
#define AZNFSCFG_FUSE_MAX_IDLE_THR_MAX INT_MAX
#define AZNFSCFG_CACHE_MAX_MB_MIN 512
#define AZNFSCFG_CACHE_MAX_MB_MAX (10 * 1024 * 1024)
// Default value for percentage of total RAM to be used for cache.
#define AZNFSCFG_CACHE_MAX_MB_PERCENT_DEF 60
#define AZNFSCFG_FILECACHE_MAX_GB_MIN 1
#define AZNFSCFG_FILECACHE_MAX_GB_MAX (1024 * 1024)
#define AZNFSCFG_FILECACHE_MAX_GB_DEF (1024)
#define AZNFSCFG_RETRANS_MIN    1
#define AZNFSCFG_RETRANS_MAX    100
#define AZNFSCFG_ACTIMEO_MIN    1
#define AZNFSCFG_ACTIMEO_MAX    3600
#define AZNFSCFG_LOOKUPCACHE_NONE   1
#define AZNFSCFG_LOOKUPCACHE_POS    2
#define AZNFSCFG_LOOKUPCACHE_ALL    3
#define AZNFSCFG_LOOKUPCACHE_DEF    AZNFSCFG_LOOKUPCACHE_ALL

// W/o jumbo blocks, 5TiB is the max file size we can support.
#define AZNFSC_MAX_FILE_SIZE    (50'000ULL * AZNFSC_MAX_BLOCK_SIZE)

/*
 * Max fuse_opcode enum value.
 * This keeps increasing with newer fuse versions, but we don't want it
 * to be the exact maximum, we just want it to be more than all the opcodes
 * that we support.
 */
#define FUSE_OPCODE_MAX         FUSE_LSEEK

/*
 * fuse_reply_iov() uses writev() for sending the iov over to the fuse
 * device. writev() can accept max 1024 sized vector, and fuse_reply_iov()
 * uses the first element of the vector for conveying the req id and status,
 * so we cannot convey more than 1023 vector elements through fuse_reply_iov().
 */
#define FUSE_REPLY_IOV_MAX_COUNT (1023)

/*
 * In paranoid builds, also enable pressure points (aka error injection).
 */
#ifdef ENABLE_PARANOID
#define ENABLE_PRESSURE_POINTS
extern double inject_err_prob_pct_def;
#endif

/**
 * Enum for defining the various consistency levels we support.
 * Ref details in sample-config.yaml.
 */
enum class consistency_t
{
    INVALID = 0,
    SOLOWRITER,
    STANDARDNFS,
    AZUREMPA,
};

/**
 * This structure holds the entire aznfsclient configuration that controls the
 * behaviour of the aznfsclient fuse program. These config variables can be
 * configured in many ways, allowing user to conveniently express their default
 * configuration and allowing easy overrides for some as needed.
 *
 * Here are the various ways these config values are populated:
 * 1. Most configs have default values.
 *    Note: Some of the config variables pertain to user details and cannot
 *          have default values.
 * 2. Convenient place for defining config variables which don't need to be
 *    changed often is the config.yaml file that user can provide with the
 *    --config-file=./config.yaml cmdline option to aznfsclient.
 *    These override the defaults.
 * 3. Some but not all config variables can be set using environment variables.
 *    These override the variables set by config.yaml and the default.
 * 4. Most config variables can be set using specific command line options to
 *    aznfsclient.
 *    These have the highest preference and will override the variables set
 *    by environment variables, config.yaml and the default.
 *
 * Note: This MUST not contains C++ object types as members as fuse parser
 *       writes into those members. For char* members fuse also allocates
 *       memory.
 *       An exception to this are the fields in the "Aggregates" section.
 *       These are not set by fuse parser but are stored for convenience.
 */
typedef struct aznfsc_cfg
{
    // config.yaml file path specified using --config-file= cmdline option.
    const char *config_yaml = nullptr;

    // Enable debug logging?
    bool debug = false;

    /*************************************************
     **                Mount path                   **
     ** Identify the server and the export to mount **
     *************************************************/

    /*
     * Storage account and container to mount and the optional cloud suffix.
     * The share path mounted is:
     * <account>.<cloud_suffix>:/<account>/<container>
     */
    const char *account = nullptr;
    const char *container = nullptr;
    const char *cloud_suffix = nullptr;

    /*************************************************
     **                   Misc                      **
     *************************************************/

    
    /**********************************************************************
     **                          Auth config                             **
     **********************************************************************/

    /*
     * Whether auth should be performed. If this is set to true, tenant id, 
     * subscription id and authtype should be set. 
     */
    bool auth = false;

    /**********************************************************************
     **                          Mount options                           **
     ** These are deliberately named after the popular NFS mount options **
     **********************************************************************/

    /*
     * NFS and Mount port to use.
     * If this is non-zero, portmapper won't be contacted.
     * Note that Blob NFS uses the same port for Mount and NFS, hence we have
     * just one config.
     */
    int port = -1;

    // Number of connections to be established to the server.
    int nconnect = -1;

    // Maximum size of read request.
    int rsize = -1;

    // Maximum size of write request.
    int wsize = -1;

    /*
     * Number of times the request will be retransmitted to the server when no
     * response is received, before the "server not responding" message is
     * logged and further recovery is attempted.
     */
    int retrans = -1;

    /*
     * Time in deci-seconds we will wait for a response before retrying the
     * request.
     */
    int timeo = -1;

    /*
     * Regular file and directory attribute cache timeout min and max values.
     * min value specifies the minimum time in seconds that we cache the
     * corresponding file type's attributes before we request fresh attributes
     * from the server. A successful attribute revalidation (i.e., mtime
     * remains unchanged) doubles the attribute timeout (up to
     * acregmax/acdirmax for file/directory), while a failed revalidation
     * resets it to acregmin/acdirmin.
     * If actimeo is specified it overrides all ac{reg|dir}min/ac{reg|dir}max
     * and the single actimeo value is used as the min and max attribute cache
     * timeout values for both file and directory types.
     */
    int acregmin = -1;
    int acregmax = -1;
    int acdirmin = -1;
    int acdirmax = -1;
    int actimeo = -1;

    // Whether to cache positive/negative lookup responses.
    const char *lookupcache = nullptr;
    int lookupcache_int = AZNFSCFG_LOOKUPCACHE_DEF;

    // Maximum number of readdir entries that can be requested in a single call.
    int readdir_maxcount = -1;

    // Readahead size in KB.
    int readahead_kb = -1;

    // Fuse max_background config value.
    int fuse_max_background = -1;

    // Fuse max_threads config value.
    int fuse_max_threads = -1;

    // Fuse max_idle_threads config value.
    int fuse_max_idle_threads = -1;

    // Whether to use TLS or not.
    const char *xprtsec = nullptr;

    // Whether to disable OOM killing for the aznfsclient process.
    bool oom_kill_disable = true;

    /*************************************************
     **              Cconsistency config            **
     *************************************************/
    const char *consistency = nullptr;
    consistency_t consistency_int = consistency_t::INVALID;

    /*
     * Convenience shortcuts for consistency mode check.
     */
    bool consistency_solowriter = false;
    bool consistency_standardnfs = false;
    bool consistency_azurempa = false;

    /*************************************************
     **                 Cache config                **
     *************************************************/

    struct {
        struct {
            /*
             * Userspace attribute/lookup cache.
             * To disable kernel attribute cache set actimeo to 0.
             */
            struct {
                bool enable = true;
            } user;
        } attr;

        struct {
            /*
             * Kernel readdir cache.
             */
            struct {
                bool enable = true;
            } kernel;

            /*
             * Userspace readdir cache.
             * This cannot be disabled currently.
             */
            struct {
                const bool enable = true;

                // Max userspace readdir cache size in MB.
                int max_size_mb = -1;
            } user;
        } readdir;

        struct {
            /*
             * Kernel data/page cache.
             */
            struct {
                bool enable = true;
            } kernel;

            /*
             * Userspace data cache.
             * This cannot be disabled as we need it for performing any IO
             * operation.
             */
            struct {
                const bool enable = true;

                // Max userspace data cache size in MB.
                int max_size_mb = -1;
            } user;
        } data;
    } cache;

    struct {
        bool enable = false;

        // Directory where file caches will be persisted.
        const char *cachedir = nullptr;

        // Max filecache size in GB.
        int max_size_gb = -1;
    } filecache;

    /*************************************************
     **           System related config             **
     *************************************************/
     struct {
        /*
         * If set, stable writes will be forced, else we start with unstable
         * write and fallback to stable in case of non-append write pattern.
         */
        bool force_stable_writes = true;

        /*
         * Resolve server name before reconnect, else connect to the last
         * resolved IP.
         */
        bool resolve_before_reconnect = true;

        /*
         * How should we behave when a retransmitted RPC fails possibly due to
         * lack of federated DRC at the server.
         */
        struct {
            /*
             * REMOVE/RMDIR failing with NFS3ERR_NOENT must be treated as
             * success.
             */
            bool remove_noent_as_success = true;

            /*
             * CREATE/MKNOD/MKDIR/SYMLINK failing with NFS3ERR_EXIST must be
             * treated as success.
             */
            bool create_exist_as_success = true;

            /*
             * RENAME failing with NFS3ERR_NOENT must be treated as success.
             */
            bool rename_noent_as_success = true;
        } nodrc;
     } sys;

    /*
     * TODO:
     * - Add auth related config.
     * - Add perf related config,
     * - Add hard/soft mount option,
     *   e.g., amount of RAM used for staging writes, etc.
     */

    /**************************************************************************
     **                            Aggregates                                **
     ** These store composite config variables formed from other config      **
     ** variables which were set as options using aznfsc_opts.               **
     ** These aggregate membets MUST NOT be set as options using aznfsc_opts,**
     ** as these can be C++ objects.                                         **
     **************************************************************************/
    std::string server;
    std::string export_path;

    /**
     * Local mountpoint.
     * This is not present in the config file, but is taken from the
     * cmdline.
     */
    std::string mountpoint;

    /**
     * Parse config_yaml if set by cmdline --config-file=
     */
    bool parse_config_yaml();

    /**
     * Set default values for options not yet assigned.
     * This must be called after fuse_opt_parse() and parse_config_yaml()
     * assign config values from command line and the config yaml file.
     * Also sanitizes various values.
     * Returns false if it cannot set default value for one or more config.
     */
    bool set_defaults_and_sanitize();
} aznfsc_cfg_t;

extern struct aznfsc_cfg aznfsc_cfg;

#endif /* __AZNFSC_H__ */
