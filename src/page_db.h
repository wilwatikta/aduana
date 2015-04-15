#ifndef __PAGE_DB_H__
#define __PAGE_DB_H__

#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE 1
#define _GNU_SOURCE 1

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "lmdb.h"
#include "xxhash.h"

#include "hits.h"
#include "link_stream.h"
#include "page_rank.h"

#define KB 1024LL
#define MB (1024*KB)
#define GB (1024*MB

/// @addtogroup CrawledPage
/// @{
/** The information that comes with a link inside a crawled page.
 *
 * The link score is used to decide which links should be crawled next. It
 * is application dependent and tipically computed by looking at the link
 * surrounding text.
 */
typedef struct {
     char *url;   /**< ASCII, null terminated string for the page URL*/
     float score; /**< An estimated value of the link score */
} LinkInfo;

/** Allocate at least this amount of memory for link info */
#define PAGE_LINKS_MIN_LINKS 10

/** A (resizable) array of page links.
 *
 * Initially:
 *   n_links = 0
 *   m_links = PAGE_LINKS_MIN_LINKS
 *
 * Always:
 *   0 <= n_links <= m_links
 *
 * */
typedef struct {
     LinkInfo *link_info; /**< Array of LinkInfo */
     size_t n_links;      /**< Number of items inside link_info */
     size_t m_links;      /**< Maximum number of items that can be stored inside link_info */
} PageLinks;

/** The information that comes with a crawled page */
typedef struct {
     char *url;                   /**< ASCII, null terminated string for the page URL*/
     PageLinks *links;            /**< List of links inside this page */
     double time;                 /**< Number of seconds since epoch */
     float score;                 /**< A number giving an idea of the page content's value */
     char *content_hash;          /**< A hash to detect content change since last crawl.
                                       Arbitrary byte sequence */
     size_t content_hash_length;  /**< Number of byes of the content_hash */
} CrawledPage;

/** Create a new CrawledPage

    url is a new copy

    The following defaults are used for the different fields:
    - links: no links initially. Use crawled_page_add_link to add some.
    - time: current time
    - score: 0. It can be setted directly.
    - content_hash: NULL. Use crawled_page_set_hash to change

    @return NULL if failure, otherwise a newly allocated CrawledPage
*/
CrawledPage *
crawled_page_new(const char *url);

/** Delete a Crawled Page created with @ref crawled_page_new */
void
crawled_page_delete(CrawledPage *cp);

/** Set content hash
 *
 * The hash is a new copy
 */
int
crawled_page_set_hash(CrawledPage *cp, const char *hash, size_t hash_length);

/** Set content hash from a 128bit hash */
int
crawled_page_set_hash128(CrawledPage *cp, char *hash);

/** Set content hash from a 64bit hash */
int
crawled_page_set_hash64(CrawledPage *cp, uint64_t hash);

/** Set content hash from a 32bit hash */
int
crawled_page_set_hash32(CrawledPage *cp, uint32_t hash);

/** Add a new link to the crawled page */
int
crawled_page_add_link(CrawledPage *cp, const char *url, float score);

/** Get number of links inside page */
size_t
crawled_page_n_links(const CrawledPage *cp);

/** Get a pointer to the link */
const LinkInfo *
crawled_page_get_link(const CrawledPage *cp, size_t i);

/// @}

/// @addtogroup PageInfo
/// @{

/** The information we keep about crawled and uncrawled pages
 *
 * @ref PageInfo are created at the @ref PageDB, that's why there are
 * no public constructors/destructors available.
 */
typedef struct {
     char *url;                   /**< A copy of either @ref CrawledPage::url or @ref CrawledPage::links[i] */
     double first_crawl;          /**< First time this page was crawled */
     double last_crawl;           /**< Last time this page was crawled */
     size_t n_changes;            /**< Number of content changes detected between first and last crawl */
     size_t n_crawls;             /**< Number of times this page has crawled. Can be zero if it has been observed just as a link*/
     float score;                 /**< A copy of the same field at the last crawl */
     size_t content_hash_length;  /**< Number of bytes in @ref content_hash */
     char *content_hash;          /**< Byte sequence with the hash of the last crawl */
} PageInfo;

/** Write printed representation of PageInfo.

    This function is intended mainly for debugging and development.
    The representation is:
        first_crawl last_crawl n_crawls n_changes url

    Each field is separated with an space. The string is null terminated.
    We use the following format for each field:
    - first_crawl: the standard fixed size (24 bytes) as output by ctime.
      For example: Mon Jan 1 08:01:59 2015
    - last_crawl: the same as first_crawl
    - n_crawls: To ensure fixed size representation this value is converted
      to double and represented in exponential notation with two digits. It has
      therefore always 8 bytes length:
            1.21e+01
    - n_changes: The same as n_crawls
    - url: This is the only variable length field. However, it is truncated at
      512 bytes length.

    @param pi The @ref PageInfo to be printed
    @param out The output buffer, which must be at least 580 bytes long
    @return size of representation or -1 if error
*/
int
page_info_print(const PageInfo *pi, char *out);

/** Serialize the PageInfo into a contiguos block of memory.
 *
 * @param pi The PageInfo to be serialized
 * @param val The destination of the serialization
 *
 * @return 0 if success, -1 if failure.
 */
int
page_info_dump(const PageInfo *pi, MDB_val *val);

/** Create a new PageInfo loading the information from a previously
 * dumped PageInfo inside val.
 *
 * @return pointer to the new PageInfo or NULL if failure
 */
PageInfo *
page_info_load(const MDB_val *val);

/** Estimate change rate of the given page */
float
page_info_rate(const PageInfo *pi);

/** Destroy PageInfo if not NULL, otherwise does nothing */
void
page_info_delete(PageInfo *pi);

struct PageInfoList {
     uint64_t hash;
     PageInfo *page_info;
     struct PageInfoList *next;
};
typedef struct PageInfoList PageInfoList;

PageInfoList *
page_info_list_new(PageInfo *pi, uint64_t hash);

PageInfoList *
page_info_list_cons(PageInfoList *pil, PageInfo *pi, uint64_t hash);

void
page_info_list_delete(PageInfoList *pil);

/// @}

/// @addtogroup PageDB
/// @{
#define PAGE_DB_MAX_ERROR_LENGTH 10000
#define PAGE_DB_DEFAULT_SIZE 100*MB /**< Initial size of the mmap region */

typedef enum {
     page_db_error_ok = 0,       /**< No error */
     page_db_error_memory,       /**< Error allocating memory */
     page_db_error_invalid_path, /**< File system error */
     page_db_error_internal,     /**< Unexpected error */
     page_db_error_no_page       /**< A page was requested but could not be found */
} PageDBError;


// TODO Make the building of the links database optional. The are many more links
// that pages and it takes lot of space to store this structure. We should only
// build the links database if we are going to use them, for example, to compute
// PageRank or HITS scores.

/** Page database.
 *
 * We are really talking about 5 diferent key/value databases:
 *   - info
 *        Contains fixed size information about the whole database. Right now
 *        it just contains the number pages stored.
 *   - hash2idx
 *        Maps URL hash to index. Indices are consecutive identifier for every
 *        page. This allows to map pages to elements inside arrays.
 *   - hash2info
 *        Maps URL hash to a @ref PageInfo structure.
 *   - links
 *        Maps URL index to links indices. This allows us to make a fast streaming
 *        of all links inside a database.
 */
typedef struct {
     char *path;
     MDB_env *env;
     Error error;
} PageDB;


/** Creates a new database and stores data inside path
 *
 * @param db In case of @ref ::page_db_error_memory *db could be NULL, otherwise
 *           it is allocated memory so that the @ref PageDB::error_msg can be
 *           accessed and its your responsability to call @ref page_db_delete.
 *
 * @param path Path to directory. In case it doesn't exist it will created.
 *             If it exists and a database is already present operations will
 *             resume with the existing database. Note that you must have read,
 *             write and execute permissions for the directory.
 *
 * @return 0 if success, otherwise the error code
 **/
PageDBError
page_db_new(PageDB **db, const char *path);

/** Update @ref PageDB with a new crawled page
 *
 * It perform the following actions:
 * - Compute page hash
 * - If the page is not already into the database:
 *     - It generates a new ID and stores it in hash2idx
 *     - It creates a new PageInfo and stores it in hash2info
 * - If already present if updates the PageInfo inside hash2info
 * - For each link:
 *     - Compute hash
 *     - If already present in the database just retrieves the ID
 *     - If not present:
 *         - Generate new ID and store it in hash2idx
 *         - Creates a new PageInfo and stores it in hash2info
 * - Create or overwrite list of Page ID -> Links ID mapping inside links
 *   database
 *
 * @return 0 if success, otherwise the error code
 */
PageDBError
page_db_add(PageDB *db, const CrawledPage *page, PageInfoList **page_info_list);

/** Retrieve the PageInfo stored inside the database.

    Beware that if not found it will signal success but the PageInfo will be
    NULL
 */
PageDBError
page_db_get_info_from_url(PageDB *db, const char *url, PageInfo **pi);

/** Retrieve the PageInfo stored inside the database.

    Beware that if not found it will signal success but the PageInfo will be
    NULL
 */
PageDBError
page_db_get_info_from_hash(PageDB *db, uint64_t hash, PageInfo **pi);

/** Get index for the given URL */
PageDBError
page_db_get_idx(PageDB *db, const char *url, uint64_t *idx);

/** Open a new cursor inside the database */
int
page_db_open_cursor(MDB_txn *txn,
                    const char *db_name,
                    int flags,
                    MDB_cursor **cursor,
                    MDB_cmp_func *func);

int
page_db_open_hash2info(MDB_txn *txn, MDB_cursor **cursor);

int
page_db_open_hash2idx(MDB_txn *txn, MDB_cursor **cursor);

int
page_db_open_links(MDB_txn *txn, MDB_cursor **cursor);

int
page_db_open_info(MDB_txn *txn, MDB_cursor **cursor);

/** Close database */
void
page_db_delete(PageDB *db);

/** Compute, or update, the HITS scores */
PageDBError
page_db_update_hits(PageDB *db);

/** Compute, or update, the PageRank scores */
PageDBError
page_db_update_page_rank(PageDB *db);
/// @}

/// @addtogroup LinkStream
/// @{


typedef struct {
     MDB_cursor *cur; /**< Cursor to the links database */

     uint64_t from; /**< Current page */
     uint64_t *to;  /**< A list of links */
     size_t n_to;   /**< Number of links */
     size_t i_to;   /**< Current position inside @ref to */
     size_t m_to;   /**< Allocated memory for @ref to. It must be that @ref n_to <= @ref m_to. */

     LinkStreamState state;
} PageDBLinkStream;

/** Create a new stream from the given PageDB.
 *
 * @param es The new stream or NULL
 * @param db
 * @return 0 if success, otherwise the error code.
 */
PageDBError
page_db_link_stream_new(PageDBLinkStream **es, PageDB *db);

/** Rewind stream to the beginning */
LinkStreamState
page_db_link_stream_reset(void *es);

/** Get next element inside stream.
 *
 * @return @ref ::link_stream_state_next if success
 */
LinkStreamState
page_db_link_stream_next(void *es, Link *link);

/** Delete link stream and free any transaction hold inside the database. */
void
page_db_link_stream_delete(PageDBLinkStream *es);

/// @}

#if (defined TEST) && TEST
#include "CuTest.h"
CuSuite *
test_page_db_suite(void);
#endif

#endif // __PAGE_DB_H