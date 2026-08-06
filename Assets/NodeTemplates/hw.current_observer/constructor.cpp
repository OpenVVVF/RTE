/* Seed the current observer with motor parameters from calibration.
 * The codegen FOC does not call FocControlManager::start(), so the observer
 * must be initialized here. */
platform_observer_init_from_calibration();
