#pragma once

#include "Animation/Placement.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <array>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace OSF::Authoring
{
	struct AlignmentRequest
	{
		std::uint64_t sequence{}, playback{}, revision{}, expiresAtUtcMs{};
		std::int32_t handle{};
		std::string op;
		std::vector<Animation::ParticipantPlacement> offsets;
		std::optional<float> speed;
		std::string scene;
		std::vector<std::uint32_t> actors;
		std::uint32_t anchor{};
		std::array<float, 4> view{}; // azimuth/elevation degrees, radius/height metres
	};

	inline AlignmentRequest ParseAlignmentRequest(const nlohmann::json& a_json, std::string_view a_session,
		std::uint64_t a_previousSequence, std::uint64_t a_nowMs)
	{
		using Json = nlohmann::json;
		const auto require = [](bool valid, const char* why) { if (!valid) throw std::runtime_error(why); };
		const auto integer = [&](const Json& value) -> std::uint64_t {
			require(value.is_number_integer() && !value.is_boolean() && value.get<double>() > 0, "Expected a positive integer");
			return value.get<std::uint64_t>();
		};
		require(a_json.is_object() && a_json.value("schema", 0) == 1, "Expected alignment schema 1");
		require(a_json.at("session").get<std::string>() == a_session, "Bridge session changed; reconnect");
		AlignmentRequest request;
		request.sequence = integer(a_json.at("sequence"));
		require(request.sequence > a_previousSequence, "Duplicate or out-of-order command");
		request.expiresAtUtcMs = integer(a_json.at("expiresAtUtcMs"));
		require(request.expiresAtUtcMs > a_nowMs && request.expiresAtUtcMs - a_nowMs <= 60000,
			"Command expired or expiry exceeds 60 seconds");
		request.op = a_json.at("op").get<std::string>();
		require(request.op == "status" || request.op == "set" || request.op == "reset" || request.op == "start" ||
			request.op == "stop" || request.op == "reload" || request.op == "camera", "Unknown alignment operation");
		for (const auto& [key, unused] : a_json.items()) {
			const bool common = key == "schema" || key == "session" || key == "sequence" || key == "op" || key == "expiresAtUtcMs";
			const bool field = request.op == "start" ? (key == "scene" || key == "actors" || key == "anchor") :
				request.op == "set" ? (key == "target" || key == "offsets" || key == "speed") :
				request.op == "camera" ? (key == "target" || key == "view") :
				(request.op == "reset" || request.op == "stop") && key == "target";
			require(common || field, "Unknown field for alignment operation");
		}
		if (request.op == "status" || request.op == "reload") return request;
		if (request.op == "start") {
			request.scene = a_json.at("scene").get<std::string>();
			require(!request.scene.empty() && request.scene.size() <= 512, "Expected a registered scene ID");
			const auto& actors = a_json.at("actors");
			require(actors.is_array() && !actors.empty() && actors.size() <= 32, "Expected 1..32 actor form IDs");
			for (const auto& value : actors) {
				const auto id = integer(value);
				require(id <= 0xffffffffULL && std::find(request.actors.begin(), request.actors.end(), id) == request.actors.end(), "Invalid or duplicate actor form ID");
				request.actors.push_back(static_cast<std::uint32_t>(id));
			}
			const auto anchor = integer(a_json.at("anchor"));
			require(std::find(request.actors.begin(), request.actors.end(), anchor) != request.actors.end(), "Anchor must be a participant");
			request.anchor = static_cast<std::uint32_t>(anchor);
			return request;
		}
		const auto& target = a_json.at("target");
		require(target.is_object() && target.size() == 3, "Target requires handle, playback, revision");
		const auto handle = integer(target.at("handle"));
		require(handle <= 0x7fffffff, "Invalid scene handle");
		request.handle = static_cast<std::int32_t>(handle);
		request.playback = integer(target.at("playback"));
		request.revision = integer(target.at("revision"));
		if (request.op == "reset" || request.op == "stop") return request;
		if (request.op == "camera") {
			const auto& view = a_json.at("view");
			require(view.is_array() && view.size() == 4, "View requires azimuth, elevation, radius, height");
			for (std::size_t i = 0; i < 4; ++i) {
				require(view[i].is_number(), "View components must be numbers");
				request.view[i] = view[i].get<float>();
				require(std::isfinite(request.view[i]), "View components must be finite");
			}
			require(std::abs(request.view[0]) <= 360 && std::abs(request.view[1]) <= 80 &&
				request.view[2] >= 1 && request.view[2] <= 12 && request.view[3] >= .3f && request.view[3] <= 3,
				"View bounds: azimuth +/-360, elevation +/-80, radius 1..12, height .3..3");
			return request;
		}
		if (a_json.contains("offsets")) {
			const auto& offsets = a_json.at("offsets");
			require(offsets.is_array() && !offsets.empty() && offsets.size() <= 32, "Expected 1..32 actor offsets");
			for (const auto& value : offsets) {
				require(value.is_object() && value.size() == 4, "Each offset requires x, y, z, heading");
				for (const auto* key : {"x", "y", "z", "heading"}) require(value.at(key).is_number(), "Offset components must be numbers");
				const float heading = value.at("heading").get<float>();
				require(std::isfinite(heading) && std::abs(heading) <= 180, "Heading adjustment must be within +/-180 degrees");
				Animation::ParticipantPlacement offset{value.at("x").get<float>(), value.at("y").get<float>(),
					value.at("z").get<float>(), heading * 0.017453292519943295f};
				require(Animation::ValidAlignmentOffset(offset), "Position adjustments must be finite and within +/-5 metres");
				request.offsets.push_back(offset);
			}
		}
		if (a_json.contains("speed")) {
			require(a_json.at("speed").is_number(), "Speed must be a number");
			request.speed = a_json.at("speed").get<float>();
			require(std::isfinite(*request.speed) && *request.speed >= 0 && *request.speed <= 4, "Authoring speed must be 0..4");
		}
		require(!request.offsets.empty() || request.speed.has_value(), "Set needs offsets or speed");
		return request;
	}
}
