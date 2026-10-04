#include "apps/neo_editor/state/app_state.h"
#include <iostream>
namespace app { void requestUpdate() {} }
int main() {
    neo::AppState state;int calls=0;
    state.doc.text="unchanged";state.path="sample.py";
    neo::requestRisk(state,"Reset?","Description","Reset",[&]{++calls;});
    if(calls || !state.riskCallback || state.riskAction==neo::AppState::RiskAction::None) return 1;
    neo::cancelRisk(state);neo::confirmRisk(state);
    if(calls || state.riskCallback || state.doc.text!="unchanged") return 2;
    neo::requestRisk(state,"Reset?","Description","Reset",[&]{++calls;});
    neo::confirmRisk(state);neo::confirmRisk(state);
    if(calls!=1 || state.riskCallback || state.riskAction!=neo::AppState::RiskAction::None) return 3;
    neo::requestRisk(state,"Old","Description","Reset",[&]{calls+=10;});
    neo::requestRisk(state,"New","Description","Reset",[&]{++calls;});
    neo::confirmRisk(state);
    if(calls!=2 || state.path!="sample.py" || state.doc.text!="unchanged") return 4;
    std::cout<<"settings confirmation deferred/cancel/one-shot checks passed\n";
}
