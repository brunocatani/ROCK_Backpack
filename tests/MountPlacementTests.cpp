#include "BackpackConfig.h"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <limits>
#include <sstream>

using namespace rock_backpack;
void require(bool value,const char* message) { if(!value) { std::cerr<<message<<'\n'; std::exit(1); } }
std::string read(const std::filesystem::path& path) { std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}}; }
int main(int argc,char** argv)
{
    require(argc==2,"test output directory required");
    const auto directory=std::filesystem::absolute(argv[1])/"mount-placement";
    std::error_code ignored; std::filesystem::remove_all(directory,ignored); std::filesystem::create_directories(directory);
    const auto path=directory/"ROCK_Backpack.toml";
    const std::string original="[Panel]\n# Keep this note\nEnabled = true\nOffsetX = 0.0\nOffsetY = 2.0\nOffsetZ = 0.0\nPanelWidth = 3.5\nPanelHeight = 3.0\n[Unrelated]\nValue = 'keep me'\n";
    { std::ofstream file(path,std::ios::binary|std::ios::trunc); file<<original; }
    std::string error;
    auto settings=BackpackConfig::readOnly(error,path).value();
    require(settings.panelPhysicalWidth==44 && settings.panelPhysicalHeight==33 && settings.localOffset.z==10,"relative panel values did not resolve against calibration");
    require(std::abs(settings.mounts.positions[0].x-12.592592f)<.00001f && std::abs(settings.mounts.rotations[2].y+33.025402f)<.00001f,"captured weapon defaults changed");
    for(const auto slot:{storage::Location::LeftRifle,storage::Location::RightRifle,storage::Location::Pistol}) {
        const auto facing=zones::visualRotation(slot);
        const panel_math::Vec3 side{facing[0],facing[1],facing[2]},barrel{facing[3],facing[4],facing[5]},top{facing[6],facing[7],facing[8]};
        const bool pistol=slot==storage::Location::Pistol;
        require(pistol?barrel.x==-1 && barrel.y==0:barrel.x==0 && barrel.y==-1,"facing reversal changed muzzle direction");
        require(top.z==1,"rifles must stay reversed while the pistol keeps its original facing");
        if(pistol) require(side.x==0 && side.y==1 && barrel.x==-1,"pistol mount must keep its original basis");
        require(panel_math::dot(panel_math::cross(side,barrel),top)==1,"weapon facing mirrored geometry instead of rotating it");
    }
    zones::Layout layout; layout.positions={{{18,-4,11},{-19,-1,7.5f},{1,19,6}}};
    layout.rotations[1]={13,22,-31};
    const auto rifle=zones::visualRotation(storage::Location::RightRifle,layout,storage::Kind::LongGun);
    const auto pistol=zones::visualRotation(storage::Location::RightRifle,layout,storage::Kind::Pistol);
    for(unsigned column=0;column<3;++column) {
        require(std::abs(rifle[column]+pistol[column])<.00001f && std::abs(rifle[6+column]+pistol[6+column])<.00001f,
            "pistol in a shoulder mount must not inherit rifle inversion");
        require(std::abs(rifle[3+column]-pistol[3+column])<.00001f,"facing policy changed muzzle direction");
    }
    settings.mounts=layout;
    require(BackpackConfig::saveSettings(settings,error,path),"placement save failed");
    require(read(path).find("# Keep this note\nEnabled = true")!=std::string::npos && read(path).find("keep me")!=std::string::npos,
        "save lost the player's note or a foreign entry");
    const auto loaded=BackpackConfig::readOnly(error,path);
    require(loaded.has_value(),"saved settings could not load");
    require(loaded->panelPhysicalWidth==44 && loaded->localOffset.y==2,"panel settings changed while saving mounts");
    panel_math::StoredTransform root{{0,1,0,-1,0,0,0,0,1},{20,30,40},2};
    for(unsigned i=0;i<3;++i) {
        const auto location=storage::Location(i+1);
        const auto actual=zones::mount(location,loaded->mounts),expected=layout.positions[i];
        require(actual.x==expected.x && actual.y==expected.y && actual.z==expected.z,"saved mount coordinates differ");
        const auto guide=zones::guidancePose(root,location);
        const auto local=guide?zones::localPoint(root,guide->position):std::nullopt;
        require(local && zones::locate(*local,true,1u<<unsigned(location))==location,"fixed guide no longer targets its deposit region");
        const auto box=zones::depositRegion(location);
        const auto fixed=zones::mount(location);
        require(box.center.x==fixed.x && box.center.y==fixed.y && box.center.z==fixed.z,"weapon editing moved the deposit guide");
        const auto draw=zones::regions(false)[i];
        require(draw.center.x==fixed.x && draw.center.y==fixed.y && draw.center.z==fixed.z,"visual weapon editing moved its fixed grab region");

    }
    layout.positions[0].x=21;
    settings.mounts=layout;
    require(BackpackConfig::saveSettings(settings,error,path),"existing mount keys could not be updated");
    require(BackpackConfig::readOnly(error,path)->mounts.positions[0].x==21,"existing mount update ignored");
    const auto valid=read(path);
    layout.positions[1].z=std::numeric_limits<float>::quiet_NaN();
    settings.mounts=layout;
    require(!BackpackConfig::saveSettings(settings,error,path) && read(path)==valid,"invalid placement changed the file");
    settings=*loaded; settings.panelYawDegrees=18;
    require(BackpackConfig::saveSettings(settings,error,path),"angle and mounts save failed");
    const auto angled=BackpackConfig::readOnly(error,path);
    require(angled && angled->panelYawDegrees==18.f,"panel angle did not roundtrip");
    const auto rotated=read(path);
    require(rotated.find("keep me")!=std::string::npos && rotated.find("PanelWidth = 3.5\n")!=std::string::npos,"angle save lost unrelated settings");
    settings.panelYawDegrees=61;
    require(!BackpackConfig::saveSettings(settings,error,path) && read(path)==rotated,"invalid angle changed settings");
    const auto left=panel_math::anchoredInventoryPose(root,40,{},false,18);
    const auto right=panel_math::anchoredInventoryPose(root,40,{},true,18);
    const auto center=panel_math::computeBackpackPanelPose(root);
    require(left && right && center,"mirrored panel pose missing");
    require(std::abs(panel_math::dot(left->position-center->position,center->right)+panel_math::dot(right->position-center->position,center->right))<.001f,"panel sides are not mirrored");
    require(panel_math::dot(left->right,center->front)>0 && panel_math::dot(right->right,center->front)<0,"rotation must mirror with panel side");
    require(panel_math::dot(left->up,right->up)>.999f && panel_math::dot(left->front,right->front)>0,"mirrored panel text flipped");
    auto edited=*angled;
    edited.stash.centers[1]={25,12,9}; edited.stash.halves[1]={3,7,4};
    edited.mounts.rotations[0]={10,20,30};
    const auto fixedGuide=zones::guidancePose(root,storage::Location::LeftRifle);
    require(BackpackConfig::saveSettings(edited,error,path),"region/rotation save failed");
    const auto restored=BackpackConfig::readOnly(error,path);
    require(restored && restored->stash.centers[1].y==12 && restored->stash.halves[1].y==7 && restored->mounts.rotations[0].z==30,"region size, center or angle did not roundtrip");
    require(zones::locate({25,12,9},true,2,restored->stash)==storage::Location::LeftRifle,"edited region does not accept its center");
    require(!zones::locate({29,12,9},true,2,restored->stash),"edited region accepts outside its dimensions");
    const auto stillFixed=zones::guidancePose(root,storage::Location::LeftRifle);
    require(stillFixed->position.x==fixedGuide->position.x && stillFixed->position.y==fixedGuide->position.y,"stash edit moved its indicator");
    const auto pose=zones::visualRotation(storage::Location::LeftRifle,edited.mounts);
    for(unsigned row=0;row<3;++row) {
        const panel_math::Vec3 a{pose[row*3],pose[row*3+1],pose[row*3+2]};
        require(std::abs(panel_math::dot(a,a)-1.f)<.0001f,"weapon rotation changed scale");
    }
    panel_math::StoredTransform visual{pose,edited.mounts.positions[0],1};
    const auto pivot=panel_math::transformLocalPointToWorld(visual,{});
    require(pivot.x==edited.mounts.positions[0].x && pivot.y==edited.mounts.positions[0].y,"rotation moved the weapon pivot");
    edited.stash.halves[1].x=0;
    const auto good=read(path);
    require(!BackpackConfig::saveSettings(edited,error,path) && read(path)==good,"zero-size region was saved");
    // A table the mod does not read (the retired hip holster) neither breaks
    // loading nor saving, and its values stay in the file as comments.
    { std::ofstream(path,std::ios::binary|std::ios::app)<<"\n[LegHolster]\nEnabled = true\nRadius = 0.0\n"; }
    const auto retired=BackpackConfig::readOnly(error,path);
    require(retired && BackpackConfig::saveSettings(*retired,error,path),"retired LegHolster table broke loading or saving");
    require(read(path).find("Radius = 0.0")!=std::string::npos && read(path).find("keep me")!=std::string::npos,
        "save dropped entries it does not own");
    BackpackConfig defaults;
    require(defaults.snapshot().mounts.positions[2].x==BackpackSettings{}.mounts.positions[2].x,"unloaded runtime snapshot lost compiled placements");
    settings=BackpackSettings{};
    require(BackpackConfig::saveSettings(settings,error,path),"baseline save failed");
    const auto baseline=BackpackConfig::readOnly(error,path);
    require(baseline && baseline->buttonOffset.y==-12 && baseline->buttonWidth==10 && baseline->buttonHeight==3 && baseline->panelYawDegrees==45,"zero adjustments do not preserve the captured setup");
    std::istringstream lines(read(path)); std::string line;
    while(std::getline(lines,line))
        if(!line.empty() && line[0]!='#' && line[0]!='[' && line.find('=')!=std::string::npos && !line.starts_with("Enabled"))
            require(std::stof(line.substr(line.find('=')+1))==0,"baseline must save as zeros");
    { std::ofstream file(path,std::ios::binary|std::ios::trunc); file<<"[Panel]\nOffsetZ = 'ten'\n"; }
    require(!BackpackConfig::readOnly(error,path) && !BackpackConfig::saveSettings(settings,error,path) &&
        read(path)=="[Panel]\nOffsetZ = 'ten'\n","a mistyped value must refuse the file, never be saved over");

    // The first start migrates a format-2 INI's adjustments and deletes it; an
    // INI of retired absolute values is never read as adjustments.
    const auto upgrade=directory/"upgrade"; std::filesystem::create_directories(upgrade);
    { std::ofstream file(upgrade/"ROCKBackpack.ini",std::ios::binary); file<<"; Calibration format 2.\r\n[Panel]\r\niCalibrationVersion = 2\r\nbEnabled = false\r\n"
        "fOffsetZ = 1.500000\r\nfYawDegrees = 0.000000\r\n[Mount.Pistol]\r\nfX = 2.000000\r\n[Stash.Interior]\r\nfX = 0.000000\r\nfWidth = 4.000000\r\n"
        "[LegHolster]\r\nbEnabled = true\r\n"; }
    const auto migrated=BackpackConfig::readOnly(error,upgrade/"ROCK_Backpack.toml");
    require(migrated && !std::filesystem::exists(upgrade/"ROCKBackpack.ini"),"format-2 INI did not migrate or was kept");
    require(!migrated->panelEnabled && migrated->localOffset.z==11.5f && migrated->mounts.positions[2].x==BackpackSettings{}.mounts.positions[2].x+2 &&
        migrated->stash.halves[0].x==BackpackSettings{}.stash.halves[0].x+2,"migrated adjustments differ");
    const auto absolute=directory/"absolute"; std::filesystem::create_directories(absolute);
    { std::ofstream file(absolute/"ROCKBackpack.ini",std::ios::binary); file<<"[Panel]\r\nbEnabled=false\r\nfOffsetZ=10\r\n"; }
    const auto untouched=BackpackConfig::readOnly(error,absolute/"ROCK_Backpack.toml");
    require(untouched && untouched->panelEnabled && untouched->localOffset.z==10 && std::filesystem::exists(absolute/"ROCKBackpack.ini"),
        "absolute-format INI must not become adjustments");

    const BackpackSettings authored;
    const zones::StashLayout originalRegions;
    for(unsigned i=0;i<4;++i) {
        const auto actual=authored.stash.centers[i],center=originalRegions.centers[i];
        const auto half=authored.stash.halves[i],expected=originalRegions.halves[i];
        require(actual.x==center.x && actual.y==center.y && actual.z==center.z,"authored detection center changed");
        require(half.x==expected.x && half.y==expected.y && half.z==expected.z,"authored detection size changed");
        const auto guide=zones::depositRegion(storage::Location(i));
        require(guide.center.x==center.x && guide.center.y==center.y && guide.center.z==center.z &&
            guide.half.x==expected.x && guide.half.y==expected.y,"original visual marker dimensions/position changed");
    }
    std::filesystem::remove_all(directory,ignored);
}
