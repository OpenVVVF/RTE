#include "plant_backend.h"
#include "ode_plant.h"
#include "ngspice_plant.h"

#include <iostream>

namespace hostsim {

std::unique_ptr<IPlant> CreatePlantBackend(const std::string& type) {
    return CreatePlantBackend(type, MachineType::Pmsm);
}

std::unique_ptr<IPlant> CreatePlantBackend(const std::string& type,
                                           MachineType machine) {
    if (type == "ngspice" && machine == MachineType::Induction) {
        std::cerr << "HostSim: backend \"ngspice\" cannot model "
                     "machine \"induction\" (the ngspice netlists are "
                     "electrical RL / PMSM-backEMF only) — refusing to run "
                     "an induction machine there. Falling back to the ODE "
                     "plant, which models the induction machine natively.\n";
        return std::make_unique<OdePlant>();
    }
    if (type == "ngspice") {
        auto ng = std::make_unique<NgspicePlant>();
        if (ng->IsSharedspiceLoaded()) {
            return ng;
        }
        std::cerr << "HostSim: ngspice backend requested but libngspice.so "
                     "could not be loaded (or the experimental backend is not "
                     "yet functional). Falling back to OdePlant.\n";
        return std::make_unique<OdePlant>();
    }
    // Default and fallback: fast discrete PMSM ODE. This is the only
    // fully-supported plant backend today.
    return std::make_unique<OdePlant>();
}

} // namespace hostsim
