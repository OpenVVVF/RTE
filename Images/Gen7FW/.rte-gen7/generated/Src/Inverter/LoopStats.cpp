#include "Inverter/LoopStats.h"

namespace Inverter {
namespace LoopStats {

volatile uint32_t app_loop = 0;
volatile uint32_t vsense = 0;
volatile uint32_t tim_isr = 0;
volatile uint32_t adc_isr = 0;

} // namespace LoopStats
} // namespace Inverter
