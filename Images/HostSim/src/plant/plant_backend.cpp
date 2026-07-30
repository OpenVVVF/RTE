#include "plant_backend.h"
#include "ode_plant.h"
#include "ngspice_plant.h"

#include <iostream>

namespace hostsim {

std::unique_ptr<IPlant> CreatePlantBackend(const std::string& type) {
    if (type == "ngspice") {
        auto ng = std::make_unique<NgspicePlant>();
        if (ng->IsSharedspiceLoaded()) {
            return ng;
        }
        std::cerr << "HostSim: ngspice shared library not available; "
                     "falling back to OdePlant.\n";
        return std::make_unique<OdePlant>();
    }
    return std::make_unique<OdePlant>();
}

} // namespace hostsim
