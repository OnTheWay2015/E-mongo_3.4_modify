/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/old_update_position_args.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/storage/snapshot_name.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class Timer;
template <typename T>
class StatusWith;

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace rpc {
class OplogQueryMetadata;
class ReplSetMetadata;
}  // namespace rpc

namespace repl {

class ElectCmdRunner;
class FreshnessChecker;
class HandshakeArgs;
class HeartbeatResponseAction;
class LastVote;
class OplogReader;
class ReplSetRequestVotesArgs;
class ReplSetConfig;
class SyncSourceFeedback;
class StorageInterface;
class TopologyCoordinator;
class VoteRequester;

class ReplicationCoordinatorImpl : public ReplicationCoordinator {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorImpl);

public:
    // Takes ownership of the "externalState", "topCoord" and "network" objects.
    ReplicationCoordinatorImpl(const ReplSettings& settings,
                               ReplicationCoordinatorExternalState* externalState,
                               executor::NetworkInterface* network,
                               TopologyCoordinator* topoCoord,
                               StorageInterface* storage,
                               int64_t prngSeed);
    // Takes ownership of the "externalState" and "topCoord" objects.
    ReplicationCoordinatorImpl(const ReplSettings& settings,
                               ReplicationCoordinatorExternalState* externalState,
                               TopologyCoordinator* topoCoord,
                               StorageInterface* storage,
                               ReplicationExecutor* replExec,
                               int64_t prngSeed,
                               stdx::function<bool()>* isDurableStorageEngineFn);
    virtual ~ReplicationCoordinatorImpl();

    // ================== Members of public ReplicationCoordinator API ===================

    virtual void startup(OperationContext* txn) override;

    virtual void shutdown(OperationContext* txn) override;

    virtual ReplicationExecutor* getExecutor() override {
        return &_replExecutor;
    }

    virtual const ReplSettings& getSettings() const override;

    virtual Mode getReplicationMode() const override;

    virtual MemberState getMemberState() const override;

    virtual Status waitForMemberState(MemberState expectedState, Milliseconds timeout) override;

    virtual bool isInPrimaryOrSecondaryState() const override;

    virtual Seconds getSlaveDelaySecs() const override;

    virtual void clearSyncSourceBlacklist() override;

    virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext* txn, const OpTime& opTime, const WriteConcernOptions& writeConcern);

    virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOpForClient(
        OperationContext* txn, const WriteConcernOptions& writeConcern);

    virtual Status stepDown(OperationContext* txn,
                            bool force,
                            const Milliseconds& waitTime,
                            const Milliseconds& stepdownTime);

    virtual bool isMasterForReportingPurposes();

    virtual bool canAcceptWritesForDatabase(StringData dbName);

    bool canAcceptWritesFor(const NamespaceString& ns) override;

    virtual Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions& writeConcern) const;

    virtual Status checkCanServeReadsFor(OperationContext* txn,
                                         const NamespaceString& ns,
                                         bool slaveOk);

    virtual bool shouldRelaxIndexConstraints(const NamespaceString& ns);

    virtual Status setLastOptimeForSlave(const OID& rid, const Timestamp& ts);

    virtual void setMyLastAppliedOpTime(const OpTime& opTime);
    virtual void setMyLastDurableOpTime(const OpTime& opTime);

    virtual void setMyLastAppliedOpTimeForward(const OpTime& opTime);
    virtual void setMyLastDurableOpTimeForward(const OpTime& opTime);

    virtual void resetMyLastOpTimes();

    virtual void setMyHeartbeatMessage(const std::string& msg);

    virtual OpTime getMyLastAppliedOpTime() const override;
    virtual OpTime getMyLastDurableOpTime() const override;

    virtual Status waitUntilOpTimeForRead(OperationContext* txn,
                                          const ReadConcernArgs& settings) override;

    virtual OID getElectionId() override;

    virtual OID getMyRID() const override;

    virtual int getMyId() const override;

    virtual bool setFollowerMode(const MemberState& newState) override;

    virtual ApplierState getApplierState() override;

    virtual void signalDrainComplete(OperationContext* txn,
                                     long long termWhenBufferIsEmpty) override;

    virtual Status waitForDrainFinish(Milliseconds timeout) override;

    virtual void signalUpstreamUpdater() override;

    virtual Status resyncData(OperationContext* txn, bool waitUntilCompleted) override;

    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand(
        ReplSetUpdatePositionCommandStyle commandStyle) const override;

    virtual Status processReplSetGetStatus(BSONObjBuilder* result,
                                           ReplSetGetStatusResponseStyle responseStyle) override;

    virtual void fillIsMasterForReplSet(IsMasterResponse* result) override;

    virtual void appendSlaveInfoData(BSONObjBuilder* result) override;

    virtual ReplSetConfig getConfig() const override;

    virtual void processReplSetGetConfig(BSONObjBuilder* result) override;

    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) override;

    virtual void advanceCommitPoint(const OpTime& committedOpTime) override;

    virtual void cancelAndRescheduleElectionTimeout() override;

    virtual Status setMaintenanceMode(bool activate) override;

    virtual bool getMaintenanceMode() override;

    virtual Status processReplSetSyncFrom(OperationContext* txn,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj) override;

    virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                    ReplSetHeartbeatResponse* response) override;

    virtual Status processReplSetReconfig(OperationContext* txn,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetInitiate(OperationContext* txn,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetGetRBID(BSONObjBuilder* resultObj) override;

    virtual void incrementRollbackID() override;

    virtual Status processReplSetFresh(const ReplSetFreshArgs& args,
                                       BSONObjBuilder* resultObj) override;

    virtual Status processReplSetElect(const ReplSetElectArgs& args,
                                       BSONObjBuilder* response) override;

    virtual Status processReplSetUpdatePosition(const OldUpdatePositionArgs& updates,
                                                long long* configVersion) override;
    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                long long* configVersion) override;

    virtual Status processHandshake(OperationContext* txn, const HandshakeArgs& handshake) override;

    virtual bool buildsIndexes() override;

    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op,
                                                       bool durablyWritten) override;

    virtual std::vector<HostAndPort> getOtherNodesInReplSet() const override;

    virtual WriteConcernOptions getGetLastErrorDefault() override;

    virtual Status checkReplEnabledForCommand(BSONObjBuilder* result) override;

    virtual bool isReplEnabled() const override;

    virtual HostAndPort chooseNewSyncSource(const OpTime& lastOpTimeFetched) override;

    virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) override;

    virtual void resetLastOpTimesFromOplog(OperationContext* txn) override;

    virtual bool shouldChangeSyncSource(
        const HostAndPort& currentSource,
        const rpc::ReplSetMetadata& replMetadata,
        boost::optional<rpc::OplogQueryMetadata> oqMetadata) override;

    virtual OpTime getLastCommittedOpTime() const override;

    virtual Status processReplSetRequestVotes(OperationContext* txn,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response) override;

    virtual void prepareReplMetadata(const BSONObj& metadataRequestObj,
                                     const OpTime& lastOpTimeFromClient,
                                     BSONObjBuilder* builder) const override;

    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response) override;

    virtual bool isV1ElectionProtocol() const override;

    virtual bool getWriteConcernMajorityShouldJournal() override;

    virtual void summarizeAsHtml(ReplSetHtmlSummary* s) override;

    virtual void dropAllSnapshots() override;
    /**
     * Get current term from topology coordinator
     */
    virtual long long getTerm() override;

    virtual Status updateTerm(OperationContext* txn, long long term) override;

    virtual SnapshotName reserveSnapshotName(OperationContext* txn) override;

    virtual void forceSnapshotCreation() override;

    virtual void createSnapshot(OperationContext* txn,
                                OpTime timeOfSnapshot,
                                SnapshotName name) override;

    virtual OpTime getCurrentCommittedSnapshotOpTime() const override;

    virtual void waitUntilSnapshotCommitted(OperationContext* txn,
                                            const SnapshotName& untilSnapshot) override;

    virtual void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual size_t getNumUncommittedSnapshots() override;

    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) override;

    virtual ReplSettings::IndexPrefetchConfig getIndexPrefetchConfig() const override;
    virtual void setIndexPrefetchConfig(const ReplSettings::IndexPrefetchConfig cfg) override;

    virtual Status stepUpIfEligible() override;

    virtual Status abortCatchupIfNeeded() override;

    // ================== Test support API ===================

    /**
     * If called after startReplication(), blocks until all asynchronous
     * activities associated with replication start-up complete.
     */
    void waitForStartUpComplete_forTest();

    /**
     * Gets the replica set configuration in use by the node.
     */
    ReplSetConfig getReplicaSetConfig_forTest();

    /**
     * Returns scheduled time of election timeout callback.
     * Returns Date_t() if callback is not scheduled.
     */
    Date_t getElectionTimeout_forTest() const;

    /*
     * Return a randomized offset amount that is scaled in proportion to the size of the
     * _electionTimeoutPeriod.
     */
    Milliseconds getRandomizedElectionOffset_forTest();

    /**
     * Returns the scheduled time of the priority takeover callback. If a priority
     * takeover has not been scheduled, returns boost::none.
     */
    boost::optional<Date_t> getPriorityTakeover_forTest() const;

    /**
     * Simple wrappers around _setLastOptime_inlock to make it easier to test.
     */
    Status setLastAppliedOptime_forTest(long long cfgVer, long long memberId, const OpTime& opTime);
    Status setLastDurableOptime_forTest(long long cfgVer, long long memberId, const OpTime& opTime);

    /**
     * Non-blocking version of setFollowerMode.
     * Returns event handle that we can use to wait for the operation to complete.
     * When the operation is complete (wait() returns), 'success' will be set to true
     * if the member state has been set successfully.
     */
    ReplicationExecutor::EventHandle setFollowerMode_nonBlocking(const MemberState& newState,
                                                                 bool* success);

    /**
     * Non-blocking version of updateTerm.
     * Returns event handle that we can use to wait for the operation to complete.
     * When the operation is complete (waitForEvent() returns), 'updateResult' will be set
     * to a status telling if the term increased or a stepdown was triggered.

     */
    ReplicationExecutor::EventHandle updateTerm_forTest(
        long long term, TopologyCoordinator::UpdateTermResult* updateResult);

    /**
     * If called after _startElectSelfV1(), blocks until all asynchronous
     * activities associated with election complete.
     */
    void waitForElectionFinish_forTest();

    /**
     * If called after _startElectSelfV1(), blocks until all asynchronous
     * activities associated with election dry run complete, including writing
     * last vote and scheduling the real election.
     */
    void waitForElectionDryRunFinish_forTest();

private:
    using CallbackFn = executor::TaskExecutor::CallbackFn;

    using CallbackHandle = executor::TaskExecutor::CallbackHandle;

    using EventHandle = executor::TaskExecutor::EventHandle;

    using ScheduleFn = stdx::function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        const executor::TaskExecutor::CallbackFn& work)>;

    struct SnapshotInfo {
        OpTime opTime;
        SnapshotName name;

        bool operator==(const SnapshotInfo& other) const {
            return std::tie(opTime, name) == std::tie(other.opTime, other.name);
        }
        bool operator!=(const SnapshotInfo& other) const {
            return std::tie(opTime, name) != std::tie(other.opTime, other.name);
        }
        bool operator<(const SnapshotInfo& other) const {
            return std::tie(opTime, name) < std::tie(other.opTime, other.name);
        }
        bool operator<=(const SnapshotInfo& other) const {
            return std::tie(opTime, name) <= std::tie(other.opTime, other.name);
        }
        bool operator>(const SnapshotInfo& other) const {
            return std::tie(opTime, name) > std::tie(other.opTime, other.name);
        }
        bool operator>=(const SnapshotInfo& other) const {
            return std::tie(opTime, name) >= std::tie(other.opTime, other.name);
        }
        std::string toString() const;
    };

    class LoseElectionGuardV1;
    class LoseElectionDryRunGuardV1;

    ReplicationCoordinatorImpl(const ReplSettings& settings,
                               ReplicationCoordinatorExternalState* externalState,
                               TopologyCoordinator* topCoord,
                               StorageInterface* storage,
                               int64_t prngSeed,
                               executor::NetworkInterface* network,
                               ReplicationExecutor* replExec,
                               stdx::function<bool()>* isDurableStorageEngineFn);
    /**
     * Configuration states for a replica set node.
     *
     * Transition diagram:
     *
     * PreStart ------------------> ReplicationDisabled
     *    |
     *    |
     *    v
     * StartingUp -------> Uninitialized <------> Initiating
     *         \                     ^               |
     *          -------              |               |
     *                 |             |               |
     *                 v             v               |
     * Reconfig <---> Steady <----> HBReconfig       |
     *                    ^                          /
     *                    |                         /
     *                     \                       /
     *                      -----------------------
     */
    enum ConfigState {
        kConfigPreStart,
        kConfigStartingUp,
        kConfigReplicationDisabled,
        kConfigUninitialized,
        kConfigSteady,
        kConfigInitiating,
        kConfigReconfiguring,
        kConfigHBReconfiguring
    };

    /**
     * Type describing actions to take after a change to the MemberState _memberState.
     */
    enum PostMemberStateUpdateAction {
        kActionNone,
        kActionCloseAllConnections,  // Also indicates that we should clear sharding state.
        kActionFollowerModeStateChange,
        kActionWinElection,
        kActionStartSingleNodeElection
    };

    // Abstract struct that holds information about clients waiting for replication.
    // Subclasses need to define how to notify them.
    struct Waiter {
        Waiter(OpTime _opTime, const WriteConcernOptions* _writeConcern);
        virtual ~Waiter() = default;

        BSONObj toBSON() const;
        std::string toString() const;
        // It is invalid to call notify_inlock() unless holding ReplicationCoordinatorImpl::_mutex.
        virtual void notify_inlock() = 0;

        const OpTime opTime;
        const WriteConcernOptions* writeConcern = nullptr;
    };

    // When ThreadWaiter gets notified, it will signal the conditional variable.
    //
    // This is used when a thread wants to block inline until the opTime is reached with the given
    // writeConcern.
    struct ThreadWaiter : public Waiter {
        ThreadWaiter(OpTime _opTime,
                     const WriteConcernOptions* _writeConcern,
                     stdx::condition_variable* _condVar);
        void notify_inlock() override;

        stdx::condition_variable* condVar = nullptr;
    };

    // When the waiter is notified, finishCallback will be called while holding replCoord _mutex
    // since WaiterLists are protected by _mutex.
    //
    // This is used when we want to run a callback when the opTime is reached.
    struct CallbackWaiter : public Waiter {
        using FinishFunc = stdx::function<void()>;

        CallbackWaiter(OpTime _opTime, FinishFunc _finishCallback);
        void notify_inlock() override;

        // The callback that will be called when this waiter is notified.
        FinishFunc finishCallback = nullptr;
    };

    class WaiterGuard;

    class WaiterList {
    public:
        using WaiterType = Waiter*;

        // Adds waiter into the list.
        void add_inlock(WaiterType waiter);
        // Returns whether waiter is found and removed.
        bool remove_inlock(WaiterType waiter);
        // Signals and removes all waiters that satisfy the condition.
        void signalAndRemoveIf_inlock(stdx::function<bool(WaiterType)> fun);
        // Signals and removes all waiters from the list.
        void signalAndRemoveAll_inlock();

    private:
        std::vector<WaiterType> _list;
    };

    // Struct that holds information about nodes in this replication group, mainly used for
    // tracking replication progress for write concern satisfaction.
    struct SlaveInfo {
        // Our last known OpTime that this slave has applied and journaled to.
        OpTime lastDurableOpTime;
        // Our last known OpTime that this slave has applied, whether journaled or unjournaled.
        OpTime lastAppliedOpTime;
        HostAndPort hostAndPort;  // Client address of the slave.
        int memberId =
            -1;   // Id of the node in the replica set config, or -1 if we're not a replSet.
        OID rid;  // RID of the node.
        bool self = false;  // Whether this SlaveInfo stores the information about ourself
        Date_t lastUpdate =
            Date_t::max();  // The last time we heard from this node; used for liveness detection
        bool down = false;  // Indicator set when lastUpdate time exceeds the election timeout.

        BSONObj toBSON() const;
        std::string toString() const;
    };

    typedef std::vector<SlaveInfo> SlaveInfoVector;

    typedef std::vector<ReplicationExecutor::CallbackHandle> HeartbeatHandles;

    // The state and logic of primary catchup.
    //
    // When start() is called, CatchupState will schedule the timeout callback. When we get
    // responses of the latest heartbeats from all nodes, the target time (opTime of _waiter) is
    // set.
    // The primary exits catchup mode when any of the following happens.
    //   1) My last applied optime reaches the target optime, if we've received a heartbeat from all
    //      nodes.
    //   2) Catchup timeout expires.
    //   3) Primary steps down.
    //   4) The primary has to roll back to catch up.
    //   5) The primary is too stale to catch up.
    //
    // On abort, the state resets the pointer to itself in ReplCoordImpl. In other words, the
    // life cycle of the state object aligns with the conceptual state.
    // In shutdown, the timeout callback will be canceled by the executor and the state is safe to
    // destroy.
    //
    // Any function of the state must be called while holding _mutex.
    class CatchupState {
    public:
        CatchupState(ReplicationCoordinatorImpl* repl) : _repl(repl) {}
        // start() can only be called once.
        void start_inlock();
        // Reset the state itself to destruct the state.
        void abort_inlock();
        // Heartbeat calls this function to update the target optime.
        void signalHeartbeatUpdate_inlock();

    private:
        ReplicationCoordinatorImpl* _repl;  // Not owned.
        // Callback handle used to cancel a scheduled catchup timeout callback.
        ReplicationExecutor::CallbackHandle _timeoutCbh;
        // Handle to a Waiter that contains the current target optime to reach after which
        // we can exit catchup mode.
        std::unique_ptr<CallbackWaiter> _waiter;
    };

    /**
     * Appends a "replicationProgress" section with data for each member in set.
     */
    void _appendSlaveInfoData_inlock(BSONObjBuilder* result);

    /**
     * Looks up the SlaveInfo in _slaveInfo associated with the given RID and returns a pointer
     * to it, or returns NULL if there is no SlaveInfo with the given RID.
     */
    SlaveInfo* _findSlaveInfoByRID_inlock(const OID& rid);

    /**
     * Looks up the SlaveInfo in _slaveInfo associated with the given member ID and returns a
     * pointer to it, or returns NULL if there is no SlaveInfo with the given member ID.
     */
    SlaveInfo* _findSlaveInfoByMemberID_inlock(int memberID);

    /**
     * Adds the given SlaveInfo to _slaveInfo and wakes up any threads waiting for replication
     * that now have their write concern satisfied.  Only valid to call in master/slave setups.
     */
    void _addSlaveInfo_inlock(const SlaveInfo& slaveInfo);

    /**
     * Updates the durableOpTime field on the item in _slaveInfo pointed to by 'slaveInfo' with the
     * given OpTime 'opTime' and wakes up any threads waiting for replication that now have their
     * write concern satisfied.
     */
    void _updateSlaveInfoDurableOpTime_inlock(SlaveInfo* slaveInfo, const OpTime& opTime);

    /**
     * Updates the appliedOpTime field on the item in _slaveInfo pointed to by 'slaveInfo' with the
     * given OpTime 'opTime' and wakes up any threads waiting for replication that now have their
     * write concern satisfied.
     */
    void _updateSlaveInfoAppliedOpTime_inlock(SlaveInfo* slaveInfo, const OpTime& opTime);

    /**
     * Returns the index into _slaveInfo where data corresponding to ourself is stored.
     * For more info on the rules about how we know where our entry is, see the comment for
     * _slaveInfo.
     */
    size_t _getMyIndexInSlaveInfo_inlock() const;

    void _resetMyLastOpTimes_inlock();

    /**
     * Returns the _writeConcernMajorityJournalDefault of our current _rsConfig.
     */
    bool getWriteConcernMajorityShouldJournal_inlock() const;

    /**
     * Returns the OpTime of the current committed snapshot, if one exists.
     */
    OpTime _getCurrentCommittedSnapshotOpTime_inlock() const;

    /**
     * Helper method that removes entries from _slaveInfo if they correspond to a node
     * with a member ID that is not in the current replica set config.  Will always leave an
     * entry for ourself at the beginning of _slaveInfo, even if we aren't present in the
     * config.
     */
    void _updateSlaveInfoFromConfig_inlock();

    /**
     * Helper to update our saved config, cancel any pending heartbeats, and kick off sending
     * new heartbeats based on the new config.  Must *only* be called from within the
     * ReplicationExecutor context.
     *
     * Returns an action to be performed after unlocking _mutex, via
     * _performPostMemberStateUpdateAction.
     */
    PostMemberStateUpdateAction _setCurrentRSConfig_inlock(const ReplSetConfig& newConfig,
                                                           int myIndex);

    /**
     * Updates the last committed OpTime to be "committedOpTime" if it is more recent than the
     * current last committed OpTime.
     */
    void _advanceCommitPoint_inlock(const OpTime& committedOpTime);

    /**
     * Helper to wake waiters in _replicationWaiterList that are doneWaitingForReplication.
     */
    void _wakeReadyWaiters_inlock();

    /**
     * Scheduled to cause the ReplicationCoordinator to reconsider any state that might
     * need to change as a result of time passing - for instance becoming PRIMARY when a single
     * node replica set member's stepDown period ends.
     */
    void _handleTimePassing(const ReplicationExecutor::CallbackArgs& cbData);

    /**
     * Helper method for _awaitReplication that takes an already locked unique_lock, but leaves
     * operation timing to the caller.
     */
    Status _awaitReplication_inlock(stdx::unique_lock<stdx::mutex>* lock,
                                    OperationContext* txn,
                                    const OpTime& opTime,
                                    SnapshotName minSnapshot,
                                    const WriteConcernOptions& writeConcern);

    /**
     * Returns true if the given writeConcern is satisfied up to "optime" or is unsatisfiable.
     *
     * If the writeConcern is 'majority', also waits for _currentCommittedSnapshot to be newer than
     * minSnapshot.
     */
    bool _doneWaitingForReplication_inlock(const OpTime& opTime,
                                           SnapshotName minSnapshot,
                                           const WriteConcernOptions& writeConcern);

    /**
     * Helper for _doneWaitingForReplication_inlock that takes an integer write concern.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    bool _haveNumNodesReachedOpTime_inlock(const OpTime& opTime, int numNodes, bool durablyWritten);

    /**
     * Helper for _doneWaitingForReplication_inlock that takes a tag pattern representing a
     * named write concern mode.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    bool _haveTaggedNodesReachedOpTime_inlock(const OpTime& opTime,
                                              const ReplSetTagPattern& tagPattern,
                                              bool durablyWritten);

    Status _checkIfWriteConcernCanBeSatisfied_inlock(const WriteConcernOptions& writeConcern) const;

    /**
     * Helper that checks if an OpTime has been replicated to a majority of nodes.
     */
    bool _isOpTimeMajorityConfirmed(const OpTime& opTime);

    /**
     * Triggers all callbacks that are blocked waiting for new heartbeat data
     * to decide whether or not to finish a step down.
     */
    void _signalStepDownWaiter_inlock();

    /**
     * Non-blocking helper method for the stepDown method, that represents executing
     * one attempt to step down. See implementation of this method and stepDown for
     * details.
     */
    bool _tryToStepDown(Date_t waitUntil, Date_t stepdownUntil, bool force);

    OID _getMyRID_inlock() const;

    int _getMyId_inlock() const;

    OpTime _getMyLastAppliedOpTime_inlock() const;
    OpTime _getMyLastDurableOpTime_inlock() const;

    /**
     * Bottom half of setFollowerMode.
     *
     * May reschedule itself after the current election, so it is not sufficient to
     * wait for a callback scheduled to execute this method to complete.  Instead,
     * supply an event, "finishedSettingFollowerMode", and wait for that event to
     * be signaled.  Do not observe "*success" until after the event is signaled.
     */
    void _setFollowerModeFinish(const MemberState& newState,
                                const ReplicationExecutor::EventHandle& finishedSettingFollowerMode,
                                bool* success);

    /**
     * Helper method for updating our tracking of the last optime applied by a given node.
     * This is only valid to call on replica sets.
     * "configVersion" will be populated with our config version if it and the configVersion
     * of "args" differ.
     *
     * The OldUpdatePositionArgs version provides support for the pre-3.2.4 format of
     * UpdatePositionArgs.
     */
    Status _setLastOptime_inlock(const OldUpdatePositionArgs::UpdateInfo& args,
                                 long long* configVersion);
    Status _setLastOptime_inlock(const UpdatePositionArgs::UpdateInfo& args,
                                 long long* configVersion);

    /**
     * This function will report our position externally (like upstream) if necessary.
     *
     * Takes in a unique lock, that must already be locked, on _mutex.
     *
     * Lock will be released after this method finishes.
     */
    void _reportUpstream_inlock(stdx::unique_lock<stdx::mutex> lock);

    /**
     * Helpers to set the last applied and durable OpTime.
     */
    void _setMyLastAppliedOpTime_inlock(const OpTime& opTime, bool isRollbackAllowed);
    void _setMyLastDurableOpTime_inlock(const OpTime& opTime, bool isRollbackAllowed);

    /**
     * Schedules a heartbeat to be sent to "target" at "when". "targetIndex" is the index
     * into the replica set config members array that corresponds to the "target", or -1 if
     * "target" is not in _rsConfig.
     */
    void _scheduleHeartbeatToTarget(const HostAndPort& target, int targetIndex, Date_t when);

    /**
     * Processes each heartbeat response.
     *
     * Schedules additional heartbeats, triggers elections and step downs, etc.
     */
    void _handleHeartbeatResponse(const ReplicationExecutor::RemoteCommandCallbackArgs& cbData,
                                  int targetIndex);

    void _handleHeartbeatResponseV1(const ReplicationExecutor::RemoteCommandCallbackArgs& cbData,
                                    int targetIndex);

    void _trackHeartbeatHandle(const StatusWith<ReplicationExecutor::CallbackHandle>& handle);

    void _untrackHeartbeatHandle(const ReplicationExecutor::CallbackHandle& handle);

    /*
     * Return a randomized offset amount that is scaled in proportion to the size of the
     * _electionTimeoutPeriod. Used to add randomization to an election timeout.
     */
    Milliseconds _getRandomizedElectionOffset();

    /**
     * Helper for _handleHeartbeatResponse.
     *
     * Updates the lastDurableOpTime and lastAppliedOpTime associated with the member at
     * "memberIndex" in our config.
     */
    void _updateOpTimesFromHeartbeat_inlock(int targetIndex,
                                            const OpTime& durableOpTime,
                                            const OpTime& appliedOpTime);

    /**
     * Starts a heartbeat for each member in the current config.  Called while holding _topoMutex
     * and replCoord _mutex.
     */
    void _startHeartbeats_inlock();

    /**
     * Cancels all heartbeats.  Called while holding _topoMutex and replCoord _mutex.
     */
    void _cancelHeartbeats_inlock();

    /**
     * Cancels all heartbeats, then starts a heartbeat for each member in the current config.
     * Called while holding _topoMutex and replCoord _mutex.
     */
    void _restartHeartbeats_inlock();

    /**
     * Asynchronously sends a heartbeat to "target". "targetIndex" is the index
     * into the replica set config members array that corresponds to the "target", or -1 if
     * we don't have a valid replica set config.
     *
     * Scheduled by _scheduleHeartbeatToTarget.
     */
    void _doMemberHeartbeat(ReplicationExecutor::CallbackArgs cbData,
                            const HostAndPort& target,
                            int targetIndex);


    MemberState _getMemberState_inlock() const;

    /**
     * Starts loading the replication configuration from local storage, and if it is valid,
     * schedules a callback (of _finishLoadLocalConfig) to set it as the current replica set
     * config (sets _rsConfig and _thisMembersConfigIndex).
     * Returns true if it finishes loading the local config, which most likely means there
     * was no local config at all or it was invalid in some way, and false if there was a valid
     * config detected but more work is needed to set it as the local config (which will be
     * handled by the callback to _finishLoadLocalConfig).
     */
    bool _startLoadLocalConfig(OperationContext* txn);

    /**
     * Callback that finishes the work started in _startLoadLocalConfig and sets _rsConfigState
     * to kConfigSteady, so that we can begin processing heartbeats and reconfigs.
     */
    void _finishLoadLocalConfig(const ReplicationExecutor::CallbackArgs& cbData,
                                const ReplSetConfig& localConfig,
                                const StatusWith<OpTime>& lastOpTimeStatus,
                                const StatusWith<LastVote>& lastVoteStatus);

    /**
     * Start replicating data, and does an initial sync if needed first.
     */
    void _startDataReplication(OperationContext* txn,
                               stdx::function<void()> startCompleted = nullptr);

    /**
     * Stops replicating data by stopping the applier, fetcher and such.
     */
    void _stopDataReplication(OperationContext* txn);

    /**
     * Finishes the work of processReplSetInitiate() while holding _topoMutex, in the event of
     * a successful quorum check.
     */
    void _finishReplSetInitiate(const ReplSetConfig& newConfig, int myIndex);

    /**
     * Finishes the work of processReplSetReconfig while holding _topoMutex, in the event of
     * a successful quorum check.
     * Signals 'finishedEvent' on successful completion.
     */
    void _finishReplSetReconfig(const ReplicationExecutor::CallbackArgs& cbData,
                                const ReplSetConfig& newConfig,
                                int myIndex,
                                ReplicationExecutor::EventHandle finishedEvent);

    /**
     * Changes _rsConfigState to newState, and notify any waiters.
     */
    void _setConfigState_inlock(ConfigState newState);

    /**
     * Updates the cached value, _memberState, to match _topCoord's reported
     * member state, from getMemberState().
     *
     * Returns an enum indicating what action to take after releasing _mutex, if any.
     * Call performPostMemberStateUpdateAction on the return value after releasing
     * _mutex.
     */
    PostMemberStateUpdateAction _updateMemberStateFromTopologyCoordinator_inlock();

    /**
     * Performs a post member-state update action.  Do not call while holding _mutex.
     */
    void _performPostMemberStateUpdateAction(PostMemberStateUpdateAction action);

    /**
     * Begins an attempt to elect this node.
     * Called after an incoming heartbeat changes this node's view of the set such that it
     * believes it can be elected PRIMARY.
     * For proper concurrency, must be called while holding _topoMutex.
     *
     * For old style elections the election path is:
     *      _startElectSelf()
     *      _onFreshnessCheckComplete()
     *      _onElectCmdRunnerComplete()
     * For V1 (raft) style elections the election path is:
     *      _startElectSelfV1()
     *      _onDryRunComplete()
     *      _writeLastVoteForMyElection()
     *      _startVoteRequester()
     *      _onVoteRequestComplete()
     */
    void _startElectSelf();
    void _startElectSelfV1();

    /**
     * Callback called when the FreshnessChecker has completed; checks the results and
     * decides whether to continue election proceedings.
     **/
    void _onFreshnessCheckComplete();

    /**
     * Callback called when the ElectCmdRunner has completed; checks the results and
     * decides whether to complete the election and change state to primary.
     **/
    void _onElectCmdRunnerComplete();

    /**
     * Callback called when the dryRun VoteRequester has completed; checks the results and
     * decides whether to conduct a proper election.
     * "originalTerm" was the term during which the dry run began, if the term has since
     * changed, do not run for election.
     */
    void _onDryRunComplete(long long originalTerm);

    /**
     * Writes the last vote in persistent storage after completing dry run successfully.
     * This job will be scheduled to run in DB worker threads.
     */
    void _writeLastVoteForMyElection(LastVote lastVote,
                                     const ReplicationExecutor::CallbackArgs& cbData);

    /**
     * Starts VoteRequester to run the real election when last vote write has completed.
     */
    void _startVoteRequester(long long newTerm);

    /**
     * Callback called when the VoteRequester has completed; checks the results and
     * decides whether to change state to primary and alert other nodes of our primary-ness.
     * "originalTerm" was the term during which the election began, if the term has since
     * changed, do not step up as primary.
     */
    void _onVoteRequestComplete(long long originalTerm);

    /**
     * Callback called after a random delay, to prevent repeated election ties.
     */
    void _recoverFromElectionTie(const ReplicationExecutor::CallbackArgs& cbData);

    /**
     * Removes 'host' from the sync source blacklist. If 'host' isn't found, it's simply
     * ignored and no error is thrown.
     *
     * Must be scheduled as a callback.
     */
    void _unblacklistSyncSource(const ReplicationExecutor::CallbackArgs& cbData,
                                const HostAndPort& host);

    /**
     * Schedules a request that the given host step down; logs any errors.
     */
    void _requestRemotePrimaryStepdown(const HostAndPort& target);

    /**
     * Schedules stepdown to run with the global exclusive lock.
     */
    ReplicationExecutor::EventHandle _stepDownStart(bool hasMutex);

    /**
     * Completes a step-down of the current node.  Must be run with a global
     * shared or global exclusive lock.
     * Signals 'finishedEvent' on successful completion.
     */
    void _stepDownFinish(const ReplicationExecutor::CallbackArgs& cbData,
                         const ReplicationExecutor::EventHandle& finishedEvent);

    /**
     * Schedules a replica set config change.
     */
    void _scheduleHeartbeatReconfig(const ReplSetConfig& newConfig);

    /**
     * Callback that continues a heartbeat-initiated reconfig after a running election
     * completes.
     */
    void _heartbeatReconfigAfterElectionCanceled(const ReplicationExecutor::CallbackArgs& cbData,
                                                 const ReplSetConfig& newConfig);

    /**
     * Method to write a configuration transmitted via heartbeat message to stable storage.
     */
    void _heartbeatReconfigStore(const ReplicationExecutor::CallbackArgs& cbd,
                                 const ReplSetConfig& newConfig);

    /**
     * Conclusion actions of a heartbeat-triggered reconfiguration.
     */
    void _heartbeatReconfigFinish(const ReplicationExecutor::CallbackArgs& cbData,
                                  const ReplSetConfig& newConfig,
                                  StatusWith<int> myIndex);

    /**
     * Utility method that schedules or performs actions specified by a HeartbeatResponseAction
     * returned by a TopologyCoordinator::processHeartbeatResponse(V1) call with the given
     * value of "responseStatus".
     * 'hasMutex' is true if the caller is holding _mutex.  TODO(SERVER-27083): Remove this.
     */
    void _handleHeartbeatResponseAction(const HeartbeatResponseAction& action,
                                        const StatusWith<ReplSetHeartbeatResponse>& responseStatus,
                                        bool hasMutex);

    /**
     * Scan the SlaveInfoVector and determine the highest OplogEntry present on a majority of
     * servers; set _lastCommittedOpTime to this new entry, if greater than the current entry.
     */
    void _updateLastCommittedOpTime_inlock();

    /**
     * This is used to set a floor of "newOpTime" on the OpTimes we will consider committed.
     * This prevents entries from before our election from counting as committed in our view,
     * until our election (the "newOpTime" op) has been committed.
     */
    void _setFirstOpTimeOfMyTerm(const OpTime& newOpTime);

    /**
     * Callback that attempts to set the current term in topology coordinator and
     * relinquishes primary if the term actually changes and we are primary.
     * *updateTermResult will be the result of the update term attempt.
     * Returns the finish event if it does not finish in this function, for example,
     * due to stepdown, otherwise the returned EventHandle is invalid.
     */
    EventHandle _updateTerm_incallback(
        long long term, TopologyCoordinator::UpdateTermResult* updateTermResult = nullptr);

    /**
     * Callback that processes the ReplSetMetadata returned from a command run against another
     * replica set member and so long as the config version in the metadata matches the replica set
     * config version this node currently has, updates the current term.
     *
     * This does NOT update this node's notion of the commit point.
     *
     * Returns the finish event which is invalid if the process has already finished.
     */
    EventHandle _processReplSetMetadata_incallback(const rpc::ReplSetMetadata& replMetadata);

    /**
     * Prepares a metadata object for ReplSetMetadata.
     */
    void _prepareReplSetMetadata_inlock(const OpTime& lastOpTimeFromClient,
                                        BSONObjBuilder* builder) const;

    /**
     * Prepares a metadata object for OplogQueryMetadata.
     */
    void _prepareOplogQueryMetadata_inlock(BSONObjBuilder* builder) const;

    /**
     * Blesses a snapshot to be used for new committed reads.
     */
    void _updateCommittedSnapshot_inlock(SnapshotInfo newCommittedSnapshot);

    /**
     * Drops all snapshots and clears the "committed" snapshot.
     */
    void _dropAllSnapshots_inlock();

    /**
     * Bottom half of _scheduleNextLivenessUpdate.
     * Must be called with _topoMutex held.
     */
    void _scheduleNextLivenessUpdate_inlock();

    /**
     * Callback which marks downed nodes as down, triggers a stepdown if a majority of nodes are no
     * longer visible, and reschedules itself.
     */
    void _handleLivenessTimeout(const ReplicationExecutor::CallbackArgs& cbData);

    /**
     * If "updatedMemberId" is the current _earliestMemberId, cancels the current
     * _handleLivenessTimeout callback and calls _scheduleNextLivenessUpdate to schedule a new one.
     * Returns immediately otherwise.
     */
    void _cancelAndRescheduleLivenessUpdate_inlock(int updatedMemberId);

    /**
     * Cancels all outstanding _priorityTakeover callbacks.
     */
    void _cancelPriorityTakeover_inlock();

    /**
     * Cancels the current _handleElectionTimeout callback and reschedules a new callback.
     * Returns immediately otherwise.
     */
    void _cancelAndRescheduleElectionTimeout_inlock();

    /**
     * Callback which starts an election if this node is electable and using protocolVersion 1.
     * "isPriorityTakeover" is used to determine if the caller was a priority takeover or not and
     * log messages accordingly.
     */
    enum StartElectionV1Reason { kElectionTimeout, kPriorityTakeover, kStepUpRequest };

    void _startElectSelfIfEligibleV1(StartElectionV1Reason reason);

    /**
     * Resets the term of last vote to 0 to prevent any node from voting for term 0.
     * Returns the event handle that indicates when last vote write finishes.
     */
    EventHandle _resetElectionInfoOnProtocolVersionUpgrade(const ReplSetConfig& oldConfig,
                                                           const ReplSetConfig& newConfig);

    /**
     * Schedules work and returns handle to callback.
     * If work cannot be scheduled due to shutdown, returns empty handle.
     * All other non-shutdown scheduling failures will abort the process.
     * Does not run 'work' if callback is canceled.
     */
    CallbackHandle _scheduleWork(const CallbackFn& work);

    /**
     * Schedules work to be run no sooner than 'when' and returns handle to callback.
     * If work cannot be scheduled due to shutdown, returns empty handle.
     * All other non-shutdown scheduling failures will abort the process.
     * Does not run 'work' if callback is canceled.
     */
    CallbackHandle _scheduleWorkAt(Date_t when, const CallbackFn& work);

    /**
     * Schedules work and waits for completion.
     */
    void _scheduleWorkAndWaitForCompletion(const CallbackFn& work);

    /**
     * Schedules work to be run no sooner than 'when' and waits for completion.
     */
    void _scheduleWorkAtAndWaitForCompletion(Date_t when, const CallbackFn& work);

    /**
     * Schedules DB work and returns handle to callback.
     * If work cannot be scheduled due to shutdown, returns empty handle.
     * All other non-shutdown scheduling failures will abort the process.
     * Does not run 'work' if callback is canceled.
     */
    CallbackHandle _scheduleDBWork(const CallbackFn& work);

    /**
     * Does the actual work of scheduling the work with the executor.
     * Used by _scheduleWork() and _scheduleWorkAt() only.
     * Do not call this function directly.
     */
    CallbackHandle _wrapAndScheduleWork(ScheduleFn scheduleFn, const CallbackFn& work);

    /**
     * Creates an event.
     * Returns invalid event handle if the executor is shutting down.
     * Otherwise aborts on non-shutdown error.
     */
    EventHandle _makeEvent();

    /**
     * Wrap a function into executor callback.
     * If the callback is cancelled, the given function won't run.
     */
    executor::TaskExecutor::CallbackFn _wrapAsCallbackFn(const stdx::function<void()>& work);

    /**
     * Finish catch-up mode and start drain mode.
     */
    void _enterDrainMode_inlock();

    /**
     * Waits for the config state to leave kConfigStartingUp, which indicates that start() has
     * finished.
     */
    void _waitForStartUpComplete();

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (PS) Pointer is read-only in concurrent operation, item pointed to is self-synchronizing;
    //      Access in any context.
    // (M)  Reads and writes guarded by _mutex
    // (X)  Reads and writes guarded by _topoMutex
    // (MX) Must hold _mutex and _topoMutex to write; must either hold _mutex or _topoMutex
    //      to read.
    // (GX) Readable under a global intent lock.  Must either hold global lock in exclusive
    //      mode (MODE_X) or both hold global lock in shared mode (MODE_S) and hold _topoMutex
    //      to write.
    // (I)  Independently synchronized, see member variable comment.

    // When both _mutex and _topoMutex are needed, the caller must follow the strict locking order
    // to avoid deadlock: _topoMutex must be held before locking _mutex.
    // In other words,  _topoMutex can never be locked while holding _mutex.

    // Protects member data of this ReplicationCoordinator.
    mutable stdx::mutex _mutex;  // (S)

    // Protects member data of the TopologyCoordinator.
    mutable stdx::mutex _topoMutex;  // (S)

    // Handles to actively queued heartbeats.
    HeartbeatHandles _heartbeatHandles;  // (X)

    // When this node does not know itself to be a member of a config, it adds
    // every host that sends it a heartbeat request to this set, and also starts
    // sending heartbeat requests to that host.  This set is cleared whenever
    // a node discovers that it is a member of a config.
    unordered_set<HostAndPort> _seedList;  // (X)

    // Parsed command line arguments related to replication.
    const ReplSettings _settings;  // (R)

    // Mode of replication specified by _settings.
    const Mode _replMode;  // (R)

    // Pointer to the TopologyCoordinator owned by this ReplicationCoordinator.
    std::unique_ptr<TopologyCoordinator> _topCoord;  // (X)

    // If the executer is owned then this will be set, but should not be used.
    // This is only used to clean up and destroy the replExec if owned
    std::unique_ptr<ReplicationExecutor> _replExecutorIfOwned;  // (S)
    // Executor that drives the topology coordinator.
    ReplicationExecutor& _replExecutor;  // (S)

    // Pointer to the ReplicationCoordinatorExternalState owned by this ReplicationCoordinator.
    std::unique_ptr<ReplicationCoordinatorExternalState> _externalState;  // (PS)

    // Our RID, used to identify us to our sync source when sending replication progress
    // updates upstream.  Set once in startReplication() and then never modified again.
    OID _myRID;  // (M)

    // Rollback ID. Used to check if a rollback happened during some interval of time
    // TODO: ideally this should only change on rollbacks NOT on mongod restarts also.
    int _rbid;  // (M)

    // Indicates that we've received a request to stepdown from PRIMARY (likely via a heartbeat)
    // TODO(SERVER-27083): This bool is redundant of the same-named bool in TopologyCoordinatorImpl,
    // but due to mutex ordering between _mutex and _topoMutex we can't inspect the
    // TopologyCoordinator field in awaitReplication() where this bool is used.  Once we get rid
    // of topoMutex and start guarding access to the TopologyCoordinator via _mutex we should
    // consolidate the two bools.
    bool _stepDownPending = false;  // (M)

    // list of information about clients waiting on replication.  Does *not* own the WaiterInfos.
    WaiterList _replicationWaiterList;  // (M)

    // list of information about clients waiting for a particular opTime.
    // Does *not* own the WaiterInfos.
    WaiterList _opTimeWaiterList;  // (M)

    // Set to true when we are in the process of shutting down replication.
    bool _inShutdown;  // (M)

    // Election ID of the last election that resulted in this node becoming primary.
    OID _electionId;  // (M)

    // Vector containing known information about each member (such as replication
    // progress and member ID) in our replica set or each member replicating from
    // us in a master-slave deployment.  In master/slave, the first entry is
    // guaranteed to correspond to ourself.  In replica sets where we don't have a
    // valid config or are in state REMOVED then the vector will be a single element
    // just with info about ourself.  In replica sets with a valid config the elements
    // will be in the same order as the members in the replica set config, thus
    // the entry for ourself will be at _thisMemberConfigIndex.
    SlaveInfoVector _slaveInfo;  // (M)

    // Used to signal threads waiting for changes to _memberState.
    stdx::condition_variable _memberStateChange;  // (M)

    // Current ReplicaSet state.
    MemberState _memberState;  // (MX)

    // Used to signal threads waiting for changes to _memberState.
    stdx::condition_variable _drainFinishedCond;  // (M)

    ReplicationCoordinator::ApplierState _applierState = ApplierState::Running;  // (M)

    // Used to signal threads waiting for changes to _rsConfigState.
    stdx::condition_variable _rsConfigStateChange;  // (M)

    // Represents the configuration state of the coordinator, which controls how and when
    // _rsConfig may change.  See the state transition diagram in the type definition of
    // ConfigState for details.
    ConfigState _rsConfigState;  // (M)

    // The current ReplicaSet configuration object, including the information about tag groups
    // that is used to satisfy write concern requests with named gle modes.
    ReplSetConfig _rsConfig;  // (MX)

    // This member's index position in the current config.
    int _selfIndex;  // (MX)

    // Condition to signal when new heartbeat data comes in.
    stdx::condition_variable _stepDownWaiters;  // (X)

    // State for conducting an election of this node.
    // the presence of a non-null _freshnessChecker pointer indicates that an election is
    // currently in progress. When using the V1 protocol, a non-null _voteRequester pointer
    // indicates this instead.
    // Only one election is allowed at a time.
    std::unique_ptr<FreshnessChecker> _freshnessChecker;  // (X)

    std::unique_ptr<ElectCmdRunner> _electCmdRunner;  // (X)

    std::unique_ptr<VoteRequester> _voteRequester;  // (X)

    // Event that the election code will signal when the in-progress election completes.
    // Unspecified value when _freshnessChecker is NULL.
    ReplicationExecutor::EventHandle _electionFinishedEvent;  // (X)

    // Event that the election code will signal when the in-progress election dry run completes,
    // which includes writing the last vote and scheduling the real election.
    ReplicationExecutor::EventHandle _electionDryRunFinishedEvent;  // (X)

    // Event that the stepdown code will signal when the in-progress stepdown completes.
    ReplicationExecutor::EventHandle _stepDownFinishedEvent;  // (X)

    // Whether we slept last time we attempted an election but possibly tied with other nodes.
    bool _sleptLastElection;  // (X)

    // Flag that indicates whether writes to databases other than "local" are allowed.  Used to
    // answer canAcceptWritesForDatabase() and canAcceptWritesFor() questions.
    // Always true for standalone nodes and masters in master-slave relationships.
    AtomicWord<bool> _canAcceptNonLocalWrites;  // (GX)

    // Flag that indicates whether reads from databases other than "local" are allowed.  Unlike
    // _canAcceptNonLocalWrites, above, this question is about admission control on secondaries,
    // and we do not require that its observers be strongly synchronized.  Accidentally
    // providing the prior value for a limited period of time is acceptable.  Also unlike
    // _canAcceptNonLocalWrites, its value is only meaningful on replica set secondaries.
    AtomicUInt32 _canServeNonLocalReads;  // (S)

    // OpTime of the latest committed operation. Matches the concurrency level of _slaveInfo.
    OpTime _lastCommittedOpTime;  // (M)

    // OpTime representing our transition to PRIMARY and the start of our term.
    // _lastCommittedOpTime cannot be set to an earlier OpTime.
    OpTime _firstOpTimeOfMyTerm;  // (M)

    // Storage interface used by initial syncer.
    StorageInterface* _storage;  // (PS)
    // InitialSyncer used for initial sync.
    std::shared_ptr<InitialSyncer>
        _initialSyncer;  // (I) pointer set under mutex, copied by callers.

    // Hands out the next snapshot name.
    AtomicUInt64 _snapshotNameGenerator;  // (S)

    // The OpTimes and SnapshotNames for all snapshots newer than the current commit point, kept in
    // sorted order. Any time this is changed, you must also update _uncommitedSnapshotsSize.
    std::deque<SnapshotInfo> _uncommittedSnapshots;  // (M)

    // A cache of the size of _uncommittedSnaphots that can be read without any locking.
    // May only be written to while holding _mutex.
    AtomicUInt64 _uncommittedSnapshotsSize;  // (I)

    // The non-null OpTime and SnapshotName of the current snapshot used for committed reads, if
    // there is one.
    // When engaged, this must be <= _lastCommittedOpTime and < _uncommittedSnapshots.front().
    boost::optional<SnapshotInfo> _currentCommittedSnapshot;  // (M)

    // Used to signal threads that are waiting for new committed snapshots.
    stdx::condition_variable _currentCommittedSnapshotCond;  // (M)

    // The cached current term. It's in sync with the term in topology coordinator.
    long long _cachedTerm = OpTime::kUninitializedTerm;  // (M)

    // Callback Handle used to cancel a scheduled LivenessTimeout callback.
    ReplicationExecutor::CallbackHandle _handleLivenessTimeoutCbh;  // (M)

    // Callback Handle used to cancel a scheduled ElectionTimeout callback.
    ReplicationExecutor::CallbackHandle _handleElectionTimeoutCbh;  // (M)

    // Election timeout callback will not run before this time.
    // If this date is Date_t(), the callback is either unscheduled or canceled.
    // Used for testing only.
    Date_t _handleElectionTimeoutWhen;  // (M)

    // Callback Handle used to cancel a scheduled PriorityTakover callback.
    ReplicationExecutor::CallbackHandle _priorityTakeoverCbh;  // (M)

    // Priority takeover callback will not run before this time.
    // If this date is Date_t(), the callback is either unscheduled or canceled.
    // Used for testing only.
    Date_t _priorityTakeoverWhen;  // (M)

    // Callback handle used by _waitForStartUpComplete() to block until configuration
    // is loaded and external state threads have been started (unless this node is an arbiter).
    CallbackHandle _finishLoadLocalConfigCbh;  // (M)

    // The id of the earliest member, for which the handleLivenessTimeout callback has been
    // scheduled.  We need this so that we don't needlessly cancel and reschedule the callback on
    // every liveness update.
    int _earliestMemberId = -1;  // (M)

    // Cached copy of the current config protocol version.
    AtomicInt64 _protVersion;  // (S)

    // Lambda indicating durability of storageEngine.
    stdx::function<bool()> _isDurableStorageEngine;  // (R)

    // This setting affects the Applier prefetcher behavior.
    mutable stdx::mutex _indexPrefetchMutex;
    ReplSettings::IndexPrefetchConfig _indexPrefetchConfig =
        ReplSettings::IndexPrefetchConfig::PREFETCH_ALL;  // (I)

    // The catchup state including all catchup logic. The presence of a non-null pointer indicates
    // that the node is currently in catchup mode.
    std::unique_ptr<CatchupState> _catchupState;  // (X)
};

}  // namespace repl
}  // namespace mongo
