# SQNice

SQNice is a _darn nice_ SQLite API for C++.

It is based on [sqlite3pp](https://github.com/iwongu/sqlite3pp) by Wongoo Lee, but has been extensively modified by me, Jens Alfke ([@snej](https://github.com/snej)).

In the nearly 20(!) years I've been using SQLite, I've learned a number of good practices, and how to avoid pitfalls from suble misuse of its APIs. 

With **SQNice** I've tried to take advantage of my learning and bake it into the library's API, to make those good practices automatic or at least easy. I've also tried to make it idiomatic C++, and make simple things simple. Some examples:

* Headers do not include `<sqlite3.h>`, improving compile time and saving you from having to know or use the cryptic C API.
* Strongly typed: errors are returned as `status`, an enum class, not generic `int`s.
* Most strings are passed in as `string_view` so you don't have to call `c_str()` on them.
* Uses C++ exceptions by default, but they can be turned off at the database or query level when necessary if you need to handle expected errors like constraint violations.
* Supports some cool but lesser-known features, like backups and blob streams.

* Lets you set up best practices like WAL and incremental vacuuming with one [optional] setup call.
* Avoids subtle problems that can result from closing a database while statements are still busy. (At one point in the past my co-workers and I had to diagnose a very nasty database corruption bug stemming from this!)
* Manages nested transactions smoothly with RAII, including safely destructing a transaction object when an exception is thrown.
* Easy access to getting and setting pragmas and limits.

* Super easy to reuse compiled statements (`sqlite3_stmt`), without running into problems with leftover bindings or forgetting to reset.
* Convenient API for user-defined SQL functions.
* It's very simple to bind arguments to statement parameters, and to read column values. This is extensible, so you can make your custom C++ types easily bindable too.
* Unlike some other C++ APIs, you can bind or return `unsigned` types without having use manual casts; and it catches problems like binding a `uint64_t` that's too big for SQLite to represent.
* It's very easy to run a query that returns a single value.
* Includes idiomatic APIs for defining custom SQL functions, even aggregates. These use the same convenient binding API as queries.

## Building It

A C++20 compiler is required. So far SQNice has been tested with Clang on macOS and GCC on Linux.

There is a CMake build script, so if your build system is CMake too, you can simply use an `add_subdirectory` call to add sqnice.

Otherwise, just add the source files in `src/` to your project, and add `include/` to your header search path.

Oh, you'll need SQLite, too. It's pre-installed on Apple platforms. Otherwise you can install it with your favorite package manager, or just download it from [sqlite.org](https://sqlite.org/download.html).

## Using It

For most purposes, you just need to include `sqnice/sqnice.hh`. Everything is in the namespace `sqnice`.

The main classes are:

- `database` – a database connection.
- `transaction` – a transaction or savepoint. Uses RAII to ensure the transaction aborts in case of exception or error.
- `query` – a compiled SQL `SELECT` statement. Rows are accessed with its `iterator`, which produces `row`s of `column_value`s.
- `command` – a compiled SQL statement other than `SELECT`; doesn’t return rows.

### Example

```c++
#include "sqnice/sqnice.hh"
#include <cstdio>
using namespace sqnice;

database db("", open_flags::memory);
db.setup();									// [1]
db.execute(R"""(
    CREATE TABLE contacts (
      id INTEGER PRIMARY KEY,
      name TEXT NOT NULL,
      phone TEXT NOT NULL,
      age INTEGER,
      UNIQUE(name, phone) ); )""");
{
    transaction txn(db);
	command ins = db.command("INSERT INTO contacts (name, phone, age) VALUES (?,?,?)");
    ins.execute("Frank", "555-1212", 36); 	// [2]
    ins.execute("Tanya", "555-1313", 50);
    txn.commit();							// [3]
}

query min_age = db.query("SELECT id, name, age FROM contacts WHERE age >= ?1");
for (query::row row : min_age(40)) {		// [4]
    int64_t id = row[0];					// [5]
    const char* name = row[1];
    unsigned age = row[2];
    std::printf("%2lld: '%s', age %u\n", id, name, age);
}											// [6]
```

**Notes:**

1. This is the “best practices setup” feature. Enables WAL, foreign key checks, extended error codes, etc. (If you don’t know what those are, don’t worry.) You’re welcome.
2. This is one way to bind parameters: all at once like a function call. Or you could call `ins.bind(1, ...)` to bind individually, or `ins[1] = ...`.
3. A `transaction` must be explicitly committed, else it aborts when its scope exits.
4. Idiomatic C++ `for` loop using an iterator behind the scenes. And again, we use the functional style to bind parameters first.
5. Accessing column values by index. The actual type is `column_value`, which has implicit conversions so you can assign it to typical C++ types.
6. Behind the scenes, the `sqlite3_stmt` is automatically reset when the iterator exits scope.

### Error Handling

SQNice defaults to throwing C++ exceptions, but recognizes that sometimes you want to handle errors without throwing. So exceptions can be disabled on any instance by calling `exceptions(false)`, causing its methods to return the error as a `status` enum instead.

Objects created from a `database` inherit the exceptions setting of the database instance, so turning off exceptions on the `database` after you create it will pretty much turn them all off.

> NOTE: Even if exceptions are turned off, SQNice will still throw exceptions for programmer errors like illegal arguments, arguments out of range, or calling an object that’s not in a valid state (like `execute()` on a closed `database`. These represent bugs in your code, not recoverable runtime errors.)

Constructors can’t return status codes, of course, so any constructor that can fail will always throw an exception on failure. But you can avoid this by using default constructors instead, then calling a regular method to initialize the object. For example, you can instantiate a `database` without parameters, call `exceptions(false)`, then call `open()` to open a database and get a status code back.

Similarly, destructors can’t return status, but nor should they throw exceptions. So for example `database`'s destructor closes the database if it’s open, but will ignore any errors. If you want to make sure to detect any errors, you can call `close()` first.

### Commands and Queries

The simplest way to execute a SQL command is with `db.execute("...")`. It just takes a SQL string. You can have multiple commands separated by semicolons. This is great for simple things like creating tables or indexes.

The **`command`** class is more sophisticated. It’s better for `INSERT`, `UPDATE`, `DELETE` statements. Usually you create one of these by calling `db.command("...")`. The database keeps a cache of compiled commands, so if you pass the same SQL string again you’ll get a `command` instance that was already compiled, which is faster.

`command` supports bindable parameters (`?`, `?1`, `:name`, `$name`) in the statement. When the object is created the parameter values are all reset to `NULL`, so before executing the statement you should bind values to these. There are three ways to do this:

- `cmd.bind(1, "foo")` or `cmd.bind(":name", "foo")`
- `cmd[1] = "foo"` or `cmd[":name"] = "foo"`
- `cmd("foo", 12, 3.14159)` … this binds all parameters at once, starting at 1.

After calling `execute()` to run the command, you can call its `last_insert_rowid()` method to get the row ID from an `INSERT`, or `changes()` to find how many rows were changed.

**`query`** is for `SELECT` statements. It has bindable parameters like `command`, but you run it by creating an `iterator` and stepping through the result row(s). You can do this with `begin()` and `end()` or with a `for(:)` loop. The iterator produces `row` objects, which act like indexable arrays of `column`s, which can be assigned to your own variables or passed to functions.

### Data Types

Binding parameters or getting column values means converting between C++ types and the SQLite types `INTEGER`, `FLOAT`, `TEXT`, `BLOB` and `NULL`. SQLite can implicitly convert to/from these C++ types:

- All integer types up to 64-bit, including unsigned ones.
- `bool`
- `float` and `double`
- `const char*`, `std::string_view`, `std::string`
- Blobs can be accessed as `const void*`, `std::span<std::byte>`, or the struct `sqnice::blob` which is simply a pointer and a size.
- `nullptr_t` , i.e. a literal `nullptr`, represents `NULL`.

When binding text and blob parameters you can tell SQLite whether to copy the value. Copying is slower but safer; if you don’t copy the value, the `command` or `query` keeps a pointer to it, and it’s your responsibility to make sure that pointer remains valid until the parameter is re-bound or you destruct the `command` or `query` object. In `sqnice` you disable copying by wrapping the parameter value with `uncopied(...)`, for example `q[1] = uncopied(myLongString)`.

Copying is also relevant when getting column values back from SQLite. If you assign a `column` to a `string_view`, for example, the `string_view` points to memory owned by the query and will only remain valid until the iterator is advanced or destructed. If that doesn’t work for you, assign it to a `string` instead.

You can write conversion functions to work with your own data types, too; details TBD, but look at the types `bindable` and `column_helper` in `query.hh` for clues.

### Transactions

Transactions are important not just for their ACID properties, but for performance. If you don’t use a transaction, SQLite implicitly wraps one around every statement it runs, which means each statement incurs the significant overhead of committing changes. If you have multiple statements to run at once, it’s highly recommended to group them in a transaction.

In SQNice, the lowest-level way to use transactions is by calling `db.beginTransaction()` and `db.endTransaction()`. But these are not recommended because they put the burden on you to balance each begin with an end.

Usually you’ll use the `transaction` class. The constructor begins the transaction, and the destructor makes sure to abort it if you didn’t already explicitly commit or abort. That means you can `return` or `break` out of a block containing a `transaction`, or even throw an exception, and everything is cleaned up for you. But you _do_ have to remember to call `txn.commit()` at the end, before it exits scope.

(There is an auto-commit mode for transactions, where it calls `commit` for you in the destructor, but it’s discouraged because a commit might fail but destructors shouldn’t throw exceptions.)

### Thread Safety

> **IMPORTANT:** SQNice's code is **not** thread-safe. You MUST NOT access a `database`, nor any objects created from it such as `command`, `query`, etc., simultaneously from multiple threads.

SQLite itself has some limited thread-safety (depending on how you set its mutex mode), but it is not very useful in practice. Interleaving SQL statements from multiple threads doing different tasks may not crash, but unless all those statements are read-only, you're likely to get race conditions at the database level. (Worse, using a single compiled statement simultaneously can result in a mishmash of bindings and result rows.) Transactions will not help! A transaction prevents other _connections_ from altering the database, not other threads accessing the _same_ connection.

Some ways to use SQNice on multiple threads:

1. Open one `database`, associate your own `mutex` with it, and make sure each thread locks the mutex while accessing the `database` or while using any `command` or `query` or `transaction` objects.
2. Open one `database` on each thread (on the same file), and make sure each thread uses only its database and derived objects. A thread-local variable can be useful for this.
3. Create a pool of `database` instances on the same file. When a thread wants to use a database it checks one out of the pool, and then returns it after it's done.

At some point I'd like to implement #3 as a utility class for SQNice.
