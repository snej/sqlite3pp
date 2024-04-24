// sqnice/database.cc
//
// The MIT License
//
// Copyright (c) 2015 Wongoo Lee (iwongu at gmail dot com)
// Copyright (c) 2024 Jens Alfke (Github: snej)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


#include "sqnice/database.hh"
#include "sqnice/query.hh"
#include "sqnice/statement_cache.hh"
#include <cstring>
#include <cassert>
#include <cstdio>

#ifdef SQNICE_LOADABLE_EXTENSION
#  include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#else
#  include <sqlite3.h>
#endif

namespace sqnice {
    using namespace std;

    static_assert(int(status::ok)               == SQLITE_OK);
    static_assert(int(status::error)            == SQLITE_ERROR);
    static_assert(int(status::perm)             == SQLITE_PERM);
    static_assert(int(status::abort)            == SQLITE_ABORT);
    static_assert(int(status::busy)             == SQLITE_BUSY);
    static_assert(int(status::locked)           == SQLITE_LOCKED);
    static_assert(int(status::readonly)         == SQLITE_READONLY);
    static_assert(int(status::interrupt)        == SQLITE_INTERRUPT);
    static_assert(int(status::ioerr)            == SQLITE_IOERR);
    static_assert(int(status::corrupt)          == SQLITE_CORRUPT);
    static_assert(int(status::cantopen)         == SQLITE_CANTOPEN);
    static_assert(int(status::constraint)       == SQLITE_CONSTRAINT);
    static_assert(int(status::mismatch)         == SQLITE_MISMATCH);
    static_assert(int(status::misuse)           == SQLITE_MISUSE);
    static_assert(int(status::auth)             == SQLITE_AUTH);
    static_assert(int(status::range)            == SQLITE_RANGE);
    static_assert(int(status::done)             == SQLITE_DONE);
    static_assert(int(status::row)              == SQLITE_ROW);

    static_assert(int(open_flags::readonly)     == SQLITE_OPEN_READONLY);
    static_assert(int(open_flags::readwrite)    == SQLITE_OPEN_READWRITE);
    static_assert(int(open_flags::create)       == SQLITE_OPEN_CREATE);
    static_assert(int(open_flags::uri)          == SQLITE_OPEN_URI);
    static_assert(int(open_flags::memory)       == SQLITE_OPEN_MEMORY);
    static_assert(int(open_flags::nomutex)      == SQLITE_OPEN_NOMUTEX);
    static_assert(int(open_flags::fullmutex)    == SQLITE_OPEN_FULLMUTEX);
    static_assert(int(open_flags::nofollow)     == SQLITE_OPEN_NOFOLLOW);
#ifdef __APPLE__
    static_assert(int(open_flags::fileprotection_complete)
                  == SQLITE_OPEN_FILEPROTECTION_COMPLETE);
    static_assert(int(open_flags::fileprotection_complete_unless_open)
                  == SQLITE_OPEN_FILEPROTECTION_COMPLETEUNLESSOPEN);
    static_assert(int(open_flags::fileprotection_complete_until_auth) 
                  == SQLITE_OPEN_FILEPROTECTION_COMPLETEUNTILFIRSTUSERAUTHENTICATION);
    static_assert(int(open_flags::fileprotection_none) 
                  == SQLITE_OPEN_FILEPROTECTION_NONE);
#endif

    static_assert(int(limit::row_length)        == SQLITE_LIMIT_LENGTH);
    static_assert(int(limit::sql_length)        == SQLITE_LIMIT_SQL_LENGTH);
    static_assert(int(limit::columns)           == SQLITE_LIMIT_COLUMN);
    static_assert(int(limit::function_args)     == SQLITE_LIMIT_FUNCTION_ARG);
    static_assert(int(limit::worker_threads)    == SQLITE_LIMIT_WORKER_THREADS);


    database_error::database_error(char const* msg, status rc)
    : std::runtime_error(msg)
    , error_code(rc) {
    }


    checking::checking(database &db)
    :checking(db.db_, db.exceptions_)
    { }


    std::shared_ptr<sqlite3> checking::check_get_db() const {
        if (shared_ptr<sqlite3> db = weak_db_.lock()) [[likely]]
            return db;
        else
            throw logic_error("database is no longer open");
    }


    status checking::check(status rc) const {
        if (!ok(rc)) [[unlikely]] {
            if (exceptions_ || rc == status::misuse)
                if (rc != status::done && rc != status::row)
                    raise(rc);
        }
        return rc;
    }


    void checking::raise(status rc) const {
        if (auto db = weak_db_.lock())
            raise(rc, sqlite3_errmsg(db.get()));
        else
            raise(rc, "");
    }

    void checking::raise(status rc, const char* msg) {
        switch (int(rc)) {
            case SQLITE_INTERNAL:
                throw std::logic_error(msg);
            case SQLITE_NOMEM:
                throw std::bad_alloc();
            case SQLITE_RANGE:
            case SQLITE_MISUSE:
                throw std::invalid_argument(msg);
            case SQLITE_OK:
            case SQLITE_NOTICE:
            case SQLITE_WARNING:
            case SQLITE_ROW:
            case SQLITE_DONE:
                throw std::logic_error("invalid call to throw_, err=" + std::to_string(int(rc)));
            default:        
                throw database_error(msg, rc);
        }
    }


    void checking::log_warning(const char* format, ...) noexcept {
        va_list args;
        va_start(args, format);
        char* message = sqlite3_vmprintf(format, args);
        va_end(args);
        sqlite3_log(SQLITE_WARNING, message);
        sqlite3_free(message);
    }


#pragma mark - DATABASE:


    database::database() noexcept
    : checking(kExceptionsByDefault)
    { }

    database::database(std::string_view dbname, open_flags flags, char const* vfs)
    : checking(kExceptionsByDefault)
    {
        connect(dbname, flags, vfs);
        weak_db_ = db_;
    }

    database::database(sqlite3* pdb) noexcept
    : checking(kExceptionsByDefault)
    , db_(pdb, [](sqlite3*) { })    // do nothing when the last shared_ptr ref is gone
    {
        weak_db_ = db_;
    }

    database::database(database&& db) noexcept
    : checking(db.exceptions_)
    , db_(std::move(db.db_))
    , bh_(std::move(db.bh_))
    , ch_(std::move(db.ch_))
    , rh_(std::move(db.rh_))
    , uh_(std::move(db.uh_))
    , ah_(std::move(db.ah_))
    {
        weak_db_ = db_;
        db.db_ = nullptr;
    }

    database& database::operator=(database&& db) noexcept {
        static_cast<checking&>(*this) = static_cast<checking&&>(db);
        db_ = std::move(db.db_);
        db.db_ = nullptr;
        bh_ = std::move(db.bh_);
        ch_ = std::move(db.ch_);
        rh_ = std::move(db.rh_);
        uh_ = std::move(db.uh_);
        ah_ = std::move(db.ah_);

        return *this;
    }

    database::~database() noexcept = default;

    database database::temporary(bool on_disk) {
        // "If the filename is an empty string, then a private, temporary on-disk database will
        // be created [and] automatically deleted as soon as the database connection is closed."
        std::string_view name;
        open_flags flags = open_flags::readwrite;
        if (!on_disk) {
            name = "temporary";
            flags |= open_flags::memory;
        }
        return database(name, flags);
    }

    status database::connect(std::string_view dbname_, open_flags flags, char const* vfs) {
        close();

        if (!!(flags & open_flags::memory) && !(flags & (open_flags::readwrite | open_flags::readonly)))
            flags |= open_flags::readwrite;

        std::string dbname(dbname_);
        // "It is recommended that when a database filename actually does begin with a ":" character
        // you should prefix the filename with a pathname such as "./" to avoid ambiguity."
        if (dbname.starts_with(":") && dbname != ":memory:" && !(flags & open_flags::uri))
            dbname = "./" + dbname; //FIXME: Is this OK on Windows?

        sqlite3* db = nullptr;
        auto rc = status{sqlite3_open_v2(dbname.c_str(), &db,
                                         int(flags) | SQLITE_OPEN_EXRESCODE,
                                         vfs)};
        if (ok(rc)) {
            // deleter function for the shared_ptr:
            auto close_db = [](sqlite3* db) {
                auto rc = status{sqlite3_close(db)};
                if (rc == status::busy) {
                    fprintf(stderr, "**SQLITE WARNING**: A `sqnice::database` object at %p"
                            "is being destructed while there are still open query iterators, blob"
                            " streams or backups. This is bad! (For more information, read the docs for"
                            "`sqnice::database::close`.)\n", (void*)db);
                    sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
                    (void)sqlite3_close_v2(db);
                }
            };
            db_ = std::shared_ptr<sqlite3>(db, close_db);
            
        } else {
            if (exceptions_) {
                std::string message;
                if (db) {
                    message = sqlite3_errmsg(db);
                    (void)sqlite3_close_v2(db);
                } else {
                    message = "can't open database";
                }
                raise(rc, message.c_str());
            } else {
                (void)sqlite3_close_v2(db);
            }
        }
        return check(rc);
    }

    status database::close(bool immediately) {
        commands_.reset();
        queries_.reset();

        if (db_) {
            if (db_.use_count() > 1)
                return check(status::busy);
            db_ = nullptr;
        }
        return status::ok;
    }

    sqlite3* database::check_handle() const {
        if (!db_) [[unlikely]]
            throw std::logic_error("database is not open");
        return db_.get();
    }

    status database::execute(std::string_view sql) {
        auto rc = status{sqlite3_exec(check_handle(), std::string(sql).c_str(), nullptr, nullptr, nullptr)};
        if (rc == status::error && exceptions_)
            throw std::invalid_argument(error_msg());
        return check(rc);
    }

    status database::executef(char const* sql, ...) {
        va_list ap;
        va_start(ap, sql);
        std::shared_ptr<char> msql(sqlite3_vmprintf(sql, ap), sqlite3_free);
        va_end(ap);

        return execute(msql.get());
    }


    command database::command(std::string_view sql) {
        if (!commands_)
            commands_ = std::make_unique<command_cache>(*this);
        return commands_->compile(std::string(sql));
    }

    query database::query(std::string_view sql) {
        if (!queries_)
            queries_ = std::make_unique<query_cache>(*this);
        return queries_->compile(std::string(sql));
    }


#pragma mark - DATABASE CONFIGURATION:


    std::tuple<int,int,int> database::sqlite_version() noexcept {
        auto v = sqlite3_libversion_number();
        return {v / 1'000'000, v / 1'000, v % 1'000};
    }

    status database::enable_foreign_keys(bool enable) {
        return check(sqlite3_db_config(check_handle(), 
                                       SQLITE_DBCONFIG_ENABLE_FKEY, enable ? 1 : 0, nullptr));
    }

    status database::enable_triggers(bool enable) {
        return check(sqlite3_db_config(check_handle(),
                                       SQLITE_DBCONFIG_ENABLE_TRIGGER, enable ? 1 : 0, nullptr));
    }

    status database::set_busy_timeout(int ms) {
        return check(sqlite3_busy_timeout(check_handle(), ms));
    }

    status database::setup() {
        status rc = enable_foreign_keys();
        if (!ok(rc)) {return rc;}
        rc = set_busy_timeout(5000);
        if (ok(rc) && writeable()) {
            rc = execute("PRAGMA auto_vacuum = incremental;" // must be the first statement executed
                         "PRAGMA journal_mode = WAL;"
                         "PRAGMA synchronous=normal");
        }
        return rc;
    }


    int64_t database::pragma(const char* pragma) {
        return sqnice::query(*this, std::string("PRAGMA \"") + pragma + "\"").single_value_or<int>(0);
    }

    std::string database:: string_pragma(const char* pragma) {
        return sqnice::query(*this, std::string("PRAGMA \"") + pragma + "\"").single_value_or<std::string>("");
    }

    status database::pragma(const char* pragma, int64_t value) {
        return executef("PRAGMA %s(%d)", pragma, value);
    }

    status database::pragma(const char* pragma, std::string_view value) {
        return executef("PRAGMA %s(%q)", pragma, std::string(value).c_str());
    }


    unsigned database::get_limit(limit lim) const noexcept {
        return sqlite3_limit(check_handle(), int(lim), -1);
    }

    unsigned database::set_limit(limit lim, unsigned val) noexcept {
        return sqlite3_limit(check_handle(), int(lim), int(val));
    }


#pragma mark - DATABASE PROPERTIES:


    const char* database::filename() const noexcept {
        return sqlite3_db_filename(check_handle(), nullptr);
    }

    status database::error_code() const noexcept {
        return status{sqlite3_errcode(check_handle())};
    }

    status database::extended_error_code() const noexcept {
        return status{sqlite3_extended_errcode(check_handle())};
    }

    char const* database::error_msg() const noexcept {
        return sqlite3_errmsg(check_handle());
    }

    bool database::writeable() const noexcept {
        return ! sqlite3_db_readonly(check_handle(), "main");
    }

    int64_t database::last_insert_rowid() const noexcept {
        return sqlite3_last_insert_rowid(check_handle());
    }

    int database::changes() const noexcept {
        return sqlite3_changes(check_handle());
    }

    int64_t database::total_changes() const noexcept {
        return sqlite3_total_changes(check_handle());
    }

    bool database::in_transaction() const noexcept {
        return !sqlite3_get_autocommit(check_handle());
    }


#pragma mark - TRANSACTIONS:


    status database::beginTransaction(bool immediate) {
        if (txn_depth_ == 0) {
            if (immediate) {
                if (in_transaction())
                    throw std::logic_error("unexpectedly already in a transaction");
                // Create an immediate txn, otherwise SAVEPOINT defaults to DEFERRED
                if (auto rc = command("BEGIN IMMEDIATE").execute(); !ok(rc))
                    return rc;
            }
            txn_immediate_ = immediate;
        }

        char sql[30];
        snprintf(sql, sizeof(sql), "SAVEPOINT sp_%d", txn_depth_ + 1);
        if (auto rc = command(sql).execute(); !ok(rc)) {
            if (txn_depth_ == 0 && immediate)
                (void)command("ROLLBACK").execute();
            return rc;
        }

        ++txn_depth_;
        return status::ok;
    }


    status database::endTransaction(bool commit) {
        if (txn_depth_ <= 0) [[unlikely]]
            throw std::logic_error("transaction underflow");
        char sql[50];
        if (!commit) {
            /// "Instead of cancelling the transaction, the ROLLBACK TO command restarts the
            /// transaction again at the beginning. All intervening SAVEPOINTs are canceled,
            /// however." --https://sqlite.org/lang_savepoint.html
            snprintf(sql, sizeof(sql), "ROLLBACK TO SAVEPOINT sp_%d", txn_depth_);
            if (auto rc = command(sql).execute(); !ok(rc))
                return rc;
            /// Thus we also have to call RELEASE to pop the savepoint from the stack...
        }
        snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT sp_%d", txn_depth_);
        if (auto rc = command(sql).execute(); !ok(rc))
            return rc;

        --txn_depth_;
        if (txn_depth_ == 0) {
            if (txn_immediate_) {
                if (!in_transaction())
                    throw std::logic_error("unexpectedly not in a transaction");
                if (auto rc = command(commit ? "COMMIT" : "ROLLBACK").execute(); !ok(rc)) {
                    ++txn_depth_;
                    return rc;
                }
            }
        }
        return status::ok;
    }


#pragma mark - BACKUP:


    status database::backup(database& destdb, backup_handler h) {
        return backup("main", destdb, "main", h);
    }

    status database::backup(std::string_view dbname,
                            database& destdb, 
                            std::string_view destdbname,
                            backup_handler handler,
                            int step_page)
    {
        auto rc = status::ok;
        sqlite3_backup* bkup = sqlite3_backup_init(destdb.check_handle(),
                                                   std::string(destdbname).c_str(),
                                                   check_handle(),
                                                   std::string(dbname).c_str());
        if (!bkup) {
            // "If an error occurs within sqlite3_backup_init, then ... an error code and error
            // message are stored in the destination database connection"
            rc = destdb.extended_error_code();
            if (exceptions_)
                raise(rc, destdb.error_msg());
            return rc;
        }

        // Run the backup incrementally:
        do {
            rc = status{sqlite3_backup_step(bkup, step_page)};
            if (handler)
                handler(sqlite3_backup_remaining(bkup), sqlite3_backup_pagecount(bkup), rc);
        } while (rc == status::ok || rc == status::busy || rc == status::locked);

        // Finish:
        auto end_rc = status{sqlite3_backup_finish(bkup)};
        if (rc == status::done)
            rc = end_rc;
        return check(rc);
    }


    status database::optimize() {
        /* "The optimize pragma is usually a no-op but it will occasionally run ANALYZE if it
         seems like doing so will be useful to the query planner. The analysis_limit pragma
         limits the scope of any ANALYZE command that the optimize pragma runs so that it does
         not consume too many CPU cycles. The constant "400" can be adjusted as needed. Values
         between 100 and 1000 work well for most applications."
         -- <https://sqlite.org/lang_analyze.html> */
        if (!writeable())
            return status::ok;
        status rc = pragma("analysis_limit", 400);
        if (ok(rc))
            rc = pragma("optimize", 0xfffe);
        return rc;
    }


    // If this fraction of the database is composed of free pages, vacuum it on close
    static constexpr float kVacuumFractionThreshold = 0.25;
    // If the database has many bytes of free space, vacuum it on close
    static constexpr int64_t kVacuumSizeThreshold = 10'000'000;


    std::optional<int64_t> database::incremental_vacuum(bool always, int64_t nPages) {
        // <https://blogs.gnome.org/jnelson/2015/01/06/sqlite-vacuum-and-auto_vacuum/>
        if (!writeable())
            return std::nullopt;
        int64_t pageCount = pragma("page_count");
        bool do_it = always;
        if (!always) {
            int64_t freePages = pragma("freelist_count");
            float freeFraction = pageCount ? (float)freePages / pageCount : 0;
            do_it = freeFraction >= kVacuumFractionThreshold
                    || freePages * pragma("page_size") >= kVacuumSizeThreshold;
        }
        if (!do_it)
            return std::nullopt;

        pragma("incremental_vacuum", nPages);
        if (always) {
            // On explicit compact, truncate the WAL file to save more disk space:
            pragma("wal_checkpoint", "TRUNCATE");
        }
        return pageCount - pragma("page_count");
    }



#pragma mark - HOOKS:

    
    namespace {
        int busy_handler_impl(void* p, int attempts) noexcept {
            auto h = static_cast<database::busy_handler*>(p);
            return (*h)(attempts);
        }

        int commit_hook_impl(void* p) noexcept {
            auto h = static_cast<database::commit_handler*>(p);
            return int((*h)());
        }

        void rollback_hook_impl(void* p) noexcept {
            auto h = static_cast<database::rollback_handler*>(p);
            (*h)();
        }

        void update_hook_impl(void* p, int opcode, char const* dbname, char const* tablename,
                              long long int rowid) noexcept {
            auto h = static_cast<database::update_handler*>(p);
            (*h)(opcode, dbname, tablename, rowid);
        }

        int authorizer_impl(void* p, int action, char const* p1, char const* p2,
                            char const* dbname, char const* tvname) noexcept {
            auto h = static_cast<database::authorize_handler*>(p);
            return int((*h)(action, p1, p2, dbname, tvname));
        }

    } // namespace


    void database::set_log_handler(log_handler h) noexcept {
        static log_handler sLogHandler;

        sLogHandler = std::move(h);
        
        auto callback = [](void* p, int errCode, const char* msg) noexcept {
            if ( (errCode & 0xFF) == SQLITE_SCHEMA )
                return;  // ignore harmless "statement aborts ... database schema has changed"
            if (sLogHandler)
                sLogHandler(status(errCode), msg);
        };
        sqlite3_config(SQLITE_CONFIG_LOG, h ? callback : nullptr, nullptr);
    }

    void database::set_busy_handler(busy_handler h) noexcept {
        bh_ = h;
        sqlite3_busy_handler(check_handle(), bh_ ? busy_handler_impl : nullptr, &bh_);
    }

    void database::set_commit_handler(commit_handler h) noexcept {
        ch_ = h;
        sqlite3_commit_hook(check_handle(), ch_ ? commit_hook_impl : nullptr, &ch_);
    }

    void database::set_rollback_handler(rollback_handler h) noexcept {
        rh_ = h;
        sqlite3_rollback_hook(check_handle(), rh_ ? rollback_hook_impl : nullptr, &rh_);
    }

    void database::set_update_handler(update_handler h) noexcept {
        uh_ = h;
        sqlite3_update_hook(check_handle(), uh_ ? update_hook_impl : nullptr, &uh_);
    }

    void database::set_authorize_handler(authorize_handler h) noexcept {
        ah_ = h;
        sqlite3_set_authorizer(check_handle(), ah_ ? authorizer_impl : nullptr, &ah_);
    }

}
