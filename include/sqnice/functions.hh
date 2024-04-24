// sqnice/functions.hh
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

#pragma once
#ifndef SQNICEEXT_H
#define SQNICEEXT_H

#include "sqnice/database.hh"
#include "sqnice/query.hh"
//#include <sqlite3.h>    //TODO: Remove this dependency
#include <cstddef>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

ASSUME_NONNULL_BEGIN

struct sqlite3_context;
struct sqlite3_value;

namespace sqnice {
    class arg_value;
    class context;
    class context_result;

    namespace {
        template<size_t N>
        struct Apply {
            template<typename F, typename T, typename... A>
            static inline auto apply(F&& f, T&& t, A&&... a)
            -> decltype(Apply<N-1>::apply(std::forward<F>(f),
                                          std::forward<T>(t),
                                          std::get<N-1>(std::forward<T>(t)),
                                          std::forward<A>(a)...)) {
                return Apply<N-1>::apply(std::forward<F>(f),
                                         std::forward<T>(t),
                                         std::get<N-1>(std::forward<T>(t)),
                                         std::forward<A>(a)...);
            }
        };

        template<>
        struct Apply<0> {
            template<typename F, typename T, typename... A>
            static inline auto apply(F&& f, T&&, A&&... a)
            -> decltype(std::forward<F>(f)(std::forward<A>(a)...)) {
                return std::forward<F>(f)(std::forward<A>(a)...);
            }
        };

        template<typename F, typename T>
        inline auto apply_f(F&& f, T&& t)
        -> decltype(Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(std::forward<F>(f), std::forward<T>(t))) {
            return Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(
                                                                                      std::forward<F>(f), std::forward<T>(t));
        }
    }


    /** The concept `resultable` identifies custom types that can be assigned as a function result.
         Declare a function `sqlite3cpp::result_helper(context&, T)`.
         This function should call context::result(U) for some built-in type U. */
    template <typename T>
    concept resultable = requires(context& ctx, T value) {
        {result_helper(ctx, value)} -> std::same_as<status>;
    };


    /** Represents the result of a function. The type of `context::result`. */
    class context_result : noncopyable {
    public:
        void operator= (std::signed_integral auto v) {
            static_assert(sizeof(v) <= 8);
            if constexpr (sizeof(v) <= 4)
                set_int(v);
            else
                set_int64(v);
        }

        void operator= (std::unsigned_integral auto v) {
            static_assert(sizeof(v) <= 8);
            if constexpr (sizeof(v) < 4)
                set_int(int(v));
            else if constexpr (sizeof(v) < 8)
                set_int64(int64_t(v));
            else
                set_uint64(v);
        }

        void operator= (std::floating_point auto v)     {set_double(v);}
        void operator= (nullptr_t);
        void operator= (null_type)                      {*this = nullptr;}
        void operator= (arg_value const&);

        void operator= (char const* _Nullable value)    {set(value);}
        void operator= (std::string_view value)         {set(value);}
        void operator= (blob const&);
        void operator= (std::span<const std::byte> value) {set(value);}

        void set(char const* _Nullable value, copy_semantic = copy);
        void set(std::string_view value, copy_semantic = copy);
        void set(std::span<const std::byte> value, copy_semantic = copy);

        template <resultable T>
        void operator= (T&& v) {
            set_helper(*this, std::forward<T>(v));
        }

        using pointer_destructor = void(*)(void*);
        void set_pointer(void* pointer, const char* type, pointer_destructor);

        void set_error(status, std::string_view msg);
        void set_error(std::string_view msg);

    private:
        friend class context;
        explicit context_result(sqlite3_context* ctx)   :ctx_(ctx) { }
        void set_int(int value);
        void set_int64(int64_t value);
        void set_uint64(uint64_t value);
        void set_double(double value);

        sqlite3_context* ctx_;
    };


    /** The context of a SQLite function call. Holds the arguments and result. */
    class context : noncopyable {
    public:
        using argv_t = sqlite3_value* _Nullable * _Nullable;
        explicit context(sqlite3_context* ctx, int nargs = 0, argv_t values = nullptr);

        class context_args {
        public:
            arg_value operator[] (unsigned arg) const;
            size_t size() const                         {return argc_;}
        private:
            friend class context;
            context_args(int argc, argv_t argv)         :argc_(argc), argv_(argv) { }
            int     argc_;
            argv_t  argv_;
        };

        size_t const    argc;       ///< The number of arguments
        context_args    argv;       ///< The "array" of arguments
        context_result  result;     ///< Assign the result to this

        template <class T> T get(int idx) const;

        void result_error(std::string_view msg)         {result.set_error(msg);}

    private:
        friend class functions;

        void* _Nullable aggregate_data(int size);

        template <class T>
        T* _Nonnull aggregate_state() {
            auto data = static_cast<uint8_t*>(aggregate_data(sizeof(T) + 1));
            if (!data[sizeof(T)]) { // last byte tracks whether T has been constructed
                new (data) T;
                data[sizeof(T)] = true;
            }
            return reinterpret_cast<T*>(data);
        }

        template <class... Ts>
        std::tuple<Ts...> to_tuple() {
            return to_tuple_impl(0, *this, std::tuple<Ts...>());
        }

        void* user_data();

        template<class H, class... Ts>
        static inline std::tuple<H, Ts...> to_tuple_impl(int index, const context& c, std::tuple<H, Ts...>&&) {
            auto h = std::make_tuple(c.context::get<H>(index));
            return std::tuple_cat(h, to_tuple_impl(++index, c, std::tuple<Ts...>()));
        }
        static inline std::tuple<> to_tuple_impl(int /*index*/, const context& /*c*/, std::tuple<>&&) {
            return std::tuple<>();
        }

    private:
        sqlite3_context* ctx_;
    };


    /** Represents a single function argument; returned by `context[]`. */
    class arg_value : sqnice::noncopyable {
    public:
        explicit arg_value(sqlite3_value* v) noexcept   :value_(v) { }

        /// Gets the value as type `T`.
        template <typename T> T get() const noexcept;

        /// Implicit conversion to type `T`, for assignment or passing as a parameter.
        template <typename T> operator T() const noexcept  {return get<T>();}

        /// The data type of the column value.
        data_type type() const noexcept;
        bool not_null() const noexcept                  {return type() != data_type::null;}
        bool is_blob() const noexcept                   {return type() == data_type::blob;}

        /// The length in bytes of a text or blob value.
        size_t size_bytes() const noexcept;

        // The following are just the specializations of get<T>() ...

        template <std::signed_integral T>
        T get() const noexcept {
            if constexpr (sizeof(T) <= sizeof(int))
                return static_cast<T>(get_int());
            else
                return get_int64();
        }

        template <std::unsigned_integral T>
        T get() const noexcept {
            // pin negative values to 0 instead of returning bogus huge numbers
            if constexpr (sizeof(T) < sizeof(int))
                return static_cast<T>(std::max(0, get_int()));
            else
                return static_cast<T>(std::max(int64_t(0), get_int64()));
        }

        template<std::floating_point T>
        T get() const noexcept                          {return static_cast<T>(get_double());}

        template<> bool get() const noexcept            {return get_int() != 0;}
        template<> char const* get() const noexcept;
        template<> std::string get() const noexcept   {return std::string(get<std::string_view>());}
        template<> std::string_view get() const noexcept;
        template<> void const* get() const noexcept;
        template<> blob get() const noexcept;
        template<> null_type get() const noexcept       {return ignore;}

        template <columnable T> T get() const noexcept  {return column_helper<T>::get(*this);}

        sqlite3_value* value() const noexcept           {return value_;}

    private:
        arg_value(arg_value&&) = delete;
        arg_value& operator=(arg_value&&) = delete;

        int get_int() const noexcept;
        int64_t get_int64() const noexcept;
        double get_double() const noexcept;

        sqlite3_value* value_;
    };


    template <class T> T context::get(int idx) const    {return argv[idx];}


    /** Manages user-defined functions for a database. */
    class functions : public checking, noncopyable {
    public:
        using function_handler = std::function<void (context&)>;
        using pfunction_base = std::shared_ptr<void>;

        explicit functions(database& db) : checking(db) { }

        status create(char const* name, function_handler h, int nargs);

        template <class F>
        status create(char const* name, std::function<F> h) {
            auto db = check_get_db();
            auto fh = new std::function<F>(std::move(h));
            auto destroy = [](void* pApp) {delete static_cast<std::function<F>*>(pApp);};
            return create_function_impl<F>()(this, name, fh, destroy);
        }

        status create_aggregate(char const* name, function_handler s, function_handler f, int nargs);

        template <class T, class... Ps>
        status create_aggregate(char const* name) {
            auto db = check_get_db();
            return register_function(db, name, sizeof...(Ps), 0, nullptr,
                                     stepx_impl<T, Ps...>, finishN_impl<T>, destroy_impl<T>);
        }

    private:

        template<class R, class... Ps>
        struct create_function_impl;

        template<class R, class... Ps>
        struct create_function_impl<R (Ps...)> {
            status operator()(functions* fns, char const* name, void* fh,
                              void (*destroy)(void*)) {
                return fns->register_function(fns->check_get_db(), name, sizeof...(Ps), fh, functionx_impl<R, Ps...>, nullptr, nullptr, destroy);
            }
        };

        template <class R, class... Ps>
        static void functionx_impl(sqlite3_context* ctx, int nargs, context::argv_t values) {
            context c(ctx, nargs, values);
            auto f = static_cast<std::function<R (Ps...)>*>(c.user_data());
            c.result = apply_f(*f, c.to_tuple<Ps...>());
        }

        template <class T, class... Ps>
        static void stepx_impl(sqlite3_context* ctx, int nargs, context::argv_t values) {
            context c(ctx, nargs, values);
            T* t = c.aggregate_state<T>();
            apply_f([](T* tt, Ps... ps){tt->step(ps...);},
                    std::tuple_cat(std::make_tuple(t), c.to_tuple<Ps...>()));
        }

        template <class T>
        static void finishN_impl(sqlite3_context* ctx) {
            context c(ctx);
            T* t = c.aggregate_state<T>();
            c.result = t->finish();
            t->~T();
        }

        template <class T>
        static void destroy_impl(void* pApp) {
        }

        using callFn = void (*)(sqlite3_context*, int, context::argv_t);
        using finishFn = void (*)(sqlite3_context*);
        using destroyFn = void (*)(void*);
        status register_function(std::shared_ptr<sqlite3> const&,
                                 const char *name, int nArg, void* _Nullable pApp,
                                 callFn _Nullable call,
                                 callFn _Nullable step = nullptr,
                                 finishFn _Nullable finish = nullptr,
                                 destroyFn destroy = nullptr);
    };

} // namespace sqnice

ASSUME_NONNULL_END

#endif
