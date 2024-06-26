#include "sqnice_test.hh"
#include "sqnice/functions.hh"
#include "sqnice/pool.hh"

using namespace std;
using namespace std::placeholders;

TEST_CASE_METHOD(sqnice_test, "SQNice insert", "[sqnice]") {
    db.execute("INSERT INTO contacts (name, phone) VALUES ('AAAA', '1234')");

    {
        sqnice::transaction xct;
        xct.begin(db);

        sqnice::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (?, ?)");

        cmd.bind(1, "BBBB");
        cmd.bind(2, "555-1212");
        cmd.execute();

        cmd.execute("CCCC", "555-1313");


        cmd.binder() << "DD" << "555-1414";

        cmd.execute();

        xct.commit();
    }

    {
        sqnice::transaction xct(db, true);

        sqnice::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (:name, :name)");

        cmd[":name"] = "DDDD";

        cmd.execute();
    }
}

TEST_CASE_METHOD(sqnice_test, "SQNice insert_execute", "[sqnice]") {
    db.execute("INSERT INTO contacts (name, phone) VALUES ('Mike', '555-1234')");

    sqnice::query qry(db, "SELECT name, phone FROM contacts");
    auto iter = qry.begin();
    string name, phone;
    (*iter).getter() >> name >> phone;
    expect_eq("Mike", name);
    expect_eq("555-1234", phone);
}

TEST_CASE_METHOD(sqnice_test, "SQNice invalid path", "[.sqnice]") {
    sqnice::database bad_db;
    bad_db.exceptions(false);
    auto rc = bad_db.open("/test/invalid/path");
    CHECK(rc == sqnice::status::cantopen);
    CHECK(bad_db.last_status() == sqnice::status::cantopen);
    CHECK(bad_db.error_msg() != nullptr);
}

TEST_CASE_METHOD(sqnice_test, "SQNice close", "[sqnice]") {
    {
        sqnice::transaction xct(db);
        sqnice::command cmd(db, "INSERT INTO contacts (name, phone) VALUES ('AAAA', '1234')");

        cmd.execute();
    }
    db.close();
}

TEST_CASE_METHOD(sqnice_test, "SQNice backup", "[sqnice]") {
    sqnice::database backupdb;
    backupdb.open_temporary();

    db.backup(backupdb,
              [](int pagecount, int remaining, sqnice::status rc) {
        cout << pagecount << "/" << remaining << endl;
        if (rc == sqnice::status::busy || rc == sqnice::status::locked) {
            // sleep(250);
        }
    });
}

namespace {
    struct handler
    {
        handler() : cnt_(0) {}

        void handle_update(int opcode, char const* dbname, char const* tablename, long long int rowid) {
            cout << "handle_update(" << opcode << ", " << dbname << ", " << tablename << ", " << rowid << ") - " << cnt_++ << endl;
        }
        int cnt_;
    };

    sqnice::status handle_authorize(int evcode, char const* /*p1*/, char const* /*p2*/, char const* /*dbname*/, char const* /*tvname*/) {
        cout << "handle_authorize(" << evcode << ")" << endl;
        return sqnice::status::ok;
    }

    struct rollback_handler
    {
        void operator()() {
            cout << "handle_rollback" << endl;
        }
    };
}

TEST_CASE_METHOD(sqnice_test, "SQNice callbacks", "[sqnice]") {
    {
        db.set_commit_handler([]{cout << "handle_commit\n"; return 0;});
        db.set_rollback_handler(rollback_handler());
    }

    handler h;

    db.set_update_handler(std::bind(&handler::handle_update, &h, _1, _2, _3, _4));

    db.set_authorize_handler(&handle_authorize);

    db.execute("INSERT INTO contacts (name, phone) VALUES ('AAAA', '1234')");

    {
        sqnice::transaction xct(db);

        sqnice::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (?, ?)");

        cmd.bind(1, "BBBB");
        cmd.bind(2, "1234");
        cmd.execute();

        cmd.reset();

        cmd.binder() << "CCCC" << "1234";

        cmd.execute();

        xct.commit();
    }

    {
        sqnice::transaction xct(db);

        sqnice::command cmd(db, "INSERT INTO contacts (name, phone) VALUES (:name, :name)");

        cmd[":name"] = "DDDD";

        cmd.execute();
    }

}

TEST_CASE("SQNice pool", "[sqnice]") {
    static constexpr string_view kDBPath = "sqnice_test.sqlite3";
    sqnice::pool pool(kDBPath, sqnice::open_flags::delete_first | sqnice::open_flags::readwrite);
    {
        auto db = pool.borrow_writeable();
        CHECK(pool.borrowed_count() == 1);
        db->execute(R"""(
            CREATE TABLE contacts (
              id INTEGER PRIMARY KEY,
              name TEXT NOT NULL,
              phone TEXT NOT NULL,
              address TEXT,
              UNIQUE(name, phone)
            );
          )""");
        db->execute("");
        auto cmd = db->command("INSERT INTO contacts (name, phone) VALUES (?1, ?2)");
        cmd.execute("Bob", "555-1212");

        CHECK(pool.try_borrow_writeable() == nullptr);
    }

    CHECK(pool.borrowed_count() == 0);
    CHECK(pool.open_count() == 1);

    {
        auto db1 = pool.borrow();
        CHECK(pool.borrowed_count() == 1);
        CHECK(pool.open_count() == 2);
        string name = db1->query("SELECT name FROM contacts").single_value_or<string>("");
        CHECK(name == "Bob");

        auto db2 = pool.borrow();
        CHECK(pool.borrowed_count() == 2);
        CHECK(pool.open_count() == 3);
        auto db3 = pool.borrow();
        CHECK(pool.borrowed_count() == 3);
        CHECK(pool.open_count() == 4);
        auto db4 = pool.borrow();
        CHECK(pool.borrowed_count() == 4);
        CHECK(pool.open_count() == 5);

        CHECK(pool.try_borrow() == nullptr);
        db1.reset();

        CHECK(pool.borrowed_count() == 3);
        CHECK(pool.open_count() == 5);

        auto db5 = pool.borrow();
        CHECK(pool.borrowed_count() == 4);
        CHECK(pool.open_count() == 5);

        {
            sqnice::transaction txn(pool);
            CHECK(pool.borrowed_count() == 5);
            CHECK(pool.try_borrow_writeable() == nullptr);
        }

        CHECK(pool.borrowed_count() == 4);
        CHECK(pool.open_count() == 5);
    }

    CHECK(pool.borrowed_count() == 0);
    CHECK(pool.open_count() == 5);

    pool.close_all();

    CHECK(pool.borrowed_count() == 0);
    CHECK(pool.open_count() == 0);

    sqnice::database::delete_file(kDBPath);
}


TEST_CASE("SQNice schema migration", "[sqnice]") {
    static constexpr string_view kDBPath = "sqnice_test.sqlite3";
    sqnice::database::delete_file(kDBPath);
    auto open_v1 = [] {
        sqnice::database db(kDBPath);
        db.setup();
        sqnice::transaction txn(db);
        db.migrate_from(0, 1, R"""(
            CREATE TABLE contacts (
              id INTEGER PRIMARY KEY,
              name TEXT NOT NULL,
              phone TEXT NOT NULL,
              address TEXT,
              UNIQUE(name, phone)
            );
          )""");
        txn.commit();
        CHECK(db.user_version() == 1);
    };

    open_v1();

    open_v1();

    auto open_v2 = [] (int64_t expectedVersion) {
        sqnice::database db(kDBPath);
        sqnice::transaction txn(db);
        CHECK(db.user_version() == expectedVersion);

        // Migration for a newly created database, leaving it at version 2:
        db.migrate_from(0, 2, R"""(
            CREATE TABLE contacts (
              id INTEGER PRIMARY KEY,
              name TEXT NOT NULL,
              phone TEXT NOT NULL,
              address TEXT,
              age INTEGER,
              UNIQUE(name, phone)
            );
          )""");

        // Migration to upgrade version 1 (above) to version 2:
        db.migrate_to(2, "ALTER TABLE contacts ADD COLUMN age INTEGER");
        txn.commit();

        CHECK(db.user_version() == 2);

        db.execute("INSERT INTO contacts (name, phone, age) VALUES ('Alice', '555-1919', 39)");
    };

    open_v2(1);

    sqnice::database::delete_file(kDBPath);

    open_v2(0);
}
