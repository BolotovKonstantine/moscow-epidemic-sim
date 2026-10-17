#include "json.h"

namespace mesim {
namespace json {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
	Parser(const char *data, size_t size) :
			p_(reinterpret_cast<const unsigned char *>(data)), end_(p_ + size), begin_(p_) {}

	bool document(Value &out) {
		skip_space();
		if (!value(out, 0)) {
			return false;
		}
		skip_space();
		if (p_ != end_) {
			return fail("лишние данные после значения");
		}
		return true;
	}

	std::string error;

private:
	const unsigned char *p_;
	const unsigned char *end_;
	const unsigned char *begin_;

	bool fail(const char *what) {
		error = std::string(what) + " (позиция " + std::to_string(p_ - begin_) + ")";
		return false;
	}

	void skip_space() {
		while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) {
			++p_;
		}
	}

	bool literal(const char *text) {
		const unsigned char *q = p_;
		for (; *text != '\0'; ++text, ++q) {
			if (q == end_ || *q != static_cast<unsigned char>(*text)) {
				return false;
			}
		}
		p_ = q;
		return true;
	}

	bool value(Value &out, int depth) {
		if (depth > kMaxDepth) {
			return fail("слишком глубокая вложенность");
		}
		if (p_ == end_) {
			return fail("неожиданный конец данных");
		}
		switch (*p_) {
			case '{':
				return object(out, depth);
			case '[':
				return array(out, depth);
			case '"':
				out.kind = Kind::string;
				return string(out.string);
			case 't':
				out.kind = Kind::boolean;
				out.boolean = true;
				return literal("true") || fail("неверный литерал");
			case 'f':
				out.kind = Kind::boolean;
				out.boolean = false;
				return literal("false") || fail("неверный литерал");
			case 'n':
				out.kind = Kind::null;
				return literal("null") || fail("неверный литерал");
			default:
				return number(out);
		}
	}

	bool object(Value &out, int depth) {
		out.kind = Kind::object;
		++p_;
		skip_space();
		if (p_ != end_ && *p_ == '}') {
			++p_;
			return true;
		}
		while (true) {
			skip_space();
			if (p_ == end_ || *p_ != '"') {
				return fail("ожидается ключ объекта");
			}
			std::string key;
			if (!string(key)) {
				return false;
			}
			for (const auto &member : out.members) {
				if (member.first == key) {
					return fail(("повторяющийся ключ \"" + key + "\"").c_str());
				}
			}
			skip_space();
			if (p_ == end_ || *p_ != ':') {
				return fail("ожидается ':'");
			}
			++p_;
			skip_space();
			out.members.emplace_back(std::move(key), Value{});
			if (!value(out.members.back().second, depth + 1)) {
				return false;
			}
			skip_space();
			if (p_ != end_ && *p_ == ',') {
				++p_;
				continue;
			}
			if (p_ != end_ && *p_ == '}') {
				++p_;
				return true;
			}
			return fail("ожидается ',' или '}'");
		}
	}

	bool array(Value &out, int depth) {
		out.kind = Kind::array;
		++p_;
		skip_space();
		if (p_ != end_ && *p_ == ']') {
			++p_;
			return true;
		}
		while (true) {
			skip_space();
			out.items.emplace_back();
			if (!value(out.items.back(), depth + 1)) {
				return false;
			}
			skip_space();
			if (p_ != end_ && *p_ == ',') {
				++p_;
				continue;
			}
			if (p_ != end_ && *p_ == ']') {
				++p_;
				return true;
			}
			return fail("ожидается ',' или ']'");
		}
	}

	static void append_utf8(std::string &s, uint32_t cp) {
		if (cp < 0x80) {
			s += static_cast<char>(cp);
		} else if (cp < 0x800) {
			s += static_cast<char>(0xC0 | (cp >> 6));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			s += static_cast<char>(0xE0 | (cp >> 12));
			s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			s += static_cast<char>(0xF0 | (cp >> 18));
			s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	bool hex4(uint32_t &out) {
		if (end_ - p_ < 4) {
			return fail("неполная escape-последовательность \\u");
		}
		out = 0;
		for (int i = 0; i < 4; ++i, ++p_) {
			const unsigned char c = *p_;
			uint32_t digit;
			if (c >= '0' && c <= '9') {
				digit = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				digit = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				digit = c - 'A' + 10;
			} else {
				return fail("неверная escape-последовательность \\u");
			}
			out = out * 16 + digit;
		}
		return true;
	}

	// Проверяет одну последовательность UTF-8 и копирует её в s.
	bool utf8_sequence(std::string &s) {
		const unsigned char c = *p_;
		int length;
		uint32_t cp;
		if (c < 0x80) {
			length = 1;
			cp = c;
		} else if ((c & 0xE0) == 0xC0) {
			length = 2;
			cp = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			length = 3;
			cp = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			length = 4;
			cp = c & 0x07;
		} else {
			return fail("неверный UTF-8");
		}
		if (end_ - p_ < length) {
			return fail("неверный UTF-8");
		}
		for (int i = 1; i < length; ++i) {
			const unsigned char next = p_[i];
			if ((next & 0xC0) != 0x80) {
				return fail("неверный UTF-8");
			}
			cp = (cp << 6) | (next & 0x3F);
		}
		static const uint32_t kMin[5] = { 0, 0, 0x80, 0x800, 0x10000 };
		if (cp < kMin[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
			return fail("неверный UTF-8");
		}
		s.append(reinterpret_cast<const char *>(p_), static_cast<size_t>(length));
		p_ += length;
		return true;
	}

	bool string(std::string &out) {
		++p_;  // открывающая кавычка
		while (true) {
			if (p_ == end_) {
				return fail("незакрытая строка");
			}
			const unsigned char c = *p_;
			if (c == '"') {
				++p_;
				return true;
			}
			if (c < 0x20) {
				return fail("управляющий символ в строке");
			}
			if (c != '\\') {
				if (!utf8_sequence(out)) {
					return false;
				}
				continue;
			}
			++p_;
			if (p_ == end_) {
				return fail("незакрытая строка");
			}
			const unsigned char e = *p_++;
			switch (e) {
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u': {
					uint32_t cp;
					if (!hex4(cp)) {
						return false;
					}
					if (cp >= 0xD800 && cp <= 0xDBFF) {
						uint32_t low;
						if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') {
							return fail("непарный суррогат");
						}
						p_ += 2;
						if (!hex4(low)) {
							return false;
						}
						if (low < 0xDC00 || low > 0xDFFF) {
							return fail("непарный суррогат");
						}
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
						return fail("непарный суррогат");
					}
					append_utf8(out, cp);
					break;
				}
				default:
					return fail("неверная escape-последовательность");
			}
		}
	}

	bool number(Value &out) {
		const unsigned char *start = p_;
		bool negative = false;
		if (*p_ == '-') {
			negative = true;
			++p_;
		}
		if (p_ == end_ || *p_ < '0' || *p_ > '9') {
			return fail("неверное значение");
		}
		if (*p_ == '0' && end_ - p_ > 1 && p_[1] >= '0' && p_[1] <= '9') {
			return fail("ведущий ноль в числе");
		}
		// Мантисса — первые 19 значащих цифр; остальные учитываются в порядке.
		uint64_t mantissa = 0;
		int digits = 0;
		int exponent = 0;
		bool overflow = false;
		uint64_t integer = 0;
		while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
			const unsigned digit = *p_ - '0';
			if (integer > (UINT64_MAX - digit) / 10) {
				overflow = true;
			} else {
				integer = integer * 10 + digit;
			}
			if (digits < 19) {
				mantissa = mantissa * 10 + digit;
				if (mantissa != 0) {
					++digits;
				}
			} else {
				++exponent;
			}
			++p_;
		}
		bool real = false;
		if (p_ != end_ && *p_ == '.') {
			real = true;
			++p_;
			if (p_ == end_ || *p_ < '0' || *p_ > '9') {
				return fail("неверная дробная часть");
			}
			while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
				if (digits < 19) {
					mantissa = mantissa * 10 + (*p_ - '0');
					if (mantissa != 0) {
						++digits;
					}
					--exponent;
				}
				++p_;
			}
		}
		if (p_ != end_ && (*p_ == 'e' || *p_ == 'E')) {
			real = true;
			++p_;
			bool exponent_negative = false;
			if (p_ != end_ && (*p_ == '+' || *p_ == '-')) {
				exponent_negative = *p_ == '-';
				++p_;
			}
			if (p_ == end_ || *p_ < '0' || *p_ > '9') {
				return fail("неверный порядок числа");
			}
			int e = 0;
			while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
				if (e < 100000) {
					e = e * 10 + (*p_ - '0');
				}
				++p_;
			}
			exponent += exponent_negative ? -e : e;
		}
		if (!real) {
			if (overflow) {
				p_ = start;
				return fail("целое больше 2^64 - 1");
			}
			out.kind = Kind::integer;
			out.negative = negative;
			out.magnitude = integer;
			out.real = negative ? -static_cast<double>(integer) : static_cast<double>(integer);
			return true;
		}
		// Мантисса до 2^53 и порядок до 22 дают точное, правильно округлённое значение
		// (в индексе это начало плоскости, например 376000.0); иначе — приближённо.
		static const double kPow10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
			1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
		double value = static_cast<double>(mantissa);
		if (mantissa != 0) {
			int e = exponent;
			while (e > 22) {
				value *= 1e22;
				e -= 22;
				if (value > 1.7976931348623157e308) {
					p_ = start;
					return fail("число вне диапазона double");
				}
			}
			while (e < -22) {
				value /= 1e22;
				e += 22;
			}
			value = e >= 0 ? value * kPow10[e] : value / kPow10[-e];
			if (value > 1.7976931348623157e308) {
				p_ = start;
				return fail("число вне диапазона double");
			}
		}
		out.kind = Kind::real;
		out.real = negative ? -value : value;
		return true;
	}
};

} // namespace

const Value *Value::get(const std::string &key) const {
	for (const auto &member : members) {
		if (member.first == key) {
			return &member.second;
		}
	}
	return nullptr;
}

bool parse(const char *data, size_t size, Value &out, std::string &error) {
	out = Value{};
	Parser parser(data, size);
	if (!parser.document(out)) {
		error = parser.error;
		out = Value{};
		return false;
	}
	return true;
}

} // namespace json
} // namespace mesim
