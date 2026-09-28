#include <components/rendercore/temporalframe.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
using namespace RenderCore::Temporal;
void check(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
bool near(glm::dvec2 a, glm::dvec2 b) { return glm::length(a-b)<1e-8; }
FrameInput base()
{
    FrameInput f; f.identity={1,1,1,1}; f.frame=10;
    f.render={1280,720}; f.output={1920,1080};
    f.projection=glm::perspective(glm::radians(65.0),1280.0/720.0,1.0,10000.0);
    return f;
}
int main() try
{
    History h; auto in=base(); auto f=h.prepare(in);
    check(f && !f->hasHistory() && (f->resetReasons & FirstFrame),"first frame must reset");
    check(!h.prepare(in),"two outstanding evaluations allowed");
    check(!h.commit(f->ticket+1),"wrong ticket committed");
    check(h.commit(f->ticket),"first commit failed");
    check(!h.prepare(in),"same frame evaluated twice");
    ++in.frame; in.view=glm::translate(glm::dmat4(1),glm::dvec3(-.1,.2,0));
    auto moved=h.prepare(in); check(moved && moved->hasHistory(),"normal camera move lost history");
    const glm::dvec3 point(2,1,-10);
    const auto vector=worldMotion(*moved,point,point); check(vector.has_value(),"camera vector absent");
    const glm::dvec4 jittered=moved->jitteredProjection*in.view*glm::dvec4(point,1);
    const auto sample=pixelPosition(jittered,in.render);
    const double depth=jittered.z/jittered.w*.5+.5;
    const auto reconstructed=staticMotionFromDepth(*moved,*sample,depth);
    check(reconstructed && near(*reconstructed,*vector),"camera depth reconstruction differs from geometry");
    check(vector->x>0 && vector->y>0,"current-to-previous/top-left vector sign wrong");
    auto savedJitter=moved->jitterPixels; const auto stale=moved->ticket;
    check(h.abort(stale),"abort failed"); moved=h.prepare(in);
    check(moved && near(moved->jitterPixels,savedJitter),"aborted frame advanced sampling");
    check(!h.commit(stale) && h.commit(moved->ticket),"stale ticket accepted");
    ++in.frame; f=h.prepare(in); check(f && f->hasHistory(),"static history missing");
    check(near(*worldMotion(*f,point,point),glm::dvec2(0)),"jitter leaked into motion");
    check(!worldMotion(*f,{0,0,1},{0,0,1}),"behind-camera point accepted");
    check(!staticMotionFromDepth(*f,{10,10},2),"invalid window depth accepted");
    h.commit(f->ticket);
    in.frame+=2;f=h.prepare(in);check(f && (f->resetReasons&FrameGap),"skipped frame used stale history");h.commit(f->ticket);
    ++in.frame; ++in.identity.worldEpoch;f=h.prepare(in);check(f&&(f->resetReasons&WorldChange),"world reused history");h.commit(f->ticket);
    ++in.frame; ++in.identity.cameraEpoch;f=h.prepare(in);check(f&&(f->resetReasons&CameraChange),"camera mode reused history");h.commit(f->ticket);
    ++in.frame;in.render={960,540};f=h.prepare(in);check(f&&(f->resetReasons&ExtentChange),"resized buffer reused history");h.commit(f->ticket);
    ++in.frame;in.projection=glm::dmat4(0);check(!h.prepare(in),"singular projection accepted");
    in.projection=base().projection;f=h.prepare(in);check(f&&(f->resetReasons&InvalidPreviousInput),"invalid-input reset lost");h.commit(f->ticket);
    ++in.frame;in.view[0][0]=std::numeric_limits<double>::quiet_NaN();check(!h.prepare(in),"NaN matrix accepted");
    in.view=glm::dmat4(1);in.cameraCut=true;f=h.prepare(in);check(f&&(f->resetReasons&ExplicitCut),"camera cut ignored");h.commit(f->ticket);
    glm::dmat4 matrix{1};for(int c=0;c<4;++c)for(int r=0;r<4;++r)matrix[c][r]=c*10+r;
    const auto rows=rowMajor(matrix);check(rows[1]==10 && rows[4]==1 && rows[15]==33,"row-major packing transposed twice");
    History reverse;in=base();in.jitterEnabled=false;
    // Flip clip Z without changing W: reverse-Z depth must not be inverted twice.
    for(int c=0;c<4;++c) in.projection[c][2]*=-1;
    f=reverse.prepare(in);reverse.commit(f->ticket);++in.frame;in.view=glm::translate(glm::dmat4(1),glm::dvec3(-.4,0,0));
    f=reverse.prepare(in);const auto clip=f->jitteredProjection*in.view*glm::dvec4(point,1);
    const auto reverseMv=staticMotionFromDepth(*f,*pixelPosition(clip,in.render),clip.z/clip.w*.5+.5);
    check(reverseMv&&near(*reverseMv,*worldMotion(*f,point,point)),"reverse Z mismatch");
    for(unsigned i=1;i<=1024;++i)check(halton(i,2)>=0 && halton(i,2)<1 && halton(i,3)<1,"Halton range");
    std::cout<<"PASS: transactional history, stale tickets, jitter/depth/camera flow, reset classes, invalid matrices, reverse Z, row-major packing, Halton\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
