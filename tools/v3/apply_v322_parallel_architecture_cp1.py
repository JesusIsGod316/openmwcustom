import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    if chr(0) in new:
        raise RuntimeError(f"{rel}: V3.22 parallel CP1 replacement contains an embedded NUL")
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.22 parallel CP1 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.22 parallel CP1 patched {rel} ({count} match(es))")


replace_exact(
    "apps/openmw/mwmechanics/actors.cpp",
    '''#include <array>
#include <optional>''',
    '''#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>''',
)

replace_exact(
    "apps/openmw/mwmechanics/actors.cpp",
    '''#include <components/debug/debuglog.hpp>
#include <components/misc/mathutil.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/misc/mathutil.hpp>''',
)

replace_exact(
    "apps/openmw/mwmechanics/actors.cpp",
    '''#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/values.hpp>''',
    '''#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/settings/values.hpp>''',
)

replace_exact(
    "apps/openmw/mwmechanics/actors.cpp",
    '''#include "../mwrender/vismask.hpp"''',
    '''#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/vismask.hpp"''',
)

helper_anchor = '''    // Check for command effects having ended and remove package if necessary
    void adjustCommandedActor(const MWWorld::Ptr& actor)'''
helper_code = r'''    bool v322ParallelAvoidanceEnabled()
    {
        const char* value = std::getenv("OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE");
        return value && value[0] == '1' && value[1] == '\0';
    }

    struct V322AvoidanceSnapshot
    {
        MWWorld::Ptr mPtr;
        const MWWorld::LiveCellRefBase* mKey = nullptr;
        osg::Vec3f mPosition;
        osg::Vec3f mMovement;
        osg::Vec3f mHalfExtents;
        float mRotationZ = 0.f;
        float mMaxSpeed = 0.f;
        bool mDead = false;
    };

    struct V322AvoidanceBase
    {
        MWMechanics::Movement* mMovement = nullptr;
        const MWWorld::LiveCellRefBase* mCurrentTarget = nullptr;
        osg::Vec2f mOriginalMovement;
        osg::Vec2f mBaseSpeed;
        float mMaximumDistance = 0.f;
        float mTimeToCheck = 0.f;
        bool mEnabled = false;
        bool mMoving = false;
        bool mTurnToApproachingActor = false;
    };

    struct V322AvoidanceCandidate
    {
        std::size_t mOther = 0;
        float mTime = 0.f;
        float mAngle = 0.f;
        float mDistance = 0.f;
        float mCollisionDistance = 0.f;
        osg::Vec2f mPositionAtTime;
        osg::Vec2f mRelativeSpeed;
    };

    using V322AvoidanceCandidates = std::vector<std::vector<V322AvoidanceCandidate>>;

    void calculateV322AvoidanceCandidates(std::size_t begin, std::size_t end,
        const std::vector<V322AvoidanceSnapshot>& snapshots, const std::vector<V322AvoidanceBase>& bases,
        V322AvoidanceCandidates& output)
    {
        constexpr float minGap = 10.f;
        for (std::size_t index = begin; index < end; ++index)
        {
            const V322AvoidanceBase& base = bases[index];
            if (!base.mEnabled)
                continue;

            const V322AvoidanceSnapshot& snapshot = snapshots[index];
            std::vector<V322AvoidanceCandidate>& candidates = output[index];
            candidates.clear();
            candidates.reserve(snapshots.size() / 4);

            for (std::size_t otherIndex = 0; otherIndex < snapshots.size(); ++otherIndex)
            {
                const V322AvoidanceSnapshot& other = snapshots[otherIndex];
                if (other.mKey == snapshot.mKey || other.mKey == base.mCurrentTarget)
                    continue;

                const osg::Vec3f deltaPosition = other.mPosition - snapshot.mPosition;
                const osg::Vec2f relativePosition
                    = Misc::rotateVec2f(osg::Vec2f(deltaPosition.x(), deltaPosition.y()), snapshot.mRotationZ);
                const float distance = deltaPosition.length();
                if (distance > base.mMaximumDistance || relativePosition.y() < 0)
                    continue;
                if (deltaPosition.z() > snapshot.mHalfExtents.z() * 2
                    || deltaPosition.z() < -other.mHalfExtents.z() * 2)
                    continue;

                const osg::Vec3f otherSpeed = other.mMovement * other.mMaxSpeed;
                const osg::Vec2f relativeSpeed
                    = Misc::rotateVec2f(osg::Vec2f(otherSpeed.x(), otherSpeed.y()),
                          snapshot.mRotationZ - other.mRotationZ)
                    - base.mBaseSpeed;

                float collisionDistance = minGap + snapshot.mHalfExtents.x() + other.mHalfExtents.x();
                collisionDistance = std::min(collisionDistance, relativePosition.length());
                const float velocityProjection
                    = relativePosition.x() * relativeSpeed.x() + relativePosition.y() * relativeSpeed.y();
                const float velocitySquared = relativeSpeed.length2();
                const float discriminant = velocityProjection * velocityProjection
                    - velocitySquared
                        * (relativePosition.length2() - collisionDistance * collisionDistance);
                if (discriminant <= 0 || velocitySquared == 0)
                    continue;
                const float time = (-velocityProjection - std::sqrt(discriminant)) / velocitySquared;
                if (time < 0 || time > base.mTimeToCheck)
                    continue;

                candidates.push_back({ otherIndex, time, std::atan2(deltaPosition.x(), deltaPosition.y()), distance,
                    collisionDistance, relativePosition + relativeSpeed * time, relativeSpeed });
            }
        }
    }

    class V322AvoidanceWorkItem final : public SceneUtil::WorkItem
    {
    public:
        V322AvoidanceWorkItem(std::size_t begin, std::size_t end,
            const std::vector<V322AvoidanceSnapshot>& snapshots, const std::vector<V322AvoidanceBase>& bases,
            V322AvoidanceCandidates& output)
            : mBegin(begin)
            , mEnd(end)
            , mSnapshots(snapshots)
            , mBases(bases)
            , mOutput(output)
        {
        }

        void doWork() override
        {
            calculateV322AvoidanceCandidates(mBegin, mEnd, mSnapshots, mBases, mOutput);
        }

    private:
        std::size_t mBegin;
        std::size_t mEnd;
        const std::vector<V322AvoidanceSnapshot>& mSnapshots;
        const std::vector<V322AvoidanceBase>& mBases;
        V322AvoidanceCandidates& mOutput;
    };

    // Check for command effects having ended and remove package if necessary
    void adjustCommandedActor(const MWWorld::Ptr& actor)'''
replace_exact("apps/openmw/mwmechanics/actors.cpp", helper_anchor, helper_code)

function_anchor = '''    void Actors::predictAndAvoidCollisions(float duration) const
    {
        if (!MWBase::Environment::get().getMechanicsManager()->isAIActive())
            return;

        const float minGap = 10.f;'''
parallel_prefix = r'''    void Actors::predictAndAvoidCollisions(float duration) const
    {
        if (!MWBase::Environment::get().getMechanicsManager()->isAIActive())
            return;

        if (v322ParallelAvoidanceEnabled())
        {
            constexpr float maxDistanceForPartialAvoiding = 200.f;
            constexpr float maxDistanceForStrictAvoiding = 100.f;
            constexpr float maxTimeToCheck = 2.f;
            constexpr std::size_t minimumParallelActors = 12;

            const bool giveWayWhenIdle = Settings::game().mNPCsGiveWay;
            const MWWorld::Ptr player = getPlayer();
            MWBase::World* const world = MWBase::Environment::get().getWorld();
            SceneUtil::WorkQueue* const workQueue = world->getRenderingManager()->getWorkQueue();
            const std::size_t backgroundLanes
                = static_cast<std::size_t>(std::max(0, static_cast<int>(Settings::cells().mPreloadNumThreads)));

            std::vector<V322AvoidanceSnapshot> snapshots;
            snapshots.reserve(mActors.size());
            for (const Actor& actor : mActors)
            {
                if (actor.isInvalid())
                    continue;
                const MWWorld::Ptr& ptr = actor.getPtr();
                const MWWorld::Class& cls = ptr.getClass();
                const Movement& movement = cls.getMovementSettings(ptr);
                snapshots.push_back({ ptr, ptr.mRef, ptr.getRefData().getPosition().asVec3(), movement.asVec3(),
                    world->getHalfExtents(ptr), ptr.getRefData().getPosition().rot[2], cls.getMaxSpeed(ptr),
                    cls.getCreatureStats(ptr).isDead() });
            }

            if (workQueue && backgroundLanes > 0 && snapshots.size() >= minimumParallelActors)
            {
                std::vector<V322AvoidanceBase> bases(snapshots.size());
                for (std::size_t index = 0; index < snapshots.size(); ++index)
                {
                    const MWWorld::Ptr& ptr = snapshots[index].mPtr;
                    if (ptr == player || snapshots[index].mMaxSpeed == 0.f)
                        continue;

                    Movement& movement = ptr.getClass().getMovementSettings(ptr);
                    const osg::Vec2f originalMovement(movement.mPosition[0], movement.mPosition[1]);
                    const bool moving = originalMovement.length2() > 0.01f;
                    if (movement.mPosition[1] < 0)
                        continue;

                    bool shouldAvoidCollision = moving;
                    bool shouldGiveWay = false;
                    bool turnToApproachingActor = !moving;
                    const MWWorld::LiveCellRefBase* currentTarget = nullptr;
                    const auto& aiSequence = ptr.getClass().getCreatureStats(ptr).getAiSequence();
                    if (!aiSequence.isEmpty())
                    {
                        const auto& package = aiSequence.getActivePackage();
                        if (package.getTypeId() == AiPackageTypeId::Follow)
                            shouldAvoidCollision = true;
                        else if (package.getTypeId() == AiPackageTypeId::Wander && giveWayWhenIdle)
                        {
                            if (!static_cast<const AiWander&>(package).isStationary())
                                shouldGiveWay = true;
                        }
                        else if (package.getTypeId() == AiPackageTypeId::Combat
                            || package.getTypeId() == AiPackageTypeId::Pursue)
                        {
                            currentTarget = package.getTarget().mRef;
                            shouldAvoidCollision = moving;
                            turnToApproachingActor = false;
                        }
                    }
                    if (!shouldAvoidCollision && !shouldGiveWay)
                        continue;

                    float timeToCheck = maxTimeToCheck;
                    if (!shouldGiveWay && !aiSequence.isEmpty())
                    {
                        timeToCheck = std::min(timeToCheck,
                            getTimeToDestination(**aiSequence.begin(), snapshots[index].mPosition,
                                snapshots[index].mMaxSpeed, duration, snapshots[index].mHalfExtents));
                    }
                    bases[index] = { &movement, currentTarget, originalMovement,
                        originalMovement * snapshots[index].mMaxSpeed,
                        moving ? maxDistanceForPartialAvoiding : maxDistanceForStrictAvoiding, timeToCheck, true,
                        moving, turnToApproachingActor };
                }

                const std::size_t lanes = std::min(snapshots.size(), backgroundLanes + 1);
                const std::size_t chunkSize = (snapshots.size() + lanes - 1) / lanes;
                V322AvoidanceCandidates candidates(snapshots.size());
                std::vector<osg::ref_ptr<V322AvoidanceWorkItem>> jobs;
                jobs.reserve(lanes - 1);
                for (std::size_t lane = 0; lane + 1 < lanes; ++lane)
                {
                    const std::size_t begin = lane * chunkSize;
                    const std::size_t end = std::min(begin + chunkSize, snapshots.size());
                    if (begin >= end)
                        break;
                    osg::ref_ptr<V322AvoidanceWorkItem> job
                        = new V322AvoidanceWorkItem(begin, end, snapshots, bases, candidates);
                    workQueue->addWorkItem(job, true);
                    jobs.push_back(std::move(job));
                }

                const std::size_t mainBegin = std::min(jobs.size() * chunkSize, snapshots.size());
                calculateV322AvoidanceCandidates(mainBegin, snapshots.size(), snapshots, bases, candidates);
                for (const osg::ref_ptr<V322AvoidanceWorkItem>& job : jobs)
                    job->waitTillDone();

                std::ostringstream detail;
                detail << "actors=" << snapshots.size() << ";lanes=" << lanes;
                Debug::V3Diagnostics::ScopedCsvTimer timer(
                    Debug::V3Diagnostics::renderWriter(), "actor_avoidance_commit", detail.str());

                for (std::size_t index = 0; index < snapshots.size(); ++index)
                {
                    const V322AvoidanceBase& base = bases[index];
                    if (!base.mEnabled)
                        continue;
                    float timeToCollision = base.mTimeToCheck;
                    osg::Vec2f movementCorrection(0.f, 0.f);
                    float angleToApproachingActor = 0.f;
                    for (const V322AvoidanceCandidate& candidate : candidates[index])
                    {
                        if (candidate.mTime > timeToCollision)
                            continue;
                        const MWWorld::Ptr& other = snapshots[candidate.mOther].mPtr;
                        const MWWorld::Ptr& ptr = snapshots[index].mPtr;
                        if (!world->getLOS(other, ptr))
                            continue;
                        if (!MWBase::Environment::get().getMechanicsManager()->awarenessCheck(other, ptr))
                            continue;

                        timeToCollision = candidate.mTime;
                        angleToApproachingActor = candidate.mAngle;
                        const float coefficient
                            = (candidate.mPositionAtTime.x() * candidate.mRelativeSpeed.x()
                                  + candidate.mPositionAtTime.y() * candidate.mRelativeSpeed.y())
                            / (candidate.mCollisionDistance * candidate.mCollisionDistance
                                * snapshots[index].mMaxSpeed)
                            * std::clamp((maxDistanceForPartialAvoiding - candidate.mDistance)
                                    / (maxDistanceForPartialAvoiding - maxDistanceForStrictAvoiding),
                                0.f, 1.f);
                        movementCorrection = candidate.mPositionAtTime * coefficient;
                        if (snapshots[candidate.mOther].mDead)
                            movementCorrection.y() *= 0.5f;
                    }

                    if (timeToCollision < base.mTimeToCheck)
                    {
                        osg::Vec2f newMovement = base.mOriginalMovement + movementCorrection;
                        newMovement.y() = std::max(newMovement.y(), 0.f);
                        newMovement.normalize();
                        if (base.mMoving)
                            newMovement *= base.mOriginalMovement.length();
                        base.mMovement->mPosition[0] = newMovement.x();
                        base.mMovement->mPosition[1] = newMovement.y();
                        if (base.mTurnToApproachingActor)
                            zTurn(snapshots[index].mPtr, angleToApproachingActor);
                    }
                }
                return;
            }
        }

        const float minGap = 10.f;'''
replace_exact("apps/openmw/mwmechanics/actors.cpp", function_anchor, parallel_prefix)

replace_exact(
    "apps/openmw/engine.cpp",
    '''openmw-custom-v3.22-cp1-msoc-hotpath / openmw-custom-v3.22-cp2-occluder-efficiency''',
    '''openmw-custom-v3.22-cp1-msoc-hotpath / openmw-custom-v3.22-cp2-occluder-efficiency / openmw-custom-v3.22-parallel-architecture-cp1''',
)

launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")

init_anchor = "$V322CP2OccluderMode = '0'\n$V320EngineLuaFastPaths = '0'"
if launcher.count(init_anchor) != 1:
    raise RuntimeError("V3.22 parallel CP1 launcher initialization anchor drifted")
launcher = launcher.replace(
    init_anchor,
    "$V322CP2OccluderMode = '0'\n$V322ParallelActorAvoidance = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)

menu140 = "Write-Host '140 = V3.22 CP2 aggressive 300-radius + redundant-raster suppression'"
if launcher.count(menu140) != 1:
    raise RuntimeError("V3.22 parallel CP1 launcher menu anchor drifted")
launcher = launcher.replace(
    menu140,
    menu140 + "\nWrite-Host '141 = V3.22 parallel immutable actor-avoidance prediction'",
    1,
)

choice_line = next((line for line in launcher.splitlines() if "135-140" in line and "Read-Host" in line), None)
if not choice_line or ",'139','140'))" not in choice_line:
    raise RuntimeError("V3.22 parallel CP1 launcher choice anchor drifted")
new_choice = choice_line.replace("135-140", "135-141", 1).replace(
    ",'139','140'))", ",'139','140','141'))", 1
)
launcher = launcher.replace(choice_line, new_choice, 1)

line135 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'135'"))
mode135_body = line135[line135.index("{") + 1 : line135.rindex("}")].strip()
if "$V322CP1MsocHotPath = '1'" in mode135_body or "$V322CP2OccluderMode" in mode135_body:
    raise RuntimeError("V3.22 parallel CP1 control body is contaminated")
mode141_body = mode135_body.replace("v322-cp1-v321-control", "v322-parallel-actor-avoidance", 1)
if mode141_body == mode135_body:
    raise RuntimeError("V3.22 parallel CP1 could not derive Mode141 from Mode135")
mode141 = f"        '141' {{ {mode141_body}; $V322ParallelActorAvoidance = '1' }}"
line140 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'140'"))
launcher = launcher.replace(line140 + "\n", line140 + "\n" + mode141 + "\n", 1)

manifest_anchor = '    "v322_cp2_occluder_efficiency_mode=$V322CP2OccluderMode",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.22 parallel CP1 launcher manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v322_parallel_actor_avoidance=$V322ParallelActorAvoidance",',
    1,
)

env_anchor = "    $env:OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE = $V322CP2OccluderMode"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.22 parallel CP1 launcher environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE = $V322ParallelActorAvoidance",
    1,
)

cleanup_anchor = "    Remove-Item Env:OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.22 parallel CP1 launcher cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''


V3.22 parallel architecture CP1 — immutable actor-avoidance prediction
=======================================================================

Mode 135 remains the exact final V3.21 control. Mode 141 changes only actor
collision-avoidance prediction. It snapshots positions, desired movement,
extents, speeds, target identity, and dead/alive state on the main thread;
worker jobs perform only pairwise numeric prediction; LOS, awareness checks,
steering writes, and turns remain on the main thread and commit in actor order.

The worker phase uses the existing bounded engine WorkQueue and its configured
preload thread count, plus the main thread as one lane. It activates only with
at least 12 live actors and at least one background lane. Jobs are inserted at
the front and joined before commit; there is no cross-frame state and no OSG,
physics-world, Lua, inventory, AI-sequence, or gameplay-event mutation on a
worker. If any activation precondition is absent, the exact legacy serial path
runs.

Mode 141 intentionally uses one immutable movement snapshot for the full
prediction batch. This replaces the legacy within-loop steering feedback with
a deterministic frame-consistent input set. It is therefore experimental and
must pass pathing/traffic correctness checks as well as frame-time gates before
promotion. Modes 136-140 remain dormant and Mode141 does not enable them.
'''
readme_path.write_text(readme, encoding="utf-8", newline="\n")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary", "--", ":!V3-applied-source.patch", ":!V3-applied-source-stat.txt"],
    cwd=ROOT,
    check=True,
    stdout=(ROOT / "V3-applied-source.patch").open("w", encoding="utf-8", newline="\n"),
)
subprocess.run(
    ["git", "diff", "--stat", "--", ":!V3-applied-source.patch", ":!V3-applied-source-stat.txt"],
    cwd=ROOT,
    check=True,
    stdout=(ROOT / "V3-applied-source-stat.txt").open("w", encoding="utf-8", newline="\n"),
)
print("V3.22 parallel architecture CP1 immutable actor-avoidance layer applied")
