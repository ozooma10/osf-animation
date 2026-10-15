#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace OSF::API
{
	// OSF UI 2.0 limits each EmitEvent to 1 MiB and queues 64 events per view. Keep
	// ordinary payloads intact; pace large JSON documents across frame drains.
	// Main-thread only. The owner must outlive posted callbacks (the bridge is static).
	class BrowserJsonTransport
	{
	public:
		static constexpr std::size_t kDirectBytes = 512u * 1024u;
		static constexpr std::size_t kChunkBytes = 64u * 1024u;
		static constexpr std::size_t kMaxBytes = 32u * 1024u * 1024u;
		static constexpr std::size_t kMaxChunks = 1024;
		static constexpr std::size_t kChunksPerFrame = 4;
		static constexpr std::size_t kMaxTransfers = 8;
		static constexpr std::size_t kMaxBufferedBytes = 2 * kMaxBytes;
		using Emit = std::function<bool(const std::string&, const std::string&, const std::string&)>;
		using Post = std::function<bool(std::function<void()>)>;
		using Error = std::function<void(const std::string&, const std::string&, const char*)>;

		BrowserJsonTransport(Emit a_emit, Post a_post, Error a_error) :
			m_emit(std::move(a_emit)), m_post(std::move(a_post)), m_error(std::move(a_error)) {}

		// Input is valid UTF-8 JSON produced by json::dump(error_handler_t::replace).
		bool Send(std::string a_view, std::string a_type, std::string a_text)
		{
			auto old = std::ranges::find_if(m_pending, [&](const auto& item) {
				return item.view == a_view && item.type == a_type;
			});
			// Bootstrap retries must not restart an already progressing document.
			if (old != m_pending.end() && old->text == a_text) return true;
			if (old != m_pending.end()) m_pending.erase(old);
			if (a_text.size() <= kDirectBytes) return m_emit(a_view, a_type, a_text);
			std::size_t buffered = a_text.size();
			for (const auto& item : m_pending) buffered += item.text.size();
			if (a_text.size() > kMaxBytes || m_pending.size() >= kMaxTransfers || buffered > kMaxBufferedBytes ||
				m_nextId == std::numeric_limits<std::uint64_t>::max()) return false;

			Transfer item{ std::move(a_view), std::move(a_type), std::move(a_text), std::to_string(++m_nextId) };
			for (std::size_t offset = 0; offset < item.text.size();) {
				auto end = std::min(offset + kChunkBytes, item.text.size());
				// Never cut a multibyte code point: each envelope is independently valid UTF-8.
				while (end < item.text.size() && (static_cast<unsigned char>(item.text[end]) & 0xC0) == 0x80) --end;
				item.ends.push_back(end);
				offset = end;
			}
			if (item.ends.size() > kMaxChunks) return false;
			m_pending.push_back(std::move(item));
			return Schedule();
		}

		void Clear()
		{
			m_pending.clear();
			m_scheduled = false;
			++m_epoch;
		}

	private:
		struct Transfer
		{
			std::string view, type, text, id;
			std::vector<std::size_t> ends;
			std::size_t index = 0;
		};
		bool Schedule()
		{
			if (m_scheduled || m_pending.empty()) return true;
			m_scheduled = true;
			const auto epoch = m_epoch;
			if (m_post([this, epoch] { if (epoch == m_epoch) Pump(); })) return true;
			for (const auto& item : m_pending) m_error(item.view, item.type, "frame queue refused transfer");
			Clear();
			return false;
		}
		void Pump()
		{
			m_scheduled = false;
			for (std::size_t sent = 0; sent < kChunksPerFrame && !m_pending.empty(); ++sent) {
				auto item = std::move(m_pending.front());
				m_pending.pop_front();
				const auto start = item.index == 0 ? 0 : item.ends[item.index - 1];
				const nlohmann::json envelope = {
					{ "osfAnimationTransfer", 1 }, { "id", item.id }, { "index", item.index },
					{ "count", item.ends.size() }, { "bytes", item.text.size() },
					{ "data", item.text.substr(start, item.ends[item.index] - start) }
				};
				// Worst-case JSON escaping expands a 64 KiB chunk by 6x, below the host limit.
				if (!m_emit(item.view, item.type, envelope.dump())) {
					m_error(item.view, item.type, "OSF UI refused transfer chunk");
					continue;
				}
				if (++item.index < item.ends.size()) m_pending.push_back(std::move(item));
			}
			Schedule();
		}
		Emit m_emit;
		Post m_post;
		Error m_error;
		std::deque<Transfer> m_pending;
		std::uint64_t m_nextId = 0, m_epoch = 0;
		bool m_scheduled = false;
	};
}
