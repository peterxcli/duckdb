// Formatting library for C++ - legacy printf implementation
//
// Copyright (c) 2012 - 2016, Victor Zverovich
// All rights reserved.
//
// For the license information refer to format.h.

#ifndef FMT_PRINTF_H_
#define FMT_PRINTF_H_

#include <algorithm>  // std::max
#include <limits>     // std::numeric_limits

#include "fmt/ostream.h"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

#ifdef min
#undef min
#endif

FMT_BEGIN_NAMESPACE
namespace internal {

// Checks if a value fits in int - used to avoid warnings about comparing
// signed and unsigned integers.
template <bool IsSigned> struct int_checker {
  template <typename T> static bool fits_in_int(T value) {
    unsigned max = max_value<int>();
    return value <= max;
  }
  static bool fits_in_int(bool) { return true; }
};

template <> struct int_checker<true> {
  template <typename T> static bool fits_in_int(T value) {
    return value >= std::numeric_limits<int>::min() &&
           value <= max_value<int>();
  }
  static bool fits_in_int(int) { return true; }
};

class printf_precision_handler {
 public:
  template <typename T, FMT_ENABLE_IF(is_integral<T>::value)>
  int operator()(T value) {
    if (!int_checker<std::numeric_limits<T>::is_signed || std::is_same<T, int128_t>::value>::fits_in_int(value))
      FMT_THROW(duckdb::InvalidInputException("number is too big"));
    return (std::max)(static_cast<int>(value), 0);
  }

  template <typename T, FMT_ENABLE_IF(!is_integral<T>::value)>
  int operator()(T) {
    FMT_THROW(duckdb::InvalidInputException("precision is not integer"));
    return 0;
  }
};

// An argument visitor that returns true iff arg is a zero integer.
class is_zero_int {
 public:
  template <typename T, FMT_ENABLE_IF(is_integral<T>::value)>
  bool operator()(T value) {
    return value == 0;
  }

  template <typename T, FMT_ENABLE_IF(!is_integral<T>::value)>
  bool operator()(T) {
    return false;
  }
};

template <typename T> struct make_unsigned_or_bool : std::make_unsigned<T> {};

template <> struct make_unsigned_or_bool<bool> { using type = bool; };
template <> struct make_unsigned_or_bool<int128_t> { using type = uint128_t; };
template <> struct make_unsigned_or_bool<uint128_t> { using type = uint128_t; };

template <typename T, typename Context> class arg_converter {
 private:
  using char_type = typename Context::char_type;

  basic_format_arg<Context>& arg_;
  char_type type_;

  bool is_unsigned_type(internal::type t) {
    return t == uint_type || t == ulong_long_type || t == uint128_type;
  }

  template <typename To, typename From>
  To Cast(From x) {
    return duckdb::Cast::Operation<From, To>(x);
  }
  template <typename To, typename From>
  typename std::enable_if<std::is_constructible<To, From>::value, To>::type UnsafeCast(From x) {
    return static_cast<To>(x);
  }

  template <typename To, typename From>
  typename std::enable_if<!std::is_constructible<To, From>::value, To>::type UnsafeCast(From x) {
    return static_cast<To>(x.lower);
  }

 public:
  arg_converter(basic_format_arg<Context>& arg, char_type type)
      : arg_(arg), type_(type) {
        if ((type_ == 'd' || type_ == 'i') && is_unsigned_type(arg_.type())) {
          type_ = 'u';
        }
      }

  void operator()(bool value) {
    if (type_ != 's') operator()<bool>(value);
  }

  template <typename U, FMT_ENABLE_IF(is_integral<U>::value)>
  void operator()(U value) {
    bool is_signed = type_ == 'd' || type_ == 'i';
    using target_type = conditional_t<std::is_same<T, void>::value, U, T>;
    if (const_check(sizeof(target_type) <= sizeof(int))) {
      // Extra casts are used to silence warnings.
      if (is_signed) {
        arg_ = internal::make_arg<Context>(UnsafeCast<int32_t>(UnsafeCast<target_type>(value)));
      } else {
        using unsigned_type = typename make_unsigned_or_bool<target_type>::type;
        arg_ = internal::make_arg<Context>(UnsafeCast<uint32_t>(UnsafeCast<unsigned_type>(value)));
      }
    } else {
      if (is_signed) {
        // glibc's printf doesn't sign extend arguments of smaller types:
        //   std::printf("%lld", -42);  // prints "4294967254"
        // but we don't have to do the same because it's a UB.
        if (sizeof(target_type) > sizeof(int64_t)) {
          arg_ = internal::make_arg<Context>(Cast<int128_t>(value));
        } else {
          arg_ = internal::make_arg<Context>(UnsafeCast<int64_t>(value));
        }
      } else {
        arg_ = internal::make_arg<Context>(UnsafeCast<typename make_unsigned_or_bool<U>::type>(value));
      }
    }
  }

  template <typename U, FMT_ENABLE_IF(!is_integral<U>::value)>
  void operator()(U) {}  // No conversion needed for non-integral types.
};

// Converts an integer argument to T for printf, if T is an integral type.
// If T is void, the argument is converted to corresponding signed or unsigned
// type depending on the type specifier: 'd' and 'i' - signed, other -
// unsigned).
template <typename T, typename Context, typename Char>
void convert_arg(basic_format_arg<Context>& arg, Char type) {
  visit_format_arg(arg_converter<T, Context>(arg, type), arg);
}

// Converts an integer argument to char for printf.
template <typename Context> class char_converter {
 private:
  basic_format_arg<Context>& arg_;

 public:
  explicit char_converter(basic_format_arg<Context>& arg) : arg_(arg) {}

  template <typename T, FMT_ENABLE_IF(is_integral<T>::value)>
  void operator()(T value) {
    arg_ = internal::make_arg<Context>(
        static_cast<typename Context::char_type>(static_cast<uint32_t>(value)));
  }

  template <typename T, FMT_ENABLE_IF(!is_integral<T>::value)>
  void operator()(T) {}  // No conversion needed for non-integral types.
};

// Checks if an argument is a valid printf width specifier and sets
// left alignment if it is negative.
template <typename Char> class printf_width_handler {
 private:
  using format_specs = basic_format_specs<Char>;

  format_specs& specs_;

 public:
  explicit printf_width_handler(format_specs& specs) : specs_(specs) {}

  template <typename T, FMT_ENABLE_IF(is_integral<T>::value)>
  unsigned operator()(T value) {
    auto width = static_cast<uint32_or_64_or_128_t<T>>(value);
    if (internal::is_negative(value)) {
      specs_.align = align::left;
      width = -width;
    }
    unsigned int_max = max_value<int>();
    if (width > int_max) FMT_THROW(duckdb::InvalidInputException("number is too big"));
    return static_cast<unsigned>(width);
  }

  template <typename T, FMT_ENABLE_IF(!is_integral<T>::value)>
  unsigned operator()(T) {
    FMT_THROW(duckdb::InvalidInputException("width is not integer"));
    return 0;
  }
};

template <typename Char, typename Context>
void printf(buffer<Char>& buf, basic_string_view<Char> format,
            basic_format_args<Context> args) {
  Context(std::back_inserter(buf), format, args).format();
}

template <typename OutputIt, typename Char, typename Context>
internal::truncating_iterator<OutputIt> printf(
    internal::truncating_iterator<OutputIt> it, basic_string_view<Char> format,
    basic_format_args<Context> args) {
  return Context(it, format, args).format();
}
}  // namespace internal

using internal::printf;  // For printing into memory_buffer.

template <typename Range> class printf_arg_formatter;

template <typename OutputIt, typename Char> class basic_printf_context;

/**
  \rst
  The ``printf`` argument formatter.
  \endrst
 */
template <typename Range>
class printf_arg_formatter : public internal::arg_formatter_base<Range> {
 public:
  using iterator = typename Range::iterator;

 private:
  using char_type = typename Range::value_type;
  using base = internal::arg_formatter_base<Range>;
  using context_type = basic_printf_context<iterator, char_type>;

  context_type& context_;

  void write_null_pointer(char) {
    this->specs()->type = 0;
    this->write("(nil)");
  }

  void write_null_pointer(wchar_t) {
    this->specs()->type = 0;
    this->write(L"(nil)");
  }

 public:
  using format_specs = typename base::format_specs;

  /**
    \rst
    Constructs an argument formatter object.
    *buffer* is a reference to the output buffer and *specs* contains format
    specifier information for standard argument types.
    \endrst
   */
  printf_arg_formatter(iterator iter, format_specs& specs, context_type& ctx)
      : base(Range(iter), &specs, internal::locale_ref()), context_(ctx) {}

  template <typename T, FMT_ENABLE_IF(duckdb_fmt::internal::is_integral<T>::value)>
  iterator operator()(T value) {
    // MSVC2013 fails to compile separate overloads for bool and char_type so
    // use std::is_same instead.
    if (std::is_same<T, bool>::value) {
      format_specs& fmt_specs = *this->specs();
      if (fmt_specs.type != 's') return base::operator()(value ? 1 : 0);
      fmt_specs.type = 0;
      this->write(value != 0);
    } else if (std::is_same<T, char_type>::value) {
      format_specs& fmt_specs = *this->specs();
      if (fmt_specs.type && fmt_specs.type != 'c')
        return (*this)(static_cast<int>(value));
      fmt_specs.sign = sign::none;
      fmt_specs.alt = false;
      fmt_specs.align = align::right;
      return base::operator()(value);
    } else {
      return base::operator()(value);
    }
    return this->out();
  }

  template <typename T, FMT_ENABLE_IF(std::is_floating_point<T>::value)>
  iterator operator()(T value) {
    return base::operator()(value);
  }

  /** Formats a null-terminated C string. */
  iterator operator()(const char* value) {
    if (value)
      base::operator()(value);
    else if (this->specs()->type == 'p')
      write_null_pointer(char_type());
    else
      this->write("(null)");
    return this->out();
  }

  /** Formats a null-terminated wide C string. */
  iterator operator()(const wchar_t* value) {
    if (value)
      base::operator()(value);
    else if (this->specs()->type == 'p')
      write_null_pointer(char_type());
    else
      this->write(L"(null)");
    return this->out();
  }

  iterator operator()(basic_string_view<char_type> value) {
    return base::operator()(value);
  }

  iterator operator()(monostate value) { return base::operator()(value); }

  /** Formats a pointer. */
  iterator operator()(const void* value) {
    if (value) return base::operator()(value);
    this->specs()->type = 0;
    write_null_pointer(char_type());
    return this->out();
  }

  /** Formats an argument of a custom (user-defined) type. */
  iterator operator()(typename basic_format_arg<context_type>::handle handle) {
    handle.format(context_.parse_context(), context_);
    return this->out();
  }
};

template <typename T> struct printf_formatter {
  template <typename ParseContext>
  auto parse(ParseContext& ctx) -> decltype(ctx.begin()) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const T& value, FormatContext& ctx) -> decltype(ctx.out()) {
    internal::format_value(internal::get_container(ctx.out()), value);
    return ctx.out();
  }
};

/** This template formats data and writes the output to a writer. */
template <typename OutputIt, typename Char> class basic_printf_context {
 public:
  /** The character type for the output. */
  using char_type = Char;
  using format_arg = basic_format_arg<basic_printf_context>;
  template <typename T> using formatter_type = printf_formatter<T>;

 private:
  using format_specs = basic_format_specs<char_type>;

  OutputIt out_;
  basic_format_args<basic_printf_context> args_;
  basic_format_parse_context<Char> parse_ctx_;

  static void parse_flags(format_specs& specs, const Char*& it,
                          const Char* end);

  // Returns the argument with specified index or, if arg_index is -1, the next
  // argument.
  format_arg get_arg(int arg_index = -1);

  // Parses argument index, flags and width and returns the argument index.
  int parse_header(const Char*& it, const Char* end, format_specs& specs);

 public:
  /**
   \rst
   Constructs a ``printf_context`` object. References to the arguments and
   the writer are stored in the context object so make sure they have
   appropriate lifetimes.
   \endrst
   */
  basic_printf_context(OutputIt out, basic_string_view<char_type> format_str,
                       basic_format_args<basic_printf_context> args)
      : out_(out), args_(args), parse_ctx_(format_str) {}

  OutputIt out() { return out_; }
  void advance_to(OutputIt it) { out_ = it; }

  format_arg arg(int id) const { return args_.get(id); }

  basic_format_parse_context<Char>& parse_context() { return parse_ctx_; }

  FMT_CONSTEXPR void on_error(std::string message) {
    parse_ctx_.on_error(message);
  }

  /** Formats stored arguments and writes the output to the range. */
  template <typename ArgFormatter = printf_arg_formatter<buffer_range<Char>>>
  OutputIt format();
};

template <typename OutputIt, typename Char>
void basic_printf_context<OutputIt, Char>::parse_flags(format_specs& specs,
                                                       const Char*& it,
                                                       const Char* end) {
  for (; it != end; ++it) {
    switch (*it) {
    case '-':
      specs.align = align::left;
      break;
    case '+':
      specs.sign = sign::plus;
      break;
    case '0':
      specs.fill[0] = '0';
      break;
    case ' ':
      specs.sign = sign::space;
      break;
    case '#':
      specs.alt = true;
      break;
    case ',':
      specs.thousands = ',';
      break;
    case '\'':
      specs.thousands = '\'';
      break;
    case '_':
      specs.thousands = '_';
      break;
    default:
      return;
    }
  }
}

template <typename OutputIt, typename Char>
typename basic_printf_context<OutputIt, Char>::format_arg
basic_printf_context<OutputIt, Char>::get_arg(int arg_index) {
  if (arg_index < 0)
    arg_index = parse_ctx_.next_arg_id();
  else
    parse_ctx_.check_arg_id(--arg_index);
  return internal::get_arg(*this, arg_index);
}

template <typename OutputIt, typename Char>
int basic_printf_context<OutputIt, Char>::parse_header(
    const Char*& it, const Char* end, format_specs& specs) {
  int arg_index = -1;
  char_type c = *it;
  if (c >= '0' && c <= '9') {
    // Parse an argument index (if followed by '$') or a width possibly
    // preceded with '0' flag(s).
    internal::error_handler eh;
    int value = parse_nonnegative_int(it, end, eh);
    if (it != end && *it == '$') {  // value is an argument index
      ++it;
      arg_index = value;
    } else {
      if (c == '0') specs.fill[0] = '0';
      if (value != 0) {
        // Nonzero value means that we parsed width and don't need to
        // parse it or flags again, so return now.
        specs.width = value;
        return arg_index;
      }
    }
  }
  parse_flags(specs, it, end);
  // Parse width.
  if (it != end) {
    if (*it >= '0' && *it <= '9') {
      internal::error_handler eh;
      specs.width = parse_nonnegative_int(it, end, eh);
    } else if (*it == '*') {
      ++it;
      specs.width = static_cast<int>(visit_format_arg(
          internal::printf_width_handler<char_type>(specs), get_arg()));
    }
  }
  return arg_index;
}

template <typename OutputIt, typename Char>
template <typename ArgFormatter>
OutputIt basic_printf_context<OutputIt, Char>::format() {
  auto out = this->out();
  const Char* start = parse_ctx_.begin();
  const Char* end = parse_ctx_.end();
  auto it = start;
  while (it != end) {
    char_type c = *it++;
    if (c != '%') continue;
    if (it != end && *it == c) {
      out = std::copy(start, it, out);
      start = ++it;
      continue;
    }
    out = std::copy(start, it - 1, out);

    format_specs specs;
    specs.align = align::right;

    // Parse argument index, flags and width.
    int arg_index = parse_header(it, end, specs);
    if (arg_index == 0) on_error("argument index out of range");

    // Parse precision.
	bool empty_precision = false;
    if (it != end && *it == '.') {
      ++it;
      c = it != end ? *it : 0;
      if ('0' <= c && c <= '9') {
        internal::error_handler eh;
        specs.precision = parse_nonnegative_int(it, end, eh);
      } else if (c == '*') {
        ++it;
        specs.precision =
            static_cast<int>(visit_format_arg(internal::printf_precision_handler(), get_arg()));
      } else {
        specs.precision = 0;
		empty_precision = true;
      }
    }

    format_arg arg = get_arg(arg_index);
    if (specs.alt && visit_format_arg(internal::is_zero_int(), arg))
      specs.alt = false;
    if (specs.fill[0] == '0') {
      if (arg.is_arithmetic())
        specs.align = align::numeric;
      else
        specs.fill[0] = ' ';  // Ignore '0' flag for non-numeric types.
    }

    // Parse length and convert the argument to the required type.
    c = it != end ? *it++ : 0;
    char_type t = it != end ? *it : 0;
    using internal::convert_arg;
    switch (c) {
    case 'h':
      if (t == 'h') {
        ++it;
        t = it != end ? *it : 0;
        convert_arg<signed char>(arg, t);
      } else {
        convert_arg<short>(arg, t);
      }
      break;
    case 'l':
      if (t == 'l') {
        ++it;
        t = it != end ? *it : 0;
        convert_arg<long long>(arg, t);
      } else {
        convert_arg<long>(arg, t);
      }
      break;
    case 'j':
      convert_arg<intmax_t>(arg, t);
      break;
    case 'z':
      convert_arg<std::size_t>(arg, t);
      break;
    case 't':
      convert_arg<std::ptrdiff_t>(arg, t);
      break;
    case 'L':
      // printf produces garbage when 'L' is omitted for long double, no
      // need to do the same.
      break;
    default:
      --it;
      convert_arg<void>(arg, c);
    }

    // Parse type.
    if (it == end) FMT_THROW(duckdb::InvalidInputException("invalid format string"));
    specs.type = static_cast<char>(*it++);
    if (arg.is_integral()) {
      // Normalize type.
      switch (specs.type) {
      case 'i':
      case 'u':
        specs.type = 'd';
        break;
      case 'c':
        visit_format_arg(internal::char_converter<basic_printf_context>(arg),
                         arg);
        break;
      }
    }
	if (specs.type == 'd' && empty_precision) {
		specs.thousands = '.';
	}

    start = it;

    // Format argument.
    visit_format_arg(ArgFormatter(out, specs, *this), arg);
  }
  return std::copy(start, it, out);
}

template <typename Char>
using basic_printf_context_t =
    basic_printf_context<std::back_insert_iterator<internal::buffer<Char>>,
                         Char>;

using printf_context = basic_printf_context_t<char>;
using wprintf_context = basic_printf_context_t<wchar_t>;

using printf_args = basic_format_args<printf_context>;
using wprintf_args = basic_format_args<wprintf_context>;

/**
  \rst
  Constructs an `~fmt::format_arg_store` object that contains references to
  arguments and can be implicitly converted to `~fmt::printf_args`.
  \endrst
 */
template <typename... Args>
inline format_arg_store<printf_context, Args...> make_printf_args(
    const Args&... args) {
  return {args...};
}

/**
  \rst
  Constructs an `~fmt::format_arg_store` object that contains references to
  arguments and can be implicitly converted to `~fmt::wprintf_args`.
  \endrst
 */
template <typename... Args>
inline format_arg_store<wprintf_context, Args...> make_wprintf_args(
    const Args&... args) {
  return {args...};
}

template <typename S, typename Char = char_t<S>>
inline std::basic_string<Char> vsprintf(
    const S& format, basic_format_args<basic_printf_context_t<Char>> args) {
  basic_memory_buffer<Char> buffer;
  printf(buffer, to_string_view(format), args);
  return to_string(buffer);
}

/**
  \rst
  Formats arguments and returns the result as a string.

  **Example**::

    std::string message = fmt::sprintf("The answer is %d", 42);
  \endrst
*/
template <typename S, typename... Args,
          typename Char = enable_if_t<internal::is_string<S>::value, char_t<S>>>
inline std::basic_string<Char> sprintf(const S& format, const Args&... args) {
  using context = basic_printf_context_t<Char>;
  return vsprintf(to_string_view(format), {make_format_args<context>(args...)});
}

template <typename S, typename Char = char_t<S>>
inline int vfprintf(std::FILE* f, const S& format,
                    basic_format_args<basic_printf_context_t<Char>> args) {
  basic_memory_buffer<Char> buffer;
  printf(buffer, to_string_view(format), args);
  std::size_t size = buffer.size();
  return std::fwrite(buffer.data(), sizeof(Char), size, f) < size
             ? -1
             : static_cast<int>(size);
}

/**
  \rst
  Prints formatted data to the file *f*.

  **Example**::

    fmt::fprintf(stderr, "Don't %s!", "panic");
  \endrst
 */
template <typename S, typename... Args,
          typename Char = enable_if_t<internal::is_string<S>::value, char_t<S>>>
inline int fprintf(std::FILE* f, const S& format, const Args&... args) {
  using context = basic_printf_context_t<Char>;
  return vfprintf(f, to_string_view(format),
                  {make_format_args<context>(args...)});
}

template <typename S, typename Char = char_t<S>>
inline int vprintf(const S& format,
                   basic_format_args<basic_printf_context_t<Char>> args) {
  return vfprintf(stdout, to_string_view(format), args);
}

/**
  \rst
  Prints formatted data to ``stdout``.

  **Example**::

    fmt::printf("Elapsed time: %.2f seconds", 1.23);
  \endrst
 */
template <typename S, typename... Args,
          FMT_ENABLE_IF(internal::is_string<S>::value)>
inline int printf(const S& format_str, const Args&... args) {
  using context = basic_printf_context_t<char_t<S>>;
  return vprintf(to_string_view(format_str),
                 {make_format_args<context>(args...)});
}

template <typename S, typename Char = char_t<S>>
inline int vfprintf(std::basic_ostream<Char>& os, const S& format,
                    basic_format_args<basic_printf_context_t<Char>> args) {
  basic_memory_buffer<Char> buffer;
  printf(buffer, to_string_view(format), args);
  internal::write(os, buffer);
  return static_cast<int>(buffer.size());
}

/** Formats arguments and writes the output to the range. */
template <typename ArgFormatter, typename Char,
          typename Context =
              basic_printf_context<typename ArgFormatter::iterator, Char>>
typename ArgFormatter::iterator vprintf(internal::buffer<Char>& out,
                                        basic_string_view<Char> format_str,
                                        basic_format_args<Context> args) {
  typename ArgFormatter::iterator iter(out);
  Context(iter, format_str, args).template format<ArgFormatter>();
  return iter;
}

/**
  \rst
  Prints formatted data to the stream *os*.

  **Example**::

    fmt::fprintf(cerr, "Don't %s!", "panic");
  \endrst
 */
template <typename S, typename... Args, typename Char = char_t<S>>
inline int fprintf(std::basic_ostream<Char>& os, const S& format_str,
                   const Args&... args) {
  using context = basic_printf_context_t<Char>;
  return vfprintf(os, to_string_view(format_str),
                  {make_format_args<context>(args...)});
}
FMT_END_NAMESPACE

#endif  // FMT_PRINTF_H_
