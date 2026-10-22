#include "Check.h"
#include "API/BrowserCatalogJson.h"
#include "API/CatalogJson.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <limits>

namespace
{
	using json = nlohmann::json;
	using OSF::Test::Check;
	using Snapshot = OSF::Catalog::Snapshot;
	namespace Parser = OSF::API::BrowserCatalogJson;

	std::unique_ptr<Snapshot> Legacy(std::string_view text)
	{
		try {
			const auto value = json::parse(text);
			if (!value.is_object() || value.value("schema", 0) != 3) return {};
			return std::make_unique<Snapshot>(value.get<Snapshot>());
		} catch (...) { return {}; }
	}
	void Compare(std::string_view text)
	{
		const auto before = Legacy(text), after = Parser::Parse(text);
		Check(bool(before) == bool(after), "typed SAX parser preserves the previous decoder's acceptance/rejection");
		if (before && after) Check(json(*before) == json(*after), "typed SAX parser preserves every catalog value and array order");
	}
	Snapshot Fixture()
	{
		Snapshot result;
		result.scenes.resize(1);
		auto& scene = result.scenes.front();
		scene.roles.resize(2); scene.stages.resize(2); scene.tags = {"first", "second"};
		for (auto& stage : scene.stages) stage.tags = {"stage"};
		scene.anchorKeywords = {0, 0xFFFFFFFF}; scene.anchorBaseForms = {42};
		result.files.resize(1);
		result.files.front().problems.resize(2);
		result.files.front().species = {"human", "robot"};
		result.files.front().missingClipExamples = {"a", "b"};
		result.errors = {"first error", "second error"};
		result.clips.resize(2); result.sourceClips.resize(2); result.nodes.resize(2);
		for (auto& node : result.nodes) node.tags = {"node"};
		auto values = json(result);
		const auto fill = [](auto&& self, json& value, const std::string& path) -> void {
			if (value.is_object() || value.is_array()) {
				for (auto& item : value.items()) self(self, item.value(), path + "/" + item.key());
			} else if (value.is_string()) value = path + " quote\" slash\\ line\n\xC3\xA9\xF0\x9F\x8E\xAC";
			else if (value.is_boolean()) value = !value.get<bool>();
			else if (value.is_number_float()) value = -1.25;
			else if (value.is_number_unsigned()) value = path.size() * 7u;
			else if (value.is_number_integer()) value = -static_cast<int>(path.size());
		};
		fill(fill, values, ""); values["schema"] = 3;
		return values.get<Snapshot>();
	}
	nlohmann::ordered_json ReverseFields(const json& value)
	{
		if (value.is_object()) {
			auto result = nlohmann::ordered_json::object();
			for (auto it = value.rbegin(); it != value.rend(); ++it) result[it.key()] = ReverseFields(it.value());
			return result;
		}
		if (value.is_array()) {
			auto result = nlohmann::ordered_json::array();
			for (const auto& item : value) result.push_back(ReverseFields(item));
			return result;
		}
		return value;
	}
}

void RunBrowserCatalogJsonTests()
{
	Compare(json(Snapshot{}).dump());
	const auto fixture = Fixture();
	const auto values = json(fixture);
	const auto text = OSF::API::CatalogJson::Serialize(fixture);
	Compare(text); // Actual engine field order, plus alphabetical and reversed JSON keys.
	Compare(values.dump()); Compare(ReverseFields(values).dump());
	Compare("\xEF\xBB\xBF" + text + " \r\n\t");
	auto unknown = values;
	unknown["future"] = {{"schema", 900}, {"nodes", {{{"deep", {nullptr, 2, "value", json::object()}}}}}};
	unknown["scenes"][0]["future"] = {unknown["future"], json::array(), true};
	Compare(unknown.dump());
	for (const auto& prefix : {"\"scenes\":null,", "\"scenes\":[{}],", "\"schema\":\"invalid\",", "\"schema\":2,"})
		Compare("{" + std::string(prefix) + text.substr(1));
	Compare(text.substr(0, text.size() - 1) + ",\"schema\":2}");
	Compare(text.substr(0, text.size() - 1) + ",\"scenes\":[]}");

	// Every required field at every record level must remain required, with its declared type.
	for (const auto* record : {"", "/scenes/0", "/scenes/0/roles/0", "/scenes/0/stages/0", "/files/0",
		"/files/0/problems/0", "/clips/0", "/sourceClips/0", "/nodes/0"}) {
		for (const auto& [key, value] : values.at(json::json_pointer(record)).items()) {
			auto missing = values; missing.at(json::json_pointer(record)).erase(key); Compare(missing.dump());
			auto wrong = values; wrong.at(json::json_pointer(record))[key] = nullptr; Compare(wrong.dump());
		}
	}
	for (const auto* path : {"/schema", "/scenes/0/priority", "/scenes/0/actorCount", "/files/0/schema",
		"/files/0/bytes", "/scenes/0/stages/0/loopSec", "/scenes/0/fade"}) {
		for (const json& number : std::vector<json>{false, true, -1, 0, 3, 3.75, -4.5,
			std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(),
			std::numeric_limits<std::uint64_t>::max(), "3", json::array(), json::object()}) {
			// Negative floating -> unsigned conversion is outside C++'s defined range.
			if (number.is_number_float() && number.get<double>() < 0 &&
				(std::string_view(path) == "/schema" || std::string_view(path) == "/scenes/0/actorCount" ||
				 std::string_view(path) == "/files/0/bytes")) continue;
			auto changed = values; changed.at(json::json_pointer(path)) = number; Compare(changed.dump());
		}
	}
	for (const float number : {0.0f, -0.0f, std::numeric_limits<float>::denorm_min(),
		std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()}) {
		auto changed = values; changed["sourceClips"][0]["seconds"] = number; Compare(changed.dump());
	}
	auto large = fixture;
	large.scenes.resize(80, fixture.scenes.front()); // Force parent vector reallocations around nested arrays.
	large.scenes.front().name.assign(32768, '\n');
	for (int i = 0; i < 32; ++i) large.scenes.back().name += static_cast<char>(i);
	large.files.resize(25, fixture.files.front()); large.nodes.resize(160, fixture.nodes.front());
	large.sourceClips.resize(1000, fixture.sourceClips.front());
	Compare(OSF::API::CatalogJson::Serialize(large));
	for (const auto* invalid : {"", "null", "[]", "{}", "{", "{\"schema\":3,}",
		"{\"future\":\"\xFF\"}", "{\"future\":\"\\uD800\"}", "{\"future\":1e999}"}) Compare(invalid);
	Compare(text + "false"); Compare(text.substr(0, text.size() / 2));
	// Escaped key names are decoded by the library before matching catalog fields.
	Compare("{\"sch\\u0065ma\":3," + json(Snapshot{}).dump().substr(1));
	std::string borrowed = text;
	auto owned = Parser::Parse(borrowed);
	borrowed.assign(borrowed.size(), 'x');
	Check(owned && json(*owned) == values, "parsed strings and nested metadata outlive the borrowed engine callback text");
	Parser::Read(nullptr, &owned);
	Check(!owned, "null callback text fails without leaving a stale output");
	std::vector<std::future<bool>> parallel;
	for (int i = 0; i < 4; ++i) parallel.push_back(std::async(std::launch::async, [&] {
		const auto parsed = Parser::Parse(text); return parsed && json(*parsed) == values;
	}));
	for (auto& result : parallel) Check(result.get(), "concurrent catalog parses have independent owned state");
}

void RunBrowserCatalogJsonBenchmark()
{
	const auto seed = Fixture();
	auto fixture = seed;
	fixture.scenes.resize(1400, seed.scenes.front());
	fixture.files.resize(38, seed.files.front());
	fixture.nodes.resize(2000, seed.nodes.front());
	fixture.sourceClips.resize(6000, seed.sourceClips.front());
	fixture.clips.resize(2000, seed.clips.front());
	const auto text = OSF::API::CatalogJson::Serialize(fixture);
	Compare(text);
	std::vector<double> oldTimes, newTimes;
	const auto measure = [&](auto parse, auto& times) {
		const auto start = std::chrono::steady_clock::now();
		{ const auto parsed = parse(text); Check(parsed && parsed->scenes.size() == 1400, "benchmark parsed complete owned metadata"); }
		times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	};
	for (int i = 0; i < 7; ++i) {
		if (i % 2) { measure(Parser::Parse, newTimes); measure(Legacy, oldTimes); }
		else { measure(Legacy, oldTimes); measure(Parser::Parse, newTimes); }
	}
	std::sort(oldTimes.begin(), oldTimes.end()); std::sort(newTimes.begin(), newTimes.end());
	std::cout << "Catalog parse benchmark (parse + typed conversion + destruction), bytes=" << text.size()
		<< ", DOM medianMs=" << oldTimes[3] << ", SAX medianMs=" << newTimes[3]
		<< ", DOM rangeMs=" << oldTimes.front() << '-' << oldTimes.back()
		<< ", SAX rangeMs=" << newTimes.front() << '-' << newTimes.back() << '\n';
}
