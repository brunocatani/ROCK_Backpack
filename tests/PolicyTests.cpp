#include "AnchorSelectionPolicy.h"
#include "BackpackConfig.h"
#include "BackpackRetrievalPolicy.h"
#include "InventoryPolicy.h"
#include "ui/ItemIconPolicy.h"
#include "ui/PointerPressGate.h"
#include "PanelPoseMath.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    void require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    }

    bool near(const float lhs, const float rhs, const float epsilon = 0.0005f)
    {
        return std::fabs(lhs - rhs) <= epsilon;
    }
}

namespace retrieval = rock_backpack::retrieval_policy;

int main()
{
    require(
        rock_backpack::kMinimumPanelPhysicalDimensionGameUnits > 0.0f &&
            rock_backpack::kMaximumPanelPhysicalDimensionGameUnits >
                rock_backpack::kDefaultPanelPhysicalWidthGameUnits,
        "live panel dimensions must remain bounded around the default");

    using namespace rock_backpack::inventory_policy;
    require(
        categoryKey(Category::Weapons) == "weapons" &&
            categoryKey(Category::Armor) == "armor" &&
            categoryKey(Category::Aid) == "aid" &&
            categoryKey(Category::Junk) == "junk" &&
            categoryKey(Category::Ammo) == "ammo",
        "the native inventory model must preserve Weapons beside the four expanded categories");
    require(
        editorIndexMask(-1) == 0 &&
            editorIndexMask(32) == 0 &&
            editorIndexMask(30) == 0x40000000u,
        "editor slot masks must reject invalid indices and preserve the Pip-Boy-range bit");
    require(
        armorZoneFlags(editorIndexMask(0)) == ArmorZone_Head,
        "head editor slots must map to the head presentation zone");
    require(
        armorZoneFlags(editorIndexMask(3)) == ArmorZone_Torso,
        "body editor slots must map to the torso presentation zone");
    require(
        armorZoneFlags(editorIndexMask(7) | editorIndexMask(13)) ==
            (ArmorZone_LeftArm | ArmorZone_RightArm),
        "multi-slot armor must preserve every affected presentation zone");
    require(
        (armorZoneFlags(editorIndexMask(28)) & ArmorZone_Utility) != 0 &&
            armorZoneFlags(0) == ArmorZone_Utility,
        "custom and unassigned armor slots must remain visible through the utility fallback");

    require(
        armorBayKey(ArmorBay::Head) == "head" &&
            armorBayKey(ArmorBay::Torso) == "torso" &&
            armorBayKey(ArmorBay::LeftArm) == "leftArm" &&
            armorBayKey(ArmorBay::RightArm) == "rightArm" &&
            armorBayKey(ArmorBay::Legs) == "legs" &&
            armorBayKey(ArmorBay::Accessory) == "accessory" &&
            armorBayKey(ArmorBay::Utility) == "utility",
        "every paperdoll bay must expose the stable key the view lays out against");
    require(
        primaryArmorBay(armorZoneFlags(editorIndexMask(0))) ==
                ArmorBay::Head &&
            primaryArmorBay(armorZoneFlags(editorIndexMask(3))) ==
                ArmorBay::Torso &&
            primaryArmorBay(armorZoneFlags(editorIndexMask(4))) ==
                ArmorBay::LeftArm &&
            primaryArmorBay(armorZoneFlags(editorIndexMask(5))) ==
                ArmorBay::RightArm &&
            primaryArmorBay(armorZoneFlags(editorIndexMask(9))) ==
                ArmorBay::Legs,
        "single-zone armor must file under its own bay");
    require(
        primaryArmorBay(
            armorZoneFlags(
                editorIndexMask(3) | editorIndexMask(7) |
                editorIndexMask(9))) == ArmorBay::Torso,
        "a garment covering torso, arm and leg zones must file as body armor rather than splitting across bays");
    require(
        primaryArmorBay(armorZoneFlags(0)) == ArmorBay::Utility &&
            primaryArmorBay(ArmorZone_None) == ArmorBay::Utility,
        "armor with no recognised zone must still reach a bay instead of disappearing from the paperdoll");
    require(
        armorZoneCount(armorZoneFlags(editorIndexMask(0))) == 1 &&
            armorZoneCount(
                armorZoneFlags(
                    editorIndexMask(7) | editorIndexMask(13))) == 2 &&
            armorZoneCount(ArmorZone_None) == 0,
        "zone counting must report the real coverage the detail panel displays");

    using rock_backpack::anchor_policy::Candidate;
    using rock_backpack::anchor_policy::selectStickyNearest;

    constexpr float maximumSquared = 700.0f * 700.0f;
    const std::array candidates{
        Candidate{ 30, 400.0f * 400.0f, true },
        Candidate{ 20, 200.0f * 200.0f, true },
        Candidate{ 10, 100.0f * 100.0f, false },
    };

    auto selection = selectStickyNearest(candidates, 0, maximumSquared);
    require(selection.formId == 20, "nearest eligible anchor should be selected");

    selection = selectStickyNearest(candidates, 30, maximumSquared);
    require(selection.formId == 30, "active anchor should remain sticky inside the visibility radius");

    const std::array leaving{
        Candidate{ 30, 701.0f * 701.0f, true },
        Candidate{ 20, 300.0f * 300.0f, true },
    };
    selection = selectStickyNearest(leaving, 30, maximumSquared);
    require(selection.formId == 20, "selection should move after active anchor exits the visibility radius");

    const float justOutsideDistance = std::nextafter(700.0f, std::numeric_limits<float>::infinity());
    const std::array exactBoundary{ Candidate{ 7, maximumSquared, true } };
    const std::array justOutsideBoundary{
        Candidate{ 7, justOutsideDistance * justOutsideDistance, true },
    };
    require(selectStickyNearest(exactBoundary, 0, maximumSquared).formId == 7,
        "the exact 10 metre boundary must remain visible");
    require(!selectStickyNearest(justOutsideBoundary, 0, maximumSquared),
        "the first representable distance beyond 10 metres must be hidden");

    const std::array tied{
        Candidate{ 42, 100.0f, true },
        Candidate{ 41, 100.0f, true },
    };
    selection = selectStickyNearest(tied, 0, maximumSquared);
    require(selection.formId == 41, "equal distances should resolve deterministically by form ID");

    const std::array invalid{
        Candidate{ 1, std::numeric_limits<float>::quiet_NaN(), true },
        Candidate{ 2, -1.0f, true },
        Candidate{ 3, 701.0f * 701.0f, true },
    };
    require(!selectStickyNearest(invalid, 0, maximumSquared),
        "invalid and out-of-range candidates must fail closed");

    using namespace rock_backpack::panel_math;
    StoredTransform identity{};
    identity.rotation = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    identity.scale = 1.0f;
    const auto pose = computeBackpackPanelPose(identity);
    require(pose.has_value(), "identity backpack transform should produce a panel pose");
    require(near(pose->position.x, -0.1214f, 0.001f) && near(pose->position.y, 0.0202f, 0.001f) &&
            near(pose->position.z, 7.4861f, 0.001f),
        "panel should sit just beyond the mesh-derived outer surface");
    float quaternionNormSquared = 0.0f;
    for (const float component : pose->orientation) {
        quaternionNormSquared += component * component;
    }
    require(near(quaternionNormSquared, 1.0f), "mesh-derived panel quaternion should be normalized");
    require(near(dot(pose->right, pose->up), 0.0f) &&
            near(dot(pose->right, pose->front), 0.0f) &&
            near(dot(pose->up, pose->front), 0.0f),
        "pointer intersection axes must remain orthogonal");
    require(near(dot(pose->right, pose->right), 1.0f) &&
            near(dot(pose->up, pose->up), 1.0f) &&
            near(dot(pose->front, pose->front), 1.0f),
        "pointer intersection axes must remain normalized");

    auto quarterTurn = identity;
    quarterTurn.rotation = { 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    const auto rotatedAxis = rotateLocalVectorToWorld(quarterTurn, { 1.0f, 0.0f, 0.0f });
    require(near(rotatedAxis.x, 0.0f) && near(rotatedAxis.y, -1.0f) && near(rotatedAxis.z, 0.0f),
        "backpack axes must use ROCK Prober's proven row-basis Prisma convention");
    const auto rotatedPose = computeBackpackPanelPose(quarterTurn);
    require(rotatedPose.has_value(), "a rotated backpack transform should produce a panel pose");
    require(near(rotatedPose->position.x, 0.0202f, 0.001f) &&
            near(rotatedPose->position.y, 0.1214f, 0.001f) &&
            near(rotatedPose->position.z, 7.4861f, 0.001f),
        "the panel center must follow the same backpack basis as its orientation");

    const Vec3 quaternionVector{
        rotatedPose->orientation[0], rotatedPose->orientation[1], rotatedPose->orientation[2]
    };
    const Vec3 panelLocalFront{ 0.0f, 0.0f, 1.0f };
    const Vec3 twiceCross = cross(quaternionVector, panelLocalFront) * 2.0f;
    const Vec3 quaternionFront = panelLocalFront +
        (twiceCross * rotatedPose->orientation[3]) + cross(quaternionVector, twiceCross);
    const auto expectedFront = normalize(rotateLocalVectorToWorld(
        quarterTurn, { -0.00005f, -0.16605f, 0.98612f }));
    require(expectedFront.has_value() &&
            near(quaternionFront.x, expectedFront->x, 0.001f) &&
            near(quaternionFront.y, expectedFront->y, 0.001f) &&
            near(quaternionFront.z, expectedFront->z, 0.001f),
        "the X,Y,Z,W quaternion must face out from the rotated backpack surface");

    const Vec3 localOffset{ 2.0f, -3.0f, 4.0f };
    const auto offsetPose = computeBackpackPanelPose(quarterTurn, localOffset);
    require(offsetPose.has_value(), "finite object-local offsets should produce a panel pose");
    const auto expectedOffset = rotateLocalVectorToWorld(quarterTurn, localOffset);
    require(near(offsetPose->position.x - rotatedPose->position.x, expectedOffset.x, 0.001f) &&
            near(offsetPose->position.y - rotatedPose->position.y, expectedOffset.y, 0.001f) &&
            near(offsetPose->position.z - rotatedPose->position.z, expectedOffset.z, 0.001f),
        "Configurator XYZ offsets must rotate rigidly with the backpack");
    require(offsetPose->orientation == rotatedPose->orientation,
        "moving the panel must not decouple its object-locked orientation");

    auto tilted = identity;
    tilted.rotation = {
        0.353553f, -0.612372f, 0.707107f,
        0.926777f, 0.126826f, -0.353553f,
        0.126826f, 0.780330f, 0.612372f,
    };
    const auto tiltedPose = computeBackpackPanelPose(tilted);
    require(tiltedPose.has_value(), "combined pitch/yaw/roll should produce a panel pose");
    const Vec3 tiltedQuaternionVector{
        tiltedPose->orientation[0], tiltedPose->orientation[1], tiltedPose->orientation[2]
    };
    const Vec3 tiltedTwiceCross = cross(tiltedQuaternionVector, panelLocalFront) * 2.0f;
    const Vec3 tiltedQuaternionFront = panelLocalFront +
        (tiltedTwiceCross * tiltedPose->orientation[3]) + cross(tiltedQuaternionVector, tiltedTwiceCross);
    const auto expectedTiltedFront = normalize(rotateLocalVectorToWorld(
        tilted, { -0.00005f, -0.16605f, 0.98612f }));
    require(expectedTiltedFront.has_value() &&
            near(tiltedQuaternionFront.x, expectedTiltedFront->x, 0.001f) &&
            near(tiltedQuaternionFront.y, expectedTiltedFront->y, 0.001f) &&
            near(tiltedQuaternionFront.z, expectedTiltedFront->z, 0.001f),
        "panel orientation must remain locked through combined object rotations");

    auto malformed = identity;
    malformed.rotation[0] = std::numeric_limits<float>::infinity();
    require(!computeBackpackPanelPose(malformed),
        "non-finite transforms must fail closed");
    malformed = identity;
    malformed.scale = 0.0f;
    require(!computeBackpackPanelPose(malformed), "degenerate scale must fail closed");

    const retrieval::Vec3 bodyPosition{ 0.0f, 0.0f, 100.0f };
    const retrieval::Vec3 bodyForward{ 0.0f, 1.0f, 0.0f };
    const retrieval::Vec3 leftShoulderCenter{
        -14.0f, -18.0f, 100.0f
    };
    const retrieval::Vec3 outsideBackZone{
        -14.0f, -36.0f, 100.0f
    };
    const auto insideZone = retrieval::sampleBackZone(
        leftShoulderCenter,
        bodyPosition,
        bodyForward);
    require(insideZone.valid && insideZone.withinEnter &&
            insideZone.withinExit,
        "the left-shoulder backpack center must be inside both gates");
    const auto outsideZone = retrieval::sampleBackZone(
        outsideBackZone,
        bodyPosition,
        bodyForward);
    require(outsideZone.valid && !outsideZone.withinEnter &&
            !outsideZone.withinExit,
        "a hand beyond the exit radius must be outside the pull gate");
    const auto inFrontZone = retrieval::sampleBackZone(
        { -14.0f, 18.0f, 100.0f },
        bodyPosition,
        bodyForward);
    require(inFrontZone.valid && !inFrontZone.withinEnter &&
            !inFrontZone.withinExit,
        "the left-shoulder zone must fail the explicit behind-body gate");

    retrieval::State retrievalState{};
    require(retrieval::advance(
                retrievalState,
                { true, true, false, outsideZone }) ==
            retrieval::Outcome::None &&
            retrievalState.phase == retrieval::Phase::ReadyOutside,
        "retrieval must begin with a released grip outside the exit gate");
    require(retrieval::advance(
                retrievalState,
                { true, true, false, insideZone }) ==
            retrieval::Outcome::None &&
            retrievalState.phase == retrieval::Phase::InsideNeutral,
        "a neutral reach into the back zone must arm only the entry phase");
    require(retrieval::prompt(retrievalState,true,true,insideZone)==retrieval::Prompt::Grip,"left-shoulder backpack UI must invite a fresh grip after neutral entry");
    require(retrieval::advance(
                retrievalState,
                { true, true, true, insideZone }) ==
            retrieval::Outcome::None &&
            retrievalState.phase == retrieval::Phase::Armed,
        "grip must be pressed while the left hand is inside the back zone");
    require(retrieval::prompt(retrievalState,true,true,insideZone)==retrieval::Prompt::Pull,"armed backpack UI must instruct the pull motion");
    require(retrieval::prompt(retrievalState,false,true,insideZone)==retrieval::Prompt::Hidden &&
        retrieval::prompt(retrievalState,true,false,insideZone)==retrieval::Prompt::Hidden,"occupied hand or missing input must hide the pull prompt");
    require(retrieval::reservesHand(retrievalState,{true,true,true,outsideZone}),
        "an armed pull must retain input ownership on the exit frame even when it touches another holster");
    require(!retrieval::reservesHand(retrievalState,{true,true,false,outsideZone}),
        "releasing outside must relinquish the armed pull immediately");
    require(!retrieval::reservesHand(retrievalState,{false,true,true,insideZone}) &&
        !retrieval::reservesHand(retrievalState,{true,false,true,insideZone}),
        "lost eligibility or tracking must never retain a body-zone input reservation");
    require(retrieval::advance(
                retrievalState,
                { true, true, true, outsideZone }) ==
            retrieval::Outcome::PullCommitted &&
            retrievalState.phase ==
                retrieval::Phase::AwaitReleaseOutside,
        "leaving the hysteresis gate while gripping must commit one pull");
    require(retrieval::advance(
                retrievalState,
                { true, true, true, outsideZone }) ==
            retrieval::Outcome::None,
        "a held grip must not retrigger after a committed pull");
    require(retrieval::advance(
                retrievalState,
                { true, true, false, outsideZone }) ==
            retrieval::Outcome::None &&
            retrievalState.phase == retrieval::Phase::ReadyOutside,
        "release outside must rearm the gesture for a later inventory item");

    retrieval::reset(retrievalState);
    (void)retrieval::advance(
        retrievalState,
        { true, true, true, outsideZone });
    (void)retrieval::advance(
        retrievalState,
        { true, true, true, insideZone });
    require(retrievalState.phase ==
                retrieval::Phase::NeedOutsideNeutral,
        "a grip already held before entry must never arm retrieval");
    require(retrieval::prompt(retrievalState,true,true,insideZone)==retrieval::Prompt::Rearm,"held-entry UI must explain neutral re-entry");
    require(retrieval::reservesHand(retrievalState,{true,true,false,insideZone}) &&
        !retrieval::reservesHand(retrievalState,{true,true,false,outsideZone}),
        "neutral left-shoulder entry must reserve grab before it can select a weapon, but never away from the zone");
    (void)retrieval::advance(
        retrievalState,
        { false, true, false, outsideZone });
    require(retrievalState.phase ==
                retrieval::Phase::NeedOutsideNeutral,
        "loss of left-hand eligibility must reset all gesture history");


    retrieval::State entryPress{};
    (void)retrieval::advance(entryPress,{true,true,false,outsideZone});
    (void)retrieval::advance(entryPress,{true,true,true,insideZone});
    require(entryPress.phase==retrieval::Phase::Armed,"fresh press on entry frame was discarded");
    require(retrieval::advance(entryPress,{true,true,true,outsideZone})==retrieval::Outcome::PullCommitted,"entry-frame grip did not pull");
    retrieval::State releaseHere{};
    (void)retrieval::advance(releaseHere,{true,true,true,insideZone});
    require(releaseHere.phase==retrieval::Phase::NeedOutsideNeutral,"stale held grip armed on entry");
    (void)retrieval::advance(releaseHere,{true,true,false,insideZone});
    require(releaseHere.phase==retrieval::Phase::InsideNeutral,"release inside did not allow a fresh grip");
    (void)retrieval::advance(releaseHere,{true,true,true,insideZone});
    require(releaseHere.phase==retrieval::Phase::Armed,"fresh grip after release inside failed");
    retrieval::State departed{};
    (void)retrieval::advance(departed,{true,true,false,insideZone});
    (void)retrieval::advance(departed,{true,true,true,outsideZone});
    require(departed.phase!=retrieval::Phase::Armed,"grip after leaving zone incorrectly armed retrieval");
    using namespace rock_backpack::ui;
    PointerPressGate press;
    require(!press.update(1, Hand::Left, true), "a held gameplay press cannot become a new panel click");
    require(!press.update(1, Hand::Left, false) && press.update(1, Hand::Left, true), "a neutral pointer may begin a click");
    require(!press.update(1, Hand::None, true) && !press.update(1, Hand::Left, true), "pointer loss requires another neutral sample");
    (void)press.update(1, Hand::Left, false);
    require(!press.update(2, Hand::Left, true), "reopening must reject an old pressed widget");
    (void)press.update(2, Hand::Left, false);
    require(!press.update(2, Hand::Right, true), "switching physical hands must not inherit a click");
    require(knownItemIcon(true, 0x23736) == Icon::Stimpak, "PALM recognizes the base-game stimpak");
    require(knownItemIcon(false, 0x23736) == Icon::Automatic, "mod-local IDs must not impersonate base-game icons");
    require(weaponIcon(Icon::Automatic, 9, [](unsigned id) { return id == 0x92A85 || id == 0x4A0A0; }) == Icon::PlasmaPistol,
        "instance pistol keywords select PALM's plasma pistol");
    require(weaponIcon(Icon::Automatic, 9, [](unsigned id) { return id == 0x92A85; }) == Icon::PlasmaRifle,
        "rifle conversion selects PALM's plasma rifle");
    require(armorIcon(1u << 4, false) == Icon::LeftArm && armorIcon(1u << 5, false) == Icon::RightArm,
        "PALM apparel preserves physical side");

    std::cout << "ROCK Backpack policy tests passed\n";
    return EXIT_SUCCESS;
}
