#include "sil_world.h"

SilWorld& silWorld() {
    static SilWorld w;
    return w;
}
