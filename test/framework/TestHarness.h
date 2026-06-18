#pragma once

// A tiny, dependency-free test harness for the offline `osf-tests` target. No
// external framework (keeps the build xmake-only and fully offline, matching the
// existing osf-import-test harness style). Self-registering cases, value-printing
// checks, and exception-as-failure.
//
//   OSF_TEST_CASE(MyThing_does_x) { CHECK(cond); CHECK_EQ(a, b); }
//
// CHECK_THROWS asserts an expression throws std::exception. RunAll() prints a
// per-suite summary and returns a process exit code (0 = all passed).

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace osftest
{
	struct Case
	{
		std::string           name;
		std::function<void()> fn;
	};

	inline std::vector<Case>& Registry()
	{
		static std::vector<Case> r;
		return r;
	}

	inline int& CaseFailures()
	{
		static int f = 0;
		return f;
	}

	struct Registrar
	{
		Registrar(std::string a_name, std::function<void()> a_fn)
		{
			Registry().push_back({ std::move(a_name), std::move(a_fn) });
		}
	};

	// Stream a value for diagnostics; falls back to "<?>" for non-streamable types.
	template <class T>
	std::string Show(const T& a_value)
	{
		if constexpr (requires(std::ostream& a_os, const T& a_v) { a_os << a_v; }) {
			std::ostringstream os;
			os << a_value;
			return os.str();
		} else {
			return "<?>";
		}
	}

	inline void Fail(const char* a_expr, const char* a_file, int a_line, const std::string& a_extra = {})
	{
		CaseFailures()++;
		std::printf("    [FAIL] %s:%d: %s%s%s\n", a_file, a_line, a_expr,
			a_extra.empty() ? "" : "  -> ", a_extra.c_str());
	}

	inline int RunAll()
	{
		int passed = 0;
		int failed = 0;
		std::printf("running %zu test case(s)\n", Registry().size());
		for (auto& c : Registry()) {
			const int before = CaseFailures();
			try {
				c.fn();
			} catch (const std::exception& e) {
				Fail("uncaught std::exception", "<case>", 0, e.what());
			} catch (...) {
				Fail("uncaught non-std exception", "<case>", 0);
			}
			if (CaseFailures() == before) {
				passed++;
			} else {
				failed++;
				std::printf("  FAILED: %s\n", c.name.c_str());
			}
		}
		std::printf("\n%d passed, %d failed (%zu cases)\n", passed, failed, Registry().size());
		return failed == 0 ? 0 : 1;
	}
}

#define OSF_TEST_CASE(NAME)                                              \
	static void NAME();                                                  \
	static ::osftest::Registrar osf_reg_##NAME{ #NAME, NAME };           \
	static void NAME()

#define CHECK(COND)                                                      \
	do {                                                                 \
		if (!(COND)) {                                                   \
			::osftest::Fail(#COND, __FILE__, __LINE__);                  \
		}                                                                \
	} while (0)

#define CHECK_EQ(A, B)                                                   \
	do {                                                                 \
		auto&& osf_a = (A);                                              \
		auto&& osf_b = (B);                                              \
		if (!(osf_a == osf_b)) {                                         \
			::osftest::Fail(#A " == " #B, __FILE__, __LINE__,            \
				::osftest::Show(osf_a) + " vs " + ::osftest::Show(osf_b)); \
		}                                                                \
	} while (0)

#define CHECK_NEAR(A, B, EPS)                                            \
	do {                                                                 \
		auto osf_a = (A);                                                \
		auto osf_b = (B);                                                \
		if (!(((osf_a - osf_b) < (EPS)) && ((osf_b - osf_a) < (EPS)))) { \
			::osftest::Fail(#A " ~= " #B, __FILE__, __LINE__,            \
				::osftest::Show(osf_a) + " vs " + ::osftest::Show(osf_b)); \
		}                                                                \
	} while (0)

#define CHECK_THROWS(EXPR)                                               \
	do {                                                                 \
		bool osf_threw = false;                                          \
		try {                                                            \
			(void)(EXPR);                                                \
		} catch (const std::exception&) {                               \
			osf_threw = true;                                            \
		}                                                                \
		if (!osf_threw) {                                                \
			::osftest::Fail(#EXPR " should throw", __FILE__, __LINE__);  \
		}                                                                \
	} while (0)
