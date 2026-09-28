#include <components/rendercore/temporalinputcontract.hpp>
#include <iostream>
#include <stdexcept>
using namespace RenderCore::Temporal;
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main() try
{
    History history;FrameInput f;f.identity={1,1,1,1};f.render={1280,720};f.output={1920,1080};
    const auto frame=history.prepare(f);check(frame.has_value(),"fixture invalid");
    Inputs in;in.color={1,1,0,1,9,f.render,Format::Rgba16Float};in.depth={2,1,0,1,9,f.render,Format::Depth32Float};
    in.motion={3,1,0,1,9,f.render,Format::Rg16Float};in.output={4,1,0,1,9,f.output,Format::Rgba16Float};
    in.motionSpace=MotionSpace::CurrentToPreviousRenderPixels;in.colorExcludesHud=true;in.allowAutoExposure=true;
    check(validateInputs(*frame,in)==InputStatus::IncompleteMotion,"camera-only input advertised as full DLSS");
    in.dynamicMotionComplete=true;in.hdr=true;check(validateInputs(*frame,in)==InputStatus::Valid,"valid metadata rejected");
    auto bad=in;bad.motion.frame=99;check(validateInputs(*frame,bad)==InputStatus::MismatchedFrame,"stale motion");
    bad=in;bad.depth.view=2;check(validateInputs(*frame,bad)==InputStatus::MismatchedView,"wrong view");
    bad=in;bad.output.device=8;check(validateInputs(*frame,bad)==InputStatus::MismatchedDevice,"wrong device");
    bad=in;bad.output.extent=f.render;check(validateInputs(*frame,bad)==InputStatus::InvalidExtent,"wrong output extent");
    bad=in;bad.output.id=in.color.id;check(validateInputs(*frame,bad)==InputStatus::AliasedImages,"output input alias");
    bad=in;bad.motion.format=Format::Rgba8;check(validateInputs(*frame,bad)==InputStatus::InvalidFormat,"wrong motion format");
    bad=in;bad.color.format=Format::Rgba8;check(validateInputs(*frame,bad)==InputStatus::InvalidFormat,"HDR lost precision");
    bad=in;bad.colorExcludesHud=false;check(validateInputs(*frame,bad)==InputStatus::HudInColor,"HUD accepted");
    bad=in;bad.allowAutoExposure=false;check(validateInputs(*frame,bad)==InputStatus::MissingExposure,"no exposure policy");
    bad=in;bad.motionSpace=MotionSpace::Unknown;check(validateInputs(*frame,bad)==InputStatus::UnsupportedMotion,"unknown vector convention");
    bad=in;bad.depth.revision=0;check(validateInputs(*frame,bad)==InputStatus::MissingImage,"missing generation");
    std::cout<<"PASS: frame/view/device/extent/format identity, alias rejection, dense dynamic motion, HUD exclusion, exposure metadata gates\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
