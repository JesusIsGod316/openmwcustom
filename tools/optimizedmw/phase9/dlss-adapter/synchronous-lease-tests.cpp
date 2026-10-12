#include "../../../../components/rendercore/synchronousinputlease.hpp"
#include <iostream>
#include <stdexcept>
using namespace RenderCore::Temporal;
unsigned checks=0;
void require(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
int main() try
{
    SynchronousInputLease owner;
    SynchronousInputs inputs{1,0,1,17,4,{11,12,13},1280,720,true,true,true,true,false};
    const SynchronousRequest request{1,0,1,17,4,1280,720};
    require(!owner.acquire(request),"unpublished input borrowed");
    require(owner.publish(inputs),"first source publication");
    for(unsigned field=0;field<7;++field)
    {
        auto bad=request;
        switch(field){case 0:++bad.ticket;break;case 1:++bad.frame;break;case 2:++bad.view;break;
            case 3:++bad.context;break;case 4:++bad.generation;break;case 5:++bad.width;break;case 6:++bad.height;break;}
        require(!owner.acquire(bad),"mismatched source borrowed");
    }
    auto lease=owner.acquire(request); require(bool(lease),"complete exact-frame source refused");
    require(owner.inputs(*lease)->images==inputs.images,"borrowed images differ from publication");
    require(!owner.acquire(request),"duplicate borrow accepted");
    auto next=inputs; ++next.ticket;++next.frame;
    require(!owner.publish(next),"borrowed frame overwritten"); require(!owner.retire(),"borrowed generation retired");
    require(!owner.release({lease->serial+1}),"stale token released current borrow");
    require(owner.release(*lease),"current borrow refused release");require(!owner.release(*lease),"duplicate release accepted");
    const auto repeated=owner.acquire(request);require(bool(repeated),"same-frame later import refused");
    require(!owner.release(*lease),"released token releases a later same-frame borrow");
    require(owner.release(*repeated),"same-frame later borrow release");
    require(owner.publish(next),"next source publication");
    auto second=owner.acquire({2,1,1,17,4,1280,720});require(bool(second),"next source not borrowed");
    require(!owner.accepts(*lease),"old token matches new frame");require(owner.release(*second),"next borrow release");
    require(owner.retire() && !owner.acquire({2,1,1,17,4,1280,720}),"retired frame borrowed");
    for(unsigned flag=0;flag<3;++flag)
    {
        auto incomplete=inputs;
        if(flag==0)incomplete.motionComplete=false;else if(flag==1)incomplete.colorBeforeHud=false;
        else incomplete.motionInRenderPixels=false;
        require(owner.publish(incomplete),"incomplete producer metadata publication");
        require(!owner.acquire(request),"unsafe incomplete reconstruction contract borrowed");
        require(owner.retire(),"incomplete publication retirement");
    }
    auto aliased=inputs;aliased.images[1]=aliased.images[0];require(!owner.publish(aliased),"aliased source images published");
    auto unsubmitted=inputs;unsubmitted.producerSubmitted=false;require(!owner.publish(unsubmitted),"unsubmitted source published");
    std::cout<<"Synchronous source lease checks passed: "<<checks<<'\n';return 0;
}
catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
