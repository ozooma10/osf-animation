#include "Check.h"
#include "Authoring/AlignmentProtocol.h"
#include <limits>

int main()
{
	using OSF::Test::Check;
	using Json = nlohmann::json;
	const auto ParseAlignmentRequest = [](const Json& value, std::string_view session, std::uint64_t previous) {
		return OSF::Authoring::ParseAlignmentRequest(value, session, previous, 1000);
	};
	const Json base{{"schema",1}, {"session","current"}, {"sequence",10}, {"op","set"},
		{"expiresAtUtcMs",2000},
		{"target",{{"handle",1},{"playback",2},{"revision",3}}},
		{"offsets",Json::array({{{"x",.01},{"y",-.02},{"z",0},{"heading",90}}})}, {"speed",0}};
	const auto parsed = ParseAlignmentRequest(base, "current", 9);
	Check(parsed.speed == 0 && parsed.offsets.size() == 1, "pause and alignment are one validated command");
	Check(std::abs(parsed.offsets[0].heading - 1.5707963f) < .00001f, "wire degrees convert to runtime radians");
	const auto sum = OSF::Animation::AddPlacement({1,2,3,.5f}, parsed.offsets[0]);
	Check(std::abs(sum.x - 1.01f) < .00001f && std::abs(sum.y - 1.98f) < .00001f && sum.z == 3,
		"translation is additive in the anchor frame, not rotated twice");
	Check(std::abs(sum.heading - 2.0707963f) < .00001f, "heading adds to authored facing");
	const auto reject = [&](Json value, std::string_view reason, std::uint64_t previous = 9) {
		try { (void)ParseAlignmentRequest(value, "current", previous); Check(false, reason); }
		catch (const std::exception&) { Check(true, reason); }
	};
	auto value = base; value["session"] = "previous-process"; reject(value, "stale process rejected");
	value = base; value["expiresAtUtcMs"] = 1000; reject(value, "expired command rejected");
	value = base; value["expiresAtUtcMs"] = 62000; reject(value, "unbounded command lifetime rejected");
	reject(base, "duplicate sequence rejected", 10);
	reject(base, "out-of-order sequence rejected", 11);
	value = base; value["target"]["playback"] = 0; reject(value, "wildcard playback rejected");
	value = base; value["target"]["revision"] = -1; reject(value, "negative revision rejected");
	value = base; value["target"]["handle"] = 4294967296ULL; reject(value, "handle cannot truncate");
	value = base; value["offsets"][0]["x"] = true; reject(value, "boolean is not a distance");
	value = base; value["offsets"][0]["x"] = 5.01; reject(value, "excessive offset rejected");
	value = base; value["offsets"][0]["x"] = std::numeric_limits<double>::infinity(); reject(value, "nonfinite offset rejected");
	value = base; value["offsets"][0]["heading"] = 181; reject(value, "excessive rotation rejected");
	value = base; value["offsets"][0].erase("y"); reject(value, "partial offset batch rejected");
	value = base; value["offsets"] = Json::array(); reject(value, "empty explicit offsets rejected");
	value = base; value["offsets"] = Json::array(); for (int i=0;i<33;++i) value["offsets"].push_back(base["offsets"][0]);
	reject(value, "unbounded actor list rejected");
	value = base; value["speed"] = -1; reject(value, "negative speed rejected");
	value = base; value["speed"] = 4.01; reject(value, "excessive speed rejected");
	value = base; value["offests"] = value["offsets"]; reject(value, "typo rejected rather than ignored");
	value = base; value["op"] = "status"; reject(value, "status cannot carry mutations");
	value = base; value["op"] = "reset"; reject(value, "reset cannot carry offsets");
	value = base; value.erase("offsets"); Check(ParseAlignmentRequest(value,"current",9).speed == 0, "speed-only pause accepted");
	value.erase("speed"); reject(value, "empty mutation rejected");
	value["op"] = "reset"; Check(ParseAlignmentRequest(value,"current",9).op == "reset", "targeted reset accepted");
	value.erase("target"); value["op"] = "status"; Check(ParseAlignmentRequest(value,"current",9).op == "status", "read-only status accepted");
	const auto status = value;
	value["op"] = "start"; value["scene"] = "test.hug"; value["actors"] = {0x5986, 0x14}; value["anchor"] = 0x14;
	Check(ParseAlignmentRequest(value,"current",9).actors.size() == 2, "explicit registered scene cast accepted");
	const auto start = value;
	value["actors"] = {0x14, 0x14}; reject(value, "duplicate cast rejected");
	value = start; value["actors"][0] = 0x100000000ULL; reject(value, "form ID cannot truncate");
	value = start; value["anchor"] = 123; reject(value, "anchor must belong to cast");
	value = start; value["scene"] = ""; reject(value, "empty scene ID rejected");
	value = start; value["speed"] = 0; reject(value, "start cannot smuggle unrelated fields");
	value = status; value["op"] = "stop"; value["target"] = base["target"];
	Check(ParseAlignmentRequest(value,"current",9).op == "stop", "targeted stop accepted");
	value["op"] = "camera"; value["view"] = {90,10,2,1.2};
	Check(ParseAlignmentRequest(value,"current",9).view[2] == 2, "bounded camera view accepted");
	const auto camera = value;
	value["view"][2] = .1; reject(value, "camera radius cannot enter body");
	value = camera; value["view"][3] = 9; reject(value, "camera height bounded");
	value = camera; value["view"][0] = std::numeric_limits<double>::infinity(); reject(value, "nonfinite camera rejected");
	value = status; value["op"] = "reload"; Check(ParseAlignmentRequest(value,"current",9).op == "reload", "content reload accepted");
	value["target"] = base["target"]; reject(value, "reload takes no target");
	return OSF::Test::Finish("Alignment protocol");
}
