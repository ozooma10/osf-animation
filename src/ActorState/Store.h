#pragma once

#include <cstdint>
#include <compare>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::ActorState
{
	using Handle = std::uint64_t;
	using Actor = std::uint32_t;
	using Object = std::uint32_t;
	enum class Lifetime : std::uint32_t { Persistent, Session };
	enum class Operation : std::uint32_t { Equip, Preserve, Hide, HideOptional };
	enum class Status : std::uint32_t { Ok, Invalid, Missing, Conflict, Unavailable, Failed };
	struct Result { Status status{Status::Invalid}; Handle handle{}; explicit operator bool() const { return status == Status::Ok; } };
	struct Item {
		Object object{};
		std::uint32_t count{};
		std::uint64_t slots{};
		bool worn{}, skin{}, armor{}, weapon{};
		auto operator<=>(const Item&) const = default;
	};
	struct Request {
		Actor actor{};
		Object object{};
		Operation operation{};
		std::uint32_t group{};
		bool allowAdd{};
	};
	// The backend performs only synchronous inventory operations. Mesh readiness is a separate
	// preparation concern. IDs are world-local; Reset never calls the backend.
	class Backend {
	public:
		virtual ~Backend() = default;
		virtual std::optional<std::vector<Item>> Read(Actor) = 0;
		virtual std::optional<Item> Describe(Object) = 0;
		virtual bool SetWorn(Actor, Object, bool) = 0;
		virtual bool Add(Actor, Object) = 0;
		virtual bool Remove(Actor, Object) = 0;
	};

	struct EquipmentObservation { Item item; std::uint64_t conflictingWornSlots{}; };
	std::optional<EquipmentObservation> ReadEquipment(Backend& backend, Actor actor, Object object);

	struct Revision { std::uint64_t world{}, ownership{}, equipment{}; auto operator<=>(const Revision&) const = default; };
	enum class RevisionCause : std::uint32_t { Ownership, Equipment, Reset };
	struct Notification { std::uint64_t sequence{}; Actor actor{}; RevisionCause cause{}; Revision revision; };
	struct Notifications { std::uint64_t cursor{}, world{}; bool resnapshot{}; std::vector<Notification> events; };
	inline constexpr std::size_t kNotificationLimit = 256;

	// Serial game-context service, deliberately without locks around engine/backend calls.
	// Scopes and claims share a never-recycled token space. Owner strings are diagnostic identity,
	// not security credentials; possession of the scope is the authority to release its claims.
	class Store {
    public:
        struct Change {
            Handle scope{};
            Request request;
            bool releaseGroup{}, replaceGroup{}, optional{};
            std::optional<std::uint32_t> replaceFallbackGroup;
        };
        // Frame-context preparation transactions retain only selected scopes' claims.
        // Rollback never rewinds another owner's state or copies old inventory snapshots.
        Status PreviewPlan(const std::vector<Change>& changes);
        Status ApplyPlan(const std::vector<Change>& changes);
        Handle BeginEdit(const std::vector<Handle>& scopes);
        Status FinishEdit(Handle edit, bool commit);
		explicit Store(Backend& backend) : _backend(backend) {}
		Handle CreateScope(std::string_view owner, Lifetime lifetime, std::uint64_t session = 0);
		Result Acquire(Handle scope, const Request& request, bool replaceGroup = false,
			std::optional<std::uint32_t> replaceFallbackGroup = std::nullopt);
		Status Release(Handle scope, Handle claim);
		Status ReleaseGroup(Handle scope, std::uint32_t group);
		Status CloseScope(Handle scope);
		Status CloseSession(std::uint64_t session);
		Status SetSaveWindow(bool saving);
		Status Reconcile(Actor actor);
		void Reset();
		// Memory-only reads. Journal delivery never calls consumers during an inventory operation.
		Revision ReadRevision(Actor actor) const;
		Notifications Since(std::uint64_t cursor) const;
		std::optional<EquipmentObservation> InspectEquipment(Actor actor, Object object);
		bool HasScope(Handle scope) const { return _scopes.contains(scope); }
		std::optional<std::uint64_t> SessionOf(Handle scope) const {
			const auto it = _scopes.find(scope);
			return it == _scopes.end() ? std::nullopt : std::optional{it->second.session};
		}
		bool Owns(Handle scope, Actor actor, Object object, std::uint32_t group) const;
		std::vector<Object> Kept(Actor actor) const;
		std::vector<Object> Hidden(Actor actor) const;
		std::size_t ClaimCount() const { return _claims.size(); }
		std::size_t PendingActorCount() const { return _records.size(); }
		std::uint64_t ExternalChanges() const { return _externalChanges; }
	private:
		struct Scope { std::string owner; Lifetime lifetime; std::uint64_t session; };
		struct Claim { Handle scope; Request request; };
        struct Edit { std::vector<Handle> scopes; std::map<Handle, Claim> claims; };
		enum class Pending { None, Worn, Unworn, Add, Remove };
		struct Record {
			Item expected;
			bool baselineWorn{}, changed{}, added{};
			Pending pending{Pending::None};
		};
		void Changed(Actor actor, RevisionCause cause);
		std::optional<std::vector<Item>> ReadInventory(Actor actor);
		Handle Next();
		bool Active(const Claim& claim) const;
		enum class Displacement { None, CapturedBaseline, Any };
		Status Apply(Actor actor, Displacement displace = Displacement::None);
		void Observe(std::map<Object, Record>& records, const std::vector<Item>& inventory);
		Status ReconcileAll(const std::vector<Actor>& actors);
		Backend& _backend;
		Handle _next{1};
		bool _saving{}, _preview{};
        std::map<Handle, Edit> _edits;
		std::uint64_t _externalChanges{}, _world{1}, _sequence{};
		std::map<Actor, Revision> _revisions;
		std::map<Actor, std::optional<std::vector<Item>>> _observations;
		std::deque<Notification> _notifications;
		std::map<Handle, Scope> _scopes;
		std::map<Handle, Claim> _claims;
		std::map<Actor, std::map<Object, Record>> _records;
	};
}
